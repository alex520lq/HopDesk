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
#include "../core/xfer_task.h"

#include <stdlib.h>
#include <limits.h>
#include <string.h>

static NSInteger         s_last_change;
static dispatch_source_t s_timer;

/* 单张图片上限：4K 截图裸 BGRA 约 33 MB，留到 256 MB 足够 */
#define CLIP_IMG_MAX_BYTES HOP_XFER_DEFAULT_MAX_IMAGE_BYTES

#include <pthread.h>

static pthread_mutex_t s_offer_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_group_t s_paste_group;
static int s_paste_scan_active;
static int s_image_work_active;
static int s_clip_stopping;
static char *s_local_offer_paths[HOP_CLIP_OFFER_MAX_ITEMS];
static int s_local_offer_count;
static uint64_t s_local_offer_generation, s_local_offer_sequence;
static uint32_t s_offer_request_seq;
static uint32_t s_remote_offer_origin;
static uint64_t s_remote_offer_generation;
static uint32_t s_pending_paste_request;
static NSInteger s_pending_paste_change;
static uint32_t s_seen_paste_requests[32];
static unsigned s_seen_paste_next;

static uint32_t clip_local_device(void) {
    return g_mac.cfg.role == HOP_ROLE_SERVER ? HOP_DEV_SERVER : HOP_DEV_CLIENT;
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation);
static void clip_clear_remote_offer_local(void) {
    pthread_mutex_lock(&s_offer_lock);
    s_remote_offer_origin=0; s_remote_offer_generation=0;
    s_pending_paste_request=0; s_pending_paste_change=0;
    pthread_mutex_unlock(&s_offer_lock);
}
static void clip_invalidate_remote_offer(int notify_peer) {
    uint32_t origin=0;uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    origin=s_remote_offer_origin;generation=s_remote_offer_generation;
    s_remote_offer_origin=0;s_remote_offer_generation=0;
    s_pending_paste_request=0;s_pending_paste_change=0;
    pthread_mutex_unlock(&s_offer_lock);
    if(notify_peer&&origin&&generation)clip_send_offer_clear(origin,generation);
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation) {
    uint8_t p[HOP_CLIP_OFFER_CLEAR_BYTES];
    if(!origin||!generation)return;
    hop_put_be32(p,origin);hop_put_be64(p+4,generation);
    (void)net_send(HOP_MSG_CLIP_FILE_OFFER_CLEAR,p,sizeof(p));
}
static void clip_clear_local_offer(int notify_peer) {
    uint64_t generation;
    pthread_mutex_lock(&s_offer_lock);
    generation=s_local_offer_generation;
    for(int i=0;i<s_local_offer_count;++i)free(s_local_offer_paths[i]);
    memset(s_local_offer_paths,0,sizeof(s_local_offer_paths));
    s_local_offer_count=0;s_local_offer_generation=0;
    pthread_mutex_unlock(&s_offer_lock);
    if(notify_peer&&generation)clip_send_offer_clear(clip_local_device(),generation);
}
static int clip_send_offer(const char *const *paths,int count,uint64_t generation) {
    char *names[HOP_CLIP_OFFER_MAX_ITEMS]={0};
    uint8_t *payload=NULL;uint32_t len=0;int ok=0;
    if(!paths||count<=0||count>(int)HOP_CLIP_OFFER_MAX_ITEMS)return 0;
    for(int i=0;i<count;++i){
        const char *base=strrchr(paths[i],'/');base=base?base+1:paths[i];
        size_t n=strlen(base);
        if(!n||n>HOP_NAME_LEN_MAX)goto done;
        names[i]=strdup(base);if(!names[i])goto done;
    }
    payload=(uint8_t*)malloc(HOP_CLIP_OFFER_MAX_PAYLOAD);if(!payload)goto done;
    len=hop_clip_file_offer_encode(payload,HOP_CLIP_OFFER_MAX_PAYLOAD,
        clip_local_device(),generation,(const char *const *)names,count);
    if(len)ok=net_send(HOP_MSG_CLIP_FILE_OFFER,payload,len);
done:
    for(int i=0;i<count;++i)free(names[i]);free(payload);return ok;
}
static int clip_publish_local_paths(NSArray<NSURL *> *urls) {
    NSUInteger n=[urls count];
    if(!n||n>HOP_CLIP_OFFER_MAX_ITEMS)return 0;
    char *copies[HOP_CLIP_OFFER_MAX_ITEMS]={0};
    for(NSUInteger i=0;i<n;++i){
        const char *path=[[urls objectAtIndex:i] fileSystemRepresentation];
        if(!path||!*path||!(copies[i]=strdup(path))){
            for(NSUInteger j=0;j<n;++j)free(copies[j]);return 0;
        }
    }
    clip_invalidate_remote_offer(1);clip_clear_local_offer(1);
    pthread_mutex_lock(&s_offer_lock);
    uint64_t generation=++s_local_offer_sequence;
    if(!generation)generation=++s_local_offer_sequence;
    s_local_offer_generation=generation;
    for(NSUInteger i=0;i<n;++i)s_local_offer_paths[i]=copies[i];
    s_local_offer_count=(int)n;
    pthread_mutex_unlock(&s_offer_lock);
    if(!clip_send_offer((const char *const *)copies,(int)n,generation))
        hop_log(HOP_LOG_WARN,"clip: file offer stored; peer will be updated on reconnect");
    else hop_log(HOP_LOG_INFO,"clip: file offer published (%lu top-level item(s), no file data sent)",(unsigned long)n);
    return 1;
}
static void clip_send_local_offer_snapshot(void) {
    char *copies[HOP_CLIP_OFFER_MAX_ITEMS]={0};int count;uint64_t generation;
    pthread_mutex_lock(&s_offer_lock);
    count=s_local_offer_count;generation=s_local_offer_generation;
    for(int i=0;i<count;++i)if(s_local_offer_paths[i])copies[i]=strdup(s_local_offer_paths[i]);
    pthread_mutex_unlock(&s_offer_lock);
    int valid=count>0;for(int i=0;i<count;++i)if(!copies[i])valid=0;
    if(valid&&!clip_send_offer((const char *const *)copies,count,generation))
        hop_log(HOP_LOG_WARN,"clip: file offer resend failed");
    for(int i=0;i<count;++i)free(copies[i]);
}

