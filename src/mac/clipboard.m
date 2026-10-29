/* =====================================================================
 *  HopDesk - macOS 剪贴板同步（阶段 3，仅文本，ObjC 薄层）
 *
 *  NSPasteboard 没有可靠的"内容变更"回调，只有 changeCount。
 *  用主队列上的 250ms 定时器轮询 changeCount：变化了才读字符串。
 *  250ms 对"复制完立刻去另一台粘贴"够用，且空闲时几乎零开销。
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

static void poll_pasteboard(void) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSInteger cc = [pb changeCount];
        NSString *s;
        const char *u;
        uint32_t n;
        uint8_t *p;

        if (cc == s_last_change) return;
        s_last_change = cc;

        s = [pb stringForType:NSPasteboardTypeString];
        if (!s) return;                       /* 非文本内容：阶段 3 不管 */
        u = [s UTF8String];
        if (!u) return;
        n = (uint32_t)strlen(u);
        if (n == 0 || n > HOP_TEXT_MAX) return;

        p = (uint8_t *)malloc(4 + n);
        if (!p) return;
        hop_put_be32(p, HOP_DEV_CLIENT);
        memcpy(p + 4, u, n);
        net_send(HOP_MSG_CLIP_TEXT, p, 4 + n);
        free(p);
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
    });
}
