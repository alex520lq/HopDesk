/* =====================================================================
 *  HopDesk - macOS 剪贴板同步（阶段 3：文本 + 单文件，ObjC 薄层）
 *
 *  NSPasteboard 没有可靠的"内容变更"回调，只有 changeCount。
 *  用主队列上的 250ms 定时器轮询 changeCount：变化了才读内容。
 *  250ms 对"复制完立刻去另一台粘贴"够用，且空闲时几乎零开销。
 *
 *  文件优先于文本：Finder 里 Cmd+C 一个文件，剪贴板上是 fileURL，
 *  只读字符串的话会"什么都不发生"。
 *
 *  回环抑制：写完对端内容后把新的 changeCount 记到 s_last_change，
 *  轮询看到相同值就跳过，不会把刚收到的内容再发回去。
 *
 *  线程：轮询与写入都在主队列，s_last_change 无竞态。
 *        只有 net_send 从主队列调，net_client 的锁保证安全。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>

#include "mac_app.h"

#include <stdlib.h>
#include <string.h>

static NSInteger         s_last_change;
static dispatch_source_t s_timer;

/* payload = origin(4) + nlen(2) + name + size(4) + data */
static void send_file(NSURL *url) {
    NSString *name = [url lastPathComponent];
    const char *n;
    NSData *data;
    uint32_t nlen;
    uint8_t *p;
    size_t total;

    if (!name) return;
    n = [name UTF8String];
    if (!n) return;
    nlen = (uint32_t)strlen(n);
    if (nlen == 0 || nlen > HOP_NAME_LEN_MAX) {
        hop_log(HOP_LOG_WARN, "clip: file name length out of range");
        return;
    }

    data = [NSData dataWithContentsOfURL:url];
    if (!data) { hop_log(HOP_LOG_WARN, "clip: cannot read file"); return; }
    if ([data length] == 0 || [data length] > HOP_FILE_MAX) {
        hop_log(HOP_LOG_WARN, "clip: file %lu bytes out of range, skipped",
                (unsigned long)[data length]);
        return;
    }

    total = 4 + 2 + nlen + 4 + (size_t)[data length];
    p = (uint8_t *)malloc(total);
    if (!p) return;

    hop_put_be32(p, HOP_DEV_CLIENT);
    hop_put_be16(p + 4, (uint16_t)nlen);
    memcpy(p + 6, n, nlen);
    hop_put_be32(p + 6 + nlen, (uint32_t)[data length]);
    memcpy(p + 10 + nlen, [data bytes], (size_t)[data length]);

    if (net_send(HOP_MSG_CLIP_FILE, p, (uint32_t)total))
        hop_log(HOP_LOG_INFO, "clip: file out %s (%lu bytes)", n,
                (unsigned long)[data length]);
    else
        hop_log(HOP_LOG_WARN, "clip: file out failed (not connected)");
    free(p);
}

static void send_text(NSString *s) {
    const char *u = [s UTF8String];
    uint32_t n;
    uint8_t *p;

    if (!u) return;
    n = (uint32_t)strlen(u);
    if (n == 0 || n > HOP_TEXT_MAX) return;

    p = (uint8_t *)malloc(4 + n);
    if (!p) return;
    hop_put_be32(p, HOP_DEV_CLIENT);
    memcpy(p + 4, u, n);
    if (net_send(HOP_MSG_CLIP_TEXT, p, 4 + n))
        hop_log(HOP_LOG_INFO, "clip: text out %u bytes", n);
    else
        hop_log(HOP_LOG_WARN, "clip: text out failed (not connected)");
    free(p);
}

static void poll_pasteboard(void) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSInteger cc = [pb changeCount];
        NSArray<NSURL *> *urls;

        if (cc == s_last_change) return;
        s_last_change = cc;

        /* 文件优先 */
        urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                 options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
        if ([urls count] > 0) {
            if ([urls count] > 1)
                hop_log(HOP_LOG_WARN, "clip: %lu files copied, only the first is sent",
                        (unsigned long)[urls count]);
            send_file(urls[0]);
            return;
        }

        {
            NSString *s = [pb stringForType:NSPasteboardTypeString];
            if (s) send_text(s);
        }
    }
}

void clipboard_init(void) {
    s_last_change = [[NSPasteboard generalPasteboard] changeCount];

    s_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                     dispatch_get_main_queue());
    if (!s_timer) return;
    dispatch_source_set_timer(s_timer,
                              dispatch_time(DISPATCH_TIME_NOW, 0),
                              250ull * NSEC_PER_MSEC,
                              50ull * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(s_timer, ^{
        poll_pasteboard();
    });
    dispatch_resume(s_timer);
}

/* 收到对端文本：只在主队列动 NSPasteboard */
void clipboard_apply(const char *utf8) {
    NSString *s;

    if (!utf8 || !*utf8) return;
    s = [NSString stringWithUTF8String:utf8];
    if (!s) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:s forType:NSPasteboardTypeString];
        s_last_change = [pb changeCount];   /* 抑制回传 */
        hop_log(HOP_LOG_INFO, "clip: text in %lu bytes", (unsigned long)[s length]);
    });
}

/* 收到对端文件：落到临时目录，把 fileURL 挂到剪贴板，Finder 里 Cmd+V 即复制 */
void clipboard_apply_file(const char *name, const uint8_t *data, uint32_t size) {
    NSString *n;
    NSData *d;

    if (!name || !*name || size == 0 || size > HOP_FILE_MAX) return;

    n = [NSString stringWithUTF8String:name];
    if (!n) return;
    /* 文件名里出现路径分隔符就是越界写入的信号，直接拒绝 */
    if ([n containsString:@"/"] || [n containsString:@".."]) {
        hop_log(HOP_LOG_WARN, "clip: unsafe file name rejected");
        return;
    }
    d = [NSData dataWithBytes:data length:size];

    dispatch_async(dispatch_get_main_queue(), ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"HopDesk"];
        NSString *path;
        NSPasteboard *pb;

        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES
                       attributes:nil error:NULL];
        path = [dir stringByAppendingPathComponent:n];
        if (![d writeToFile:path atomically:YES]) {
            hop_log(HOP_LOG_WARN, "clip: failed to write received file");
            return;
        }

        pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb writeObjects:@[ [NSURL fileURLWithPath:path] ]];
        s_last_change = [pb changeCount];   /* 抑制回传 */
        hop_log(HOP_LOG_INFO, "clip: file in %s (%u bytes)", name, size);
    });
}