/* ---- 发送 ---- */

static BOOL send_paths(NSArray<NSURL *> *urls) {
    /* Only store local paths and publish a lightweight Offer. */
    return clip_publish_local_paths(urls) ? YES : NO;
}

static uint32_t s_image_work_seq;

static uint32_t image_work_transfer_id(void) {
    uint32_t id = __sync_add_and_fetch(&s_image_work_seq, 1);
    if (!id) id = __sync_add_and_fetch(&s_image_work_seq, 1);
    return id;
}

static int image_work_can_start(void) {
    pthread_mutex_lock(&s_offer_lock);
    int can_start = !s_clip_stopping && !s_image_work_active && s_paste_group != nil;
    pthread_mutex_unlock(&s_offer_lock);
    return can_start;
}

static hop_xfer_task *image_work_task_create(void) {
    hop_xfer_task *task = (hop_xfer_task *)calloc(1, sizeof(*task));
    if (!task) return NULL;
    if (!hop_xfer_task_init(task, 0, 0, image_work_transfer_id()) ||
        !hop_xfer_task_reserve_job(task) ||
        !hop_xfer_task_set_state(task, HOP_XFER_TASK_ACTIVE)) {
        if (task->initialized) {
            (void)hop_xfer_task_finish(task, HOP_XFER_TASK_FAILED);
            hop_xfer_task_dispose(task);
        }
        free(task);
        return NULL;
    }
    return task;
}

