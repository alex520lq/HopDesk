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

/* NSImage -> 自上而下 BGRA。用 CGBitmapContext 明确指定字节序，
 * 不依赖 NSBitmapImageRep 的默认通道顺序。 */
static BOOL send_image(NSImage *img, NSInteger expected_change) {
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

    /* Clipboard may change while the worker decodes/converts the snapshot. */
    if ([[NSPasteboard generalPasteboard] changeCount] != expected_change) {
        free(flip);
        CGContextRelease(ctx);
        hop_log(HOP_LOG_INFO, "clip: stale image conversion discarded");
        return NO;
    }
    hop_log(HOP_LOG_INFO, "clip: image out %dx%d", w, h);
    BOOL ok = xfer_submit_image(flip, w, h, (int)stride) ? YES : NO;
    free(flip);
    CGContextRelease(ctx);
    if (!ok) hop_log(HOP_LOG_WARN, "clip: image not queued; will retry");
    return ok;
}


/* Image decoding and BGRA conversion must not run on the main queue. Keep at
 * most one conversion in flight so rapid clipboard changes cannot create an
 * unbounded GCD backlog of large image snapshots. */
static void send_image_data_async(NSData *data, NSInteger expected_change) {
    if (!data) return;
    dispatch_group_t group = nil;
    pthread_mutex_lock(&s_offer_lock);
    if (s_clip_stopping || s_image_work_active || !s_paste_group) {
        pthread_mutex_unlock(&s_offer_lock);
        hop_log(HOP_LOG_WARN, "clip: image conversion busy; snapshot skipped");
        return;
    }
    s_image_work_active = 1;
    group = s_paste_group;
    dispatch_group_enter(group);
    pthread_mutex_unlock(&s_offer_lock);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            if ([[NSPasteboard generalPasteboard] changeCount] == expected_change) {
                NSImage *img = [[NSImage alloc] initWithData:data];
                if (img) (void)send_image(img, expected_change);
                else hop_log(HOP_LOG_WARN, "clip: image decode failed");
            } else {
                hop_log(HOP_LOG_INFO, "clip: stale image snapshot ignored");
            }
        }
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

        pthread_mutex_lock(&s_offer_lock);
        int unchanged=(cc==s_last_change);
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
            NSData *imageData = [pb dataForType:imageType];
            if (imageData) {
                clip_clear_remote_offer_local(); clip_clear_local_offer(1);
                send_image_data_async(imageData, cc);
                return;
            }
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
typedef struct { char **paths; int count; uint32_t request_id; } clip_paste_work;
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
    int ok=xfer_submit_paths_for_paste((const char *const *)w->paths,w->count,w->request_id);
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
    w->count=s_local_offer_count;w->request_id=m.request_id;
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
