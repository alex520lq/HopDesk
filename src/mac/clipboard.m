/* =====================================================================
 *  HopDesk - macOS 剪贴板同步（ObjC 薄层）
 *
 *  NSPasteboard 没有可靠的"内容变更"回调，只有 changeCount。
 *  用主队列上的 250ms 定时器轮询 changeCount：变化了才读内容。
 *
 *  发送：
 *    fileURL（可多个，含目录）  -> 数据通道，目录递归
 *    TIFF/PNG 图片              -> 转成自上而下 BGRA，数据通道
 *    字符串                     -> 控制通道（文本都很小）
 *
 *  为什么文件和图片不走控制通道：控制通道的队列是给鼠标键盘用的，
 *  塞一张 4K 截图进去，后面几十毫秒的鼠标事件全被堵住。
 *
 *  回环抑制：写完对端内容后把新的 changeCount 记到 s_last_change，
 *  轮询看到相同值就跳过，不会把刚收到的内容再发回去。
 *
 *  线程：轮询与写入都在主队列，s_last_change 无竞态。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>

#include "mac_app.h"
#include "xfer.h"

#include <stdlib.h>
#include <string.h>

static NSInteger         s_last_change;
static dispatch_source_t s_timer;

/* 单张图片上限：4K 截图裸 BGRA 约 33 MB，留到 256 MB 足够 */
#define CLIP_IMG_MAX_BYTES (256ull * 1024ull * 1024ull)

/* ---- 发送 ---- */

static void send_paths(NSArray<NSURL *> *urls) {
    NSUInteger n = [urls count];
    if (n == 0 || n > 256) return;

    const char **paths = (const char **)calloc(n, sizeof(char *));
    if (!paths) return;

    int k = 0;
    for (NSURL *u in urls) {
        const char *p = [[u path] fileSystemRepresentation];
        if (p && *p) paths[k++] = p;
    }

    if (k > 0) {
        hop_log(HOP_LOG_INFO, "clip: %d path(s) out (first: %s)", k, paths[0]);
        if (!xfer_submit_paths(paths, k))
            hop_log(HOP_LOG_WARN, "clip: data channel unavailable, paths dropped");
    }
    free(paths);
}

/* NSImage -> 自上而下 BGRA。用 CGBitmapContext 明确指定字节序，
 * 不依赖 NSBitmapImageRep 的默认通道顺序。 */
static void send_image(NSImage *img) {
    if (!img) return;

    /* 必须用 CGImage 的像素尺寸，不能用 [img size]（那是"点"）。
     * Retina 截图在点上只有一半，按点取会掉一半分辨率。 */
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return;

    int w = (int)CGImageGetWidth(cg);
    int h = (int)CGImageGetHeight(cg);
    if (w <= 0 || h <= 0) return;

    size_t stride = (size_t)w * 4;
    size_t total = stride * (size_t)h;
    if (total > CLIP_IMG_MAX_BYTES) {
        hop_log(HOP_LOG_WARN, "clip: image %.1f MB too large, skipped",
                (double)total / 1048576.0);
        return;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* ByteOrder32Little + AlphaNoneSkipFirst => 内存里是 B,G,R,X */
    CGContextRef ctx = CGBitmapContextCreate(NULL, (size_t)w, (size_t)h, 8, stride, cs,
                                            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGColorSpaceRelease(cs);
    if (!ctx) return;

    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    void *data = CGBitmapContextGetData(ctx);
    if (!data) { CGContextRelease(ctx); return; }

    /* CGContext 原点在左下，CGBitmapContext 的行序是自上而下存的，
     * 但 draw 进来之后第 0 行对应图像底部 —— 需要翻转成自上而下。 */
    uint8_t *flip = (uint8_t *)malloc(total);
    if (!flip) { CGContextRelease(ctx); return; }
    for (int y = 0; y < h; ++y) {
        memcpy(flip + (size_t)y * stride,
               (uint8_t *)data + (size_t)(h - 1 - y) * stride, stride);
    }

    hop_log(HOP_LOG_INFO, "clip: image out %dx%d", w, h);
    xfer_submit_image(flip, w, h, (int)stride);

    free(flip);
    CGContextRelease(ctx);
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

        if (cc == s_last_change) return;

        /* 文件优先：Finder 里 Cmd+C 一个文件，剪贴板上是 fileURL，
         * 只读字符串的话会"什么都不发生" */
        NSArray<NSURL *> *urls =
            [pb readObjectsForClasses:@[ [NSURL class] ]
                              options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
        if ([urls count] > 0) {
            send_paths(urls);
            /* xfer_submit_paths copies/queues synchronously; only then consume this change. */
            s_last_change = cc;
            return;
        }

        /* 图片：TIFF 覆盖最广（截图、浏览器、预览都是它） */
        if ([pb availableTypeFromArray:@[ NSPasteboardTypeTIFF, NSPasteboardTypePNG ]] != nil) {
            NSImage *img = [[NSImage alloc] initWithPasteboard:pb];
            if (img) {
                send_image(img);
                s_last_change = cc;
                return;
            }
        }

        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s) {
            const char *utf8 = [s UTF8String];
            uint32_t n = utf8 ? (uint32_t)strlen(utf8) : 0;
            if (n > 0 && n <= HOP_TEXT_MAX) {
                send_text(s);
                /* Keep the change pending while disconnected; poll retries it. */
                if (g_mac.connected) s_last_change = cc;
                return;
            }
        }

        /* Unsupported/empty clipboard formats are intentionally consumed. */
        s_last_change = cc;
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

/* ---- 接收（都在主队列执行） ---- */

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

/* 一批文件/目录落盘完毕：把顶层条目挂到剪贴板，Finder 里 Cmd+V 即复制 */
void clipboard_apply_paths(const char *const *paths, int n) {
    if (!paths || n <= 0) return;

    NSMutableArray<NSString *> *strs = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
    for (int i = 0; i < n; ++i) {
        NSString *p = [NSString stringWithUTF8String:paths[i]];
        if (p) [strs addObject:p];
    }
    if ([strs count] == 0) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:[strs count]];
        for (NSString *p in strs) {
            NSURL *u = [NSURL fileURLWithPath:p];
            hop_log(HOP_LOG_INFO, "clip: url(%s) -> %s",
                    [p UTF8String], u ? "ok" : "NULL");
            if (u) [urls addObject:u];
        }

        [pb clearContents];
        BOOL ok = [pb writeObjects:urls];
        s_last_change = [pb changeCount];   /* 抑制回传 */
        hop_log(HOP_LOG_INFO, "clip: %lu path(s) in, ok=%d types=%lu",
                (unsigned long)[urls count], (int)ok,
                (unsigned long)[[pb types] count]);
    });
}

/* 收到图片：裸 BGRA 自上而下 -> NSImage -> 剪贴板 */
void clipboard_apply_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return;

    size_t bytes = (size_t)stride * (size_t)h;
    NSData *copy = [NSData dataWithBytes:bgra length:bytes];

    dispatch_async(dispatch_get_main_queue(), ^{
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef prov =
            CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
        if (!prov) { CGColorSpaceRelease(cs); return; }

        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)stride, cs,
                                       kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                       prov, NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(prov);
        CGColorSpaceRelease(cs);
        if (!img) return;

        NSImage *ns = [[NSImage alloc] initWithCGImage:img
                                                  size:NSMakeSize(w, h)];
        CGImageRelease(img);

        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb writeObjects:@[ ns ]];
        s_last_change = [pb changeCount];   /* 抑制回传 */
        hop_log(HOP_LOG_INFO, "clip: image in %dx%d", w, h);
    });
}