static void image_work_task_finish(hop_xfer_task *task, int success) {
    if (!task) return;
    if (task->reservation.image_temp_bytes &&
        !hop_xfer_task_release_image_temp(task, task->reservation.image_temp_bytes)) {
        hop_log(HOP_LOG_ERROR, "clip: image temporary budget release failed (task=%llu)",
                (unsigned long long)task->id);
    }
    (void)hop_xfer_task_finish(task, success ? HOP_XFER_TASK_COMPLETED : HOP_XFER_TASK_FAILED);
    hop_xfer_task_dispose(task);
    free(task);
}

/* NSImage -> 自上而下 BGRA。调用方在 NSImage 解码前预留转换额度；
 * 这里按真实像素尺寸收窄额度，再分配 bitmap 与行翻转缓冲区。 */
static BOOL send_image(NSImage *img, NSInteger expected_change, hop_xfer_task *task,
                       uint64_t *conversion_reserved) {
    if (!img || !task || !conversion_reserved || !*conversion_reserved) return NO;

    /* CGImage 创建期间可能触发解码；调用方已提前预留最大可用转换内存。 */
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return NO;

    size_t cg_w = CGImageGetWidth(cg), cg_h = CGImageGetHeight(cg);
    if (!cg_w || !cg_h || cg_w > (size_t)(INT_MAX / 4) || cg_h > (size_t)INT_MAX)
        return NO;
    int w = (int)cg_w, h = (int)cg_h;
    size_t stride = cg_w * 4u;
    if (cg_h > SIZE_MAX / stride) return NO;
    size_t total = stride * cg_h;
    if (total > CLIP_IMG_MAX_BYTES || total > UINT64_MAX / 3u) {
        hop_log(HOP_LOG_WARN, "clip: image %.1f MB too large, skipped",
                (double)total / 1048576.0);
        return NO;
    }

    /* decoded source + CGContext bitmap + flip buffer; queued payload is
     * reserved separately by xfer_submit_image before its copy. */
    uint64_t conversion_bytes = (uint64_t)total * 3u;
    if (conversion_bytes > *conversion_reserved) {
        uint64_t extra = conversion_bytes - *conversion_reserved;
        if (!hop_xfer_task_reserve_image_temp(task, extra)) {
            hop_log(HOP_LOG_WARN, "clip: image conversion memory budget exhausted (task=%llu)",
                    (unsigned long long)task->id);
            return NO;
        }
        *conversion_reserved = conversion_bytes;
    } else if (*conversion_reserved > conversion_bytes) {
        uint64_t unused = *conversion_reserved - conversion_bytes;
        if (!hop_xfer_task_release_image_temp(task, unused)) {
            hop_log(HOP_LOG_ERROR, "clip: conversion budget shrink failed (task=%llu)",
                    (unsigned long long)task->id);
            return NO;
        }
        *conversion_reserved = conversion_bytes;
    }

    CGColorSpaceRef cs = NULL;
    CGContextRef ctx = NULL;
    uint8_t *flip = NULL;
    BOOL ok = NO;
    cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) goto cleanup;

    /* ByteOrder32Little + AlphaNoneSkipFirst => 内存里是 B,G,R,X */
    ctx = CGBitmapContextCreate(NULL, cg_w, cg_h, 8, stride, cs,
                                kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    if (!ctx) goto cleanup;

    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    void *data = CGBitmapContextGetData(ctx);
    if (!data) goto cleanup;

    /* Keep the known-correct row-flip path; both bitmap allocations are budgeted. */
    flip = (uint8_t *)malloc(total);
    if (!flip) goto cleanup;
    for (int y = 0; y < h; ++y) {
        memcpy(flip + (size_t)y * stride,
               (uint8_t *)data + (size_t)(h - 1 - y) * stride, stride);
    }

    if ([[NSPasteboard generalPasteboard] changeCount] != expected_change) {
        hop_log(HOP_LOG_INFO, "clip: stale image conversion discarded");
        goto cleanup;
    }
    hop_log(HOP_LOG_INFO, "clip: image out %dx%d task=%llu", w, h,
            (unsigned long long)task->id);
    {
        uint8_t *owned_pixels = flip;
        flip = NULL; /* xfer_submit_image_owned consumes the buffer on every return path */
        ok = xfer_submit_image_owned(owned_pixels, w, h, (int)stride) ? YES : NO;
    }

cleanup:
    if (flip) free(flip);
    if (ctx) CGContextRelease(ctx);
    if (cs) CGColorSpaceRelease(cs);
    if (!ok) hop_log(HOP_LOG_WARN, "clip: image not queued; will retry");
    return ok;
}

