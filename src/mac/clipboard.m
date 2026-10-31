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

static BOOL send_paths(NSArray<NSURL *> *urls) {
    NSUInteger n = [urls count];
    if (n == 0 || n > 256) return NO;

    const char **paths = (const char **)calloc(n, sizeof(char *));
    if (!paths) return NO;

    int k = 0;
    for (NSURL *u in urls) {
        const char *p = [[u path] fileSystemRepresentation];
        if (p && *p) paths[k++] = p;
    }

    BOOL ok = NO;
    if (k > 0) {
        hop_log(HOP_LOG_INFO, "clip: %d path(s) out (first: %s)", k, paths[0]);
        ok = xfer_submit_paths(paths, k) ? YES : NO;
        if (!ok) hop_log(HOP_LOG_WARN, "clip: paths not queued; will retry");
    }
    free(paths);
    return ok;
}

/* NSImage -> 自上而下 BGRA。用 CGBitmapContext 明确指定字节序，
 * 不依赖 NSBitmapImageRep 的默认通道顺序。 */
static BOOL send_image(NSImage *img) {
    if (!img) return NO;

    /* 必须用 CGImage 的像素尺寸，不能用 [img size]（那是"点"）。
     * Retina 截图在点上只有一半，按点取会掉一半分辨率。 */
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return NO;

    int w = (int)CGImageGetWidth(cg);
    int h = (int)CGImageGetHeight(cg);
    if (w <= 0 || h <= 0) return NO;

    size_t stride = (size_t)w * 4;
    size_t total = stride * (size_t)h;
    if (total > CLIP_IMG_MAX_BYTES) {
        hop_log(HOP_LOG_WARN, "clip: image %.1f MB too large, skipped",
                (double)total / 1048576.0);
        return NO;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* ByteOrder32Little + AlphaNoneSkipFirst => 内存里是 B,G,R,X */
    CGContextRef ctx = CGBitmapContextCreate(NULL, (size_t)w, (size_t)h, 8, stride, cs,
                                            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGColorSpaceRelease(cs);
    if (!ctx) return NO;

    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    void *data = CGBitmapContextGetData(ctx);
    if (!data) { CGContextRelease(ctx); return NO; }

    /* CGContext 原点在左下，CGBitmapContext 的行序是自上而下存的，
     * 但 draw 进来之后第 0 行对应图像底部 —— 需要翻转成自上而下。 */
    uint8_t *flip = (uint8_t *)malloc(total);
    if (!flip) { CGContextRelease(ctx); return NO; }
    for (int y = 0; y < h; ++y) {
        memcpy(flip + (size_t)y * stride,
               (uint8_t *)data + (size_t)(h - 1 - y) * stride, stride);
    }

    hop_log(HOP_LOG_INFO, "clip: image out %dx%d", w, h);
    BOOL ok = xfer_submit_image(flip, w, h, (int)stride) ? YES : NO;
    free(flip);
    CGContextRelease(ctx);
    if (!ok) hop_log(HOP_LOG_WARN, "clip: image not queued; will retry");
    return ok;
}

static BOOL send_text(NSString *s) {
    const char *u = [s UTF8String];
    uint32_t n;
    uint8_t *p;

    if (!u) return NO;
    n = (uint32_t)strlen(u);
    if (n == 0 || n > HOP_TEXT_MAX) return NO;

    p = (uint8_t *)malloc(4 + n);
    if (!p) return NO;
    hop_put_be32(p, HOP_DEV_CLIENT);
    memcpy(p + 4, u, n);
    BOOL ok = net_send(HOP_MSG_CLIP_TEXT, p, 4 + n) ? YES : NO;
    if (ok) hop_log(HOP_LOG_INFO, "clip: text out %u bytes", n);
    else hop_log(HOP_LOG_WARN, "clip: text out failed; will retry");
    free(p);
    return ok;
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
            if (send_paths(urls)) s_last_change = cc;
            return;
        }

        /* 图片：TIFF 覆盖最广（截图、浏览器、预览都是它） */
        if ([pb availableTypeFromArray:@[ NSPasteboardTypeTIFF, NSPasteboardTypePNG ]] != nil) {
            NSImage *img = [[NSImage alloc] initWithPasteboard:pb];
            if (img) {
                if (send_image(img)) s_last_change = cc;
                return;
            }
        }

        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s) {
            const char *utf8 = [s UTF8String];
            uint32_t n = utf8 ? (uint32_t)strlen(utf8) : 0;
            if (n > 0 && n <= HOP_TEXT_MAX) {
                if (send_text(s)) s_last_change = cc;
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

/* Retry transient pasteboard write failures, but never overwrite a newer local
 * clipboard change while waiting. expected_count guards delayed retries. */
static void apply_objects_retry(NSArray *objects, NSString *kind,
                                NSUInteger attempt, NSInteger expected_count) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            apply_objects_retry(objects, kind, attempt, expected_count);
        });
        return;
    }

    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (attempt > 0 && [pb changeCount] != expected_count) {
        hop_log(HOP_LOG_INFO, "clip: %s apply retry cancelled; clipboard changed locally",
                [kind UTF8String]);
        return;
    }

    [pb clearContents];
    s_last_change = [pb changeCount]; /* suppress the clear notification */
    BOOL ok = [pb writeObjects:objects];
    if (ok) {
        s_last_change = [pb changeCount];
        hop_log(HOP_LOG_INFO, "clip: %s in; write succeeded on attempt %lu",
                [kind UTF8String], (unsigned long)(attempt + 1));
        return;
    }

    if (attempt < 10) {
        NSInteger expected = [pb changeCount];
        hop_log(HOP_LOG_WARN, "clip: %s pasteboard write failed; retry %lu/10",
                [kind UTF8String], (unsigned long)(attempt + 1));
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150ull * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            apply_objects_retry(objects, kind, attempt + 1, expected);
        });
    } else {
        hop_log(HOP_LOG_ERROR, "clip: %s pasteboard write failed after retries",
                [kind UTF8String]);
    }
}

void clipboard_apply(const char *utf8) {
    NSString *s;

    if (!utf8 || !*utf8) return;
    s = [NSString stringWithUTF8String:utf8];
    if (!s) return;

    apply_objects_retry(@[ s ], @"text", 0, -1);
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

    NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:[strs count]];
    for (NSString *p in strs) {
        NSURL *u = [NSURL fileURLWithPath:p];
        if (u) [urls addObject:u];
    }
    if ([urls count] > 0) apply_objects_retry(urls, @"paths", 0, -1);
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

        apply_objects_retry(@[ ns ], @"image", 0, -1);
    });
}