/* Pasteboard image materialization, decoding and BGRA conversion all run on
 * one utility worker. Reserve a bounded upper estimate before dataForType so
 * the main queue never materializes a full image NSData snapshot. */
static void send_image_data_async(NSPasteboardType imageType, NSInteger expected_change) {
    if (!imageType) return;
    hop_xfer_task *task = image_work_task_create();
    if (!task) {
        hop_log(HOP_LOG_WARN, "clip: image conversion task budget exhausted");
        return;
    }
    if (!hop_xfer_task_reserve_image_temp(task, CLIP_IMG_MAX_BYTES)) {
        hop_log(HOP_LOG_WARN, "clip: image input memory budget exhausted (task=%llu)",
                (unsigned long long)task->id);
        image_work_task_finish(task, 0);
        return;
    }

    dispatch_group_t group = nil;
    pthread_mutex_lock(&s_offer_lock);
    if (s_clip_stopping || s_image_work_active || !s_paste_group) {
        pthread_mutex_unlock(&s_offer_lock);
        image_work_task_finish(task, 0);
        hop_log(HOP_LOG_WARN, "clip: image conversion busy; snapshot skipped");
        return;
    }
    s_image_work_active = 1;
    group = s_paste_group;
    dispatch_group_enter(group);
    pthread_mutex_unlock(&s_offer_lock);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int success = 0;
        uint64_t conversion_reserved = 0;
        @autoreleasepool {
            NSPasteboard *pb = [NSPasteboard generalPasteboard];
            if ([pb changeCount] == expected_change) {
                NSData *snapshot = [pb dataForType:imageType];
                NSUInteger data_bytes = [snapshot length];
                if (data_bytes > 0 && data_bytes <= CLIP_IMG_MAX_BYTES) {
                    uint64_t actual_input = (uint64_t)data_bytes;
                    uint64_t unused_input = CLIP_IMG_MAX_BYTES - actual_input;
                    if (!unused_input ||
                        hop_xfer_task_release_image_temp(task, unused_input)) {
                        uint64_t max_image = HOP_XFER_DEFAULT_MAX_IMAGE_BYTES;
                        if (actual_input < max_image) {
                            conversion_reserved = max_image - actual_input;
                            if (hop_xfer_task_reserve_image_temp(task, conversion_reserved)) {
                                NSImage *img = [[NSImage alloc] initWithData:snapshot];
                                if (img)
                                    success = send_image(img, expected_change, task,
                                                         &conversion_reserved) ? 1 : 0;
                                else
                                    hop_log(HOP_LOG_WARN, "clip: image decode failed");
                            } else {
                                conversion_reserved = 0;
                                hop_log(HOP_LOG_WARN, "clip: image decode memory budget exhausted");
                            }
                        } else {
                            hop_log(HOP_LOG_WARN, "clip: no image memory remains after encoded snapshot");
                        }
                    } else {
                        hop_log(HOP_LOG_ERROR, "clip: encoded snapshot budget adjustment failed");
                    }
                } else {
                    hop_log(HOP_LOG_WARN, "clip: encoded image snapshot missing or too large");
                }
            } else {
                hop_log(HOP_LOG_INFO, "clip: stale image snapshot ignored");
            }
        }
        /* Task finish runs after the autorelease pool drains, so NSData/NSImage
         * backing memory stays reserved until those objects have released it. */
        image_work_task_finish(task, success);
        pthread_mutex_lock(&s_offer_lock);
        s_image_work_active = 0;
        pthread_mutex_unlock(&s_offer_lock);
        dispatch_group_leave(group);
    });
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
    hop_put_be32(p, (g_mac.cfg.role==HOP_ROLE_SERVER?HOP_DEV_SERVER:HOP_DEV_CLIENT));
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
        NSInteger previous_change;

        pthread_mutex_lock(&s_offer_lock);
        previous_change=s_last_change;
        int unchanged=(cc==previous_change);
        if(!unchanged)s_last_change=cc;
        pthread_mutex_unlock(&s_offer_lock);
        if(unchanged)return;
        clip_invalidate_remote_offer(1);
        clip_clear_local_offer(1);

        /* 文件优先：Finder 里 Cmd+C 一个文件，剪贴板上是 fileURL，
         * 只读字符串的话会"什么都不发生" */
        NSArray<NSURL *> *urls =
            [pb readObjectsForClasses:@[ [NSURL class] ]
                              options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
        if ([urls count] > 0) {
            (void)send_paths(urls);
            return;
        }

        /* 图片：TIFF 覆盖最广（截图、浏览器、预览都是它） */
        NSPasteboardType imageType =
            [pb availableTypeFromArray:@[ NSPasteboardTypePNG, NSPasteboardTypeTIFF ]];
        if (imageType != nil) {
            /* Do not materialize another large NSData snapshot while the
             * previous image is still being decoded or converted. */
            if (!image_work_can_start()) {
                pthread_mutex_lock(&s_offer_lock);
                if (s_last_change == cc) s_last_change = previous_change;
                pthread_mutex_unlock(&s_offer_lock);
                hop_log(HOP_LOG_INFO, "clip: image conversion already active; latest snapshot deferred");
                return;
            }
            clip_clear_remote_offer_local(); clip_clear_local_offer(1);
            send_image_data_async(imageType, cc);
            return;
        }

        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s) {
            const char *utf8 = [s UTF8String];
            uint32_t n = utf8 ? (uint32_t)strlen(utf8) : 0;
            if (n > 0 && n <= HOP_TEXT_MAX) {
                clip_clear_remote_offer_local(); clip_clear_local_offer(1);
                (void)send_text(s);
                return;
            }
        }

        /* Unsupported/empty clipboard formats are intentionally consumed. */
        pthread_mutex_lock(&s_offer_lock); s_last_change = cc; pthread_mutex_unlock(&s_offer_lock);
    }
}

void clipboard_init(void) {
    pthread_mutex_lock(&s_offer_lock);
    s_clip_stopping=0;s_paste_scan_active=0;s_image_work_active=0;
    s_paste_group=dispatch_group_create();
    s_last_change = [[NSPasteboard generalPasteboard] changeCount];
    pthread_mutex_unlock(&s_offer_lock);

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

void clipboard_shutdown(void) {
    pthread_mutex_lock(&s_offer_lock);s_clip_stopping=1;pthread_mutex_unlock(&s_offer_lock);
    if(s_timer){dispatch_source_cancel(s_timer);s_timer=nil;}
    if(s_paste_group)dispatch_group_wait(s_paste_group,DISPATCH_TIME_FOREVER);
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
    if (expected_count >= 0 && [pb changeCount] != expected_count) {
        hop_log(HOP_LOG_INFO, "clip: %s apply retry cancelled; clipboard changed locally",
                [kind UTF8String]);
        if([kind isEqualToString:@"paths-paste"]) {
            pthread_mutex_lock(&s_offer_lock); s_pending_paste_request=0; s_pending_paste_change=0; pthread_mutex_unlock(&s_offer_lock);
        }
        return;
    }

    [pb clearContents];
    pthread_mutex_lock(&s_offer_lock); s_last_change = [pb changeCount]; pthread_mutex_unlock(&s_offer_lock); /* suppress the clear notification */
    BOOL ok = [pb writeObjects:objects];
    if (ok) {
        pthread_mutex_lock(&s_offer_lock); s_last_change = [pb changeCount]; pthread_mutex_unlock(&s_offer_lock);
        if([kind isEqualToString:@"paths-paste"]) {
            pthread_mutex_lock(&s_offer_lock);
            s_remote_offer_origin=0;s_remote_offer_generation=0;
            s_pending_paste_request=0;s_pending_paste_change=0;
            pthread_mutex_unlock(&s_offer_lock);
        }
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
        if([kind isEqualToString:@"paths-paste"]) { pthread_mutex_lock(&s_offer_lock); s_pending_paste_request=0; s_pending_paste_change=0; pthread_mutex_unlock(&s_offer_lock); }
        hop_log(HOP_LOG_ERROR, "clip: %s pasteboard write failed after retries",
                [kind UTF8String]);
    }
}

void clipboard_apply(const char *utf8) {
    NSString *s;

    if (!utf8 || !*utf8) return;
    clip_clear_local_offer(1); clip_clear_remote_offer_local();
    s = [NSString stringWithUTF8String:utf8];
    if (!s) return;

    apply_objects_retry(@[ s ], @"text", 0, -1);
}

/* 一批文件/目录落盘完毕：把顶层条目挂到剪贴板，Finder 里 Cmd+V 即复制 */
void clipboard_apply_paths(const char *const *paths, int n) {
    if (!paths || n <= 0) return;
    clip_clear_local_offer(1); clip_clear_remote_offer_local();

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
    clip_clear_local_offer(1); clip_clear_remote_offer_local();
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

/* Lazy file clipboard request/response control plane. */
typedef struct { char **paths; int count; uint32_t request_id; uint64_t offer_id; } clip_paste_work;
static void clip_send_paste_failed(uint32_t request_id,uint8_t reason) {
    uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
    uint32_t n=hop_clip_paste_failed_encode(p,request_id,reason);
    if(n)(void)net_send(HOP_MSG_CLIP_PASTE_FAILED,p,n);
}
void clipboard_on_link(int connected) {
    pthread_mutex_lock(&s_offer_lock);int stopping=s_clip_stopping;pthread_mutex_unlock(&s_offer_lock);
    if(stopping)return;
    if(connected)clip_send_local_offer_snapshot();
    else clip_clear_remote_offer_local();
}
void clipboard_on_remote_file_offer(const uint8_t *p,uint32_t len) {
    hop_clip_file_offer_msg m;
    if(!hop_clip_file_offer_decode(p,len,&m)||m.origin==clip_local_device())return;
    pthread_mutex_lock(&s_offer_lock);int stopping=s_clip_stopping;pthread_mutex_unlock(&s_offer_lock);
    if(stopping)return;
    clip_clear_local_offer(1);
    pthread_mutex_lock(&s_offer_lock);
    s_remote_offer_origin=m.origin;s_remote_offer_generation=m.generation;
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clipboard_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    int clear_local=0;
    pthread_mutex_lock(&s_offer_lock);
    if(origin==clip_local_device()&&s_local_offer_generation==generation)clear_local=1;
    else if(s_remote_offer_origin==origin&&s_remote_offer_generation==generation){
        s_remote_offer_origin=0;s_remote_offer_generation=0;
    }
    pthread_mutex_unlock(&s_offer_lock);
    if(clear_local)clip_clear_local_offer(0);
}
int clipboard_request_remote_paste(void) {
    __block NSInteger current=0;
    if([NSThread isMainThread]) current=[[NSPasteboard generalPasteboard] changeCount];
    else dispatch_sync(dispatch_get_main_queue(),^{current=[[NSPasteboard generalPasteboard] changeCount];});
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t id=0,origin=0;uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping||!s_remote_offer_origin||!s_remote_offer_generation||current!=s_last_change){
        pthread_mutex_unlock(&s_offer_lock);return 0;
    }
    if(s_pending_paste_request){pthread_mutex_unlock(&s_offer_lock);return 1;}
    if(++s_offer_request_seq==0||s_offer_request_seq>0x7fffffffu)s_offer_request_seq=1;
    id=HOP_XFER_PASTE_BATCH_FLAG|s_offer_request_seq;
    origin=s_remote_offer_origin;generation=s_remote_offer_generation;
    s_pending_paste_request=id;s_pending_paste_change=current;
    pthread_mutex_unlock(&s_offer_lock);
    int sent=hop_clip_paste_request_encode(p,id,origin,generation,(uint64_t)current) &&
             net_send(HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        pthread_mutex_lock(&s_offer_lock);
        if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_change=0;}
        pthread_mutex_unlock(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",id);
    return 1;
}
void clipboard_on_paste_failed(const uint8_t *p,uint32_t len) {
    uint32_t id=0;uint8_t reason=0;
    if(!hop_clip_paste_failed_decode(p,len,&id,&reason))return;
    pthread_mutex_lock(&s_offer_lock);
    if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_change=0;}
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_WARN,"clip: remote paste request %u failed (reason %u)",id,(unsigned)reason);
}
static void clip_paste_worker(void *arg) {
    clip_paste_work *w=(clip_paste_work*)arg;
    int ok=xfer_submit_paths_for_paste((const char *const *)w->paths,w->count,w->request_id,w->offer_id);
    if(!ok)clip_send_paste_failed(w->request_id,
        xfer_connected()?HOP_CLIP_PASTE_FAIL_SOURCE_IO:HOP_CLIP_PASTE_FAIL_TRANSFER);
    for(int i=0;i<w->count;++i)free(w->paths[i]);
    free(w->paths);free(w);
}
void clipboard_on_paste_request(const uint8_t *p,uint32_t len) {
    hop_clip_paste_request_msg m;
    if(!hop_clip_paste_request_decode(p,len,&m))return;
    if(m.origin!=clip_local_device()){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;}
    clip_paste_work *w=(clip_paste_work*)calloc(1,sizeof(*w));
    if(!w){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;}
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping||s_paste_scan_active||!s_paste_group){
        pthread_mutex_unlock(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    for(unsigned i=0;i<32;++i)if(s_seen_paste_requests[i]==m.request_id){
        pthread_mutex_unlock(&s_offer_lock);free(w);return;
    }
    if(m.generation!=s_local_offer_generation||s_local_offer_count<=0){
        pthread_mutex_unlock(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;
    }
    w->count=s_local_offer_count;w->request_id=m.request_id;w->offer_id=m.generation;
    w->paths=(char**)calloc((size_t)w->count,sizeof(char*));
    if(w->paths)for(int i=0;i<w->count;++i)w->paths[i]=strdup(s_local_offer_paths[i]);
    int copy_ok=w->paths!=NULL;
    if(copy_ok)for(int i=0;i<w->count;++i)if(!w->paths[i])copy_ok=0;
    if(!copy_ok){
        pthread_mutex_unlock(&s_offer_lock);
        if(w->paths){for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);}
        free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    s_seen_paste_requests[s_seen_paste_next++%32]=m.request_id;
    s_paste_scan_active=1;dispatch_group_enter(s_paste_group);
    pthread_mutex_unlock(&s_offer_lock);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        clip_paste_worker(w);
        pthread_mutex_lock(&s_offer_lock);s_paste_scan_active=0;pthread_mutex_unlock(&s_offer_lock);
        dispatch_group_leave(s_paste_group);
    });
}
void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success) {
    NSInteger expected;
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping){s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    if(s_pending_paste_request!=batch){pthread_mutex_unlock(&s_offer_lock);return;}
    expected=s_pending_paste_change;
    if(!success){s_pending_paste_request=0;s_pending_paste_change=0;}
    pthread_mutex_unlock(&s_offer_lock);
    if(!success){hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; pasteboard unchanged",batch);return;}
    if(!paths||n<=0){pthread_mutex_lock(&s_offer_lock);s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    NSMutableArray<NSURL *> *urls=[NSMutableArray arrayWithCapacity:(NSUInteger)n];
    for(int i=0;i<n;++i){
        NSString *path=[NSString stringWithUTF8String:paths[i]];
        if(path)[urls addObject:[NSURL fileURLWithPath:path]];
    }
    if([urls count]==0){pthread_mutex_lock(&s_offer_lock);s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    NSArray *snapshot=[urls copy];
    dispatch_async(dispatch_get_main_queue(),^{
        apply_objects_retry(snapshot,@"paths-paste",0,expected);
    });
}
