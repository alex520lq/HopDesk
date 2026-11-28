/* =====================================================================
 *  HopDesk - macOS Agent-Owned Three-Stage Updater
 *  Check -> Async Download + SHA-256 -> Restart & Apply (with cert preservation).
 *  Works identically in split-agent mode and single-process mode.
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include "mac_app.h"
#include "split_agent.h"
#include "xfer.h"
#include "../core/update_controller.h"
#include "../core/update_manifest.h"
#include "../core/update_state.h"

#define MAC_UPDATE_MAX_DOWNLOAD (512ull * 1024ull * 1024ull)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static hop_update_snapshot s_snap;
static hop_update_manifest s_manifest;
static int s_inited;
static int s_busy;
static int s_has_manifest;
static int s_staged_ready;
static int s_applying_handoff;
static NSString *s_staged_archive;

static int current_build_channel(void) {
    return hop_update_channel_from_name(HOP_CHANNEL) == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
}

static void ensure_inited_locked(void) {
    if (!s_inited) {
        hop_update_snapshot_init(&s_snap, g_mac.cfg.update_channel);
        s_inited = 1;
    }
}

static void clear_staged_locked(void) {
    if (s_staged_archive && !s_applying_handoff) {
        [[NSFileManager defaultManager] removeItemAtPath:s_staged_archive error:nil];
    }
    s_staged_archive = nil;
    s_staged_ready = 0;
}

void mac_update_get_snapshot(hop_update_snapshot *out) {
    if (!out) return;
    pthread_mutex_lock(&s_lock);
    ensure_inited_locked();
    *out = s_snap;
    pthread_mutex_unlock(&s_lock);
}

void mac_update_shutdown(void) {
    pthread_mutex_lock(&s_lock);
    clear_staged_locked();
    pthread_mutex_unlock(&s_lock);
}

static BOOL verify_file_sha256(NSString *path, const char *expected) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    CC_SHA256_CTX ctx;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    unsigned char buffer[64 * 1024];
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    size_t n;
    if (!file) return NO;
    CC_SHA256_Init(&ctx);
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
        CC_SHA256_Update(&ctx, buffer, (CC_LONG)n);
    BOOL readOK = !ferror(file);
    fclose(file);
    if (!readOK) return NO;
    CC_SHA256_Final(digest, &ctx);
    for (NSUInteger i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[sizeof(hex) - 1] = '\0';
    return strcasecmp(hex, expected) == 0;
}

static void fetch_manifest_url(NSString *urlString, int channel, BOOL allowFallback) {
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, channel, "更新清单地址无效。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:20.0];
    [request setValue:@"HopDesk-updater/2.0" forHTTPHeaderField:@"User-Agent"];

    NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        hop_update_manifest manifest;
        BOOL parsed = !error && data && data.length > 0 &&
            data.length <= HOP_UPDATE_MANIFEST_MAX &&
            http && http.statusCode == 200 &&
            hop_update_manifest_parse((const char *)data.bytes, data.length, &manifest);
        if (!parsed) {
            if (allowFallback && channel == HOP_UPDATE_CHANNEL_STABLE) {
                fetch_manifest_url(@HOP_UPDATE_MANIFEST_URL, channel, NO);
                return;
            }
            pthread_mutex_lock(&s_lock);
            hop_update_snapshot_set_error(&s_snap, channel,
                "无法读取有效的更新清单。请检查网络或确认该通道已有发布版本。");
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        pthread_mutex_lock(&s_lock);
        s_manifest = manifest;
        s_has_manifest = 1;
        hop_update_snapshot_set_manifest_result(&s_snap, HOP_VERSION, current_build_channel(),
                                                HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER,
                                                channel, &manifest);
        int resultPhase = s_snap.phase;
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        hop_log(HOP_LOG_INFO, "update: checked channel=%s result_phase=%d target=v%s",
                hop_update_channel_name(channel), resultPhase, manifest.version);
    }];
    [task resume];
}

void mac_update_check(int target_channel) {
    int ch = target_channel == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;

    pthread_mutex_lock(&s_lock);
    ensure_inited_locked();
    if (s_busy) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if (!g_mac.cfg.update_check_enabled) {
        hop_update_snapshot_set_error(&s_snap, ch,
            "已在配置中禁用更新检查（update_check_enabled=0）。");
        pthread_mutex_unlock(&s_lock);
        return;
    }
    clear_staged_locked();
    s_has_manifest = 0;
    s_busy = 1;
    hop_update_snapshot_set_checking(&s_snap, ch);
    pthread_mutex_unlock(&s_lock);

    NSString *urlString = [NSString stringWithUTF8String:hop_update_manifest_url(ch)];
    fetch_manifest_url(urlString, ch, ch == HOP_UPDATE_CHANNEL_STABLE);
}

@interface HopUpdateDownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, assign) hop_update_manifest manifest;
@property (nonatomic, assign) uint64_t lastReported;
@property (nonatomic, assign) BOOL finished;
@end

@implementation HopUpdateDownloadDelegate

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    (void)session;
    (void)bytesWritten;
    if (totalBytesWritten < 0) return;
    uint64_t done = (uint64_t)totalBytesWritten;
    uint64_t total = totalBytesExpectedToWrite > 0 ? (uint64_t)totalBytesExpectedToWrite : 0;
    if (done > MAC_UPDATE_MAX_DOWNLOAD || total > MAC_UPDATE_MAX_DOWNLOAD) {
        [downloadTask cancel];
        return;
    }
    if (done - self.lastReported >= 65536ull || (total && done >= total)) {
        self.lastReported = done;
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_downloading(&s_snap, done, total);
        pthread_mutex_unlock(&s_lock);
    }
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    (void)session;
    NSHTTPURLResponse *http = (NSHTTPURLResponse *)downloadTask.response;
    if (!location || !http || http.statusCode != 200) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, self.manifest.channel,
            "更新包下载失败：服务器未返回有效安装包。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        self.finished = YES;
        return;
    }

    NSError *fileError = nil;
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:location.path error:&fileError];
    uint64_t size = [attrs[NSFileSize] unsignedLongLongValue];
    if (fileError || size == 0 || size > MAC_UPDATE_MAX_DOWNLOAD) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, self.manifest.channel,
            "安装包大小无效或超过 512 MB，已拒绝安装。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        self.finished = YES;
        return;
    }

    NSString *archive = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"HopDesk-update-%@.zip", [[NSUUID UUID] UUIDString]]];
    if (![[NSFileManager defaultManager] moveItemAtURL:location
            toURL:[NSURL fileURLWithPath:archive] error:&fileError]) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, self.manifest.channel,
            "无法保存已下载的更新安装包。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        self.finished = YES;
        return;
    }

    if (!verify_file_sha256(archive, self.manifest.macos_sha256)) {
        [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, self.manifest.channel,
            "安全校验失败：安装包 SHA-256 与清单不一致，旧版本未被修改。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        self.finished = YES;
        return;
    }

    pthread_mutex_lock(&s_lock);
    clear_staged_locked();
    s_staged_archive = archive;
    s_staged_ready = 1;
    hop_update_snapshot_set_ready(&s_snap, size);
    s_busy = 0;
    pthread_mutex_unlock(&s_lock);
    self.finished = YES;
    hop_log(HOP_LOG_INFO, "update: downloaded and verified v%s (%llu bytes)",
            self.manifest.version, (unsigned long long)size);
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    (void)task;
    if (!self.finished) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, self.manifest.channel,
            error ? "更新包下载中断，请检查网络后重试。" : "更新包下载未完成，请稍后重试。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
    }
    [session finishTasksAndInvalidate];
}

@end

void mac_update_download(void) {
    hop_update_manifest manifest;

    pthread_mutex_lock(&s_lock);
    ensure_inited_locked();
    if (s_busy || !s_has_manifest ||
        (s_snap.phase != HOP_UPDATE_AVAILABLE && s_snap.phase != HOP_UPDATE_ERROR)) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    clear_staged_locked();
    manifest = s_manifest;
    s_busy = 1;
    hop_update_snapshot_set_downloading(&s_snap, 0, 0);
    pthread_mutex_unlock(&s_lock);

    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:manifest.macos_url]];
    if (!url) {
        pthread_mutex_lock(&s_lock);
        hop_update_snapshot_set_error(&s_snap, manifest.channel, "更新下载地址无效。");
        s_busy = 0;
        pthread_mutex_unlock(&s_lock);
        return;
    }

    HopUpdateDownloadDelegate *delegate = [HopUpdateDownloadDelegate new];
    delegate.manifest = manifest;
    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 30.0;
    cfg.timeoutIntervalForResource = 600.0;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg
                                                          delegate:delegate
                                                     delegateQueue:nil];
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:url];
    [task resume];
}

void mac_update_apply(void) {
    NSString *archive = nil;
    hop_update_manifest manifest;

    pthread_mutex_lock(&s_lock);
    ensure_inited_locked();
    if (s_busy) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if (!s_has_manifest || !s_staged_ready || !s_staged_archive.length) {
        hop_update_snapshot_set_error(&s_snap, s_snap.target_channel,
            "尚未下载就绪的更新包，请先点击“下载更新”。");
        pthread_mutex_unlock(&s_lock);
        return;
    }
    archive = s_staged_archive;
    manifest = s_manifest;
    s_busy = 1;
    hop_update_snapshot_set_applying(&s_snap);
    pthread_mutex_unlock(&s_lock);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *fileError = nil;
        NSFileManager *fm = [NSFileManager defaultManager];
        if (![fm fileExistsAtPath:archive]) {
            pthread_mutex_lock(&s_lock);
            clear_staged_locked();
            hop_update_snapshot_set_error(&s_snap, manifest.channel,
                "已下载的安装包已失效，请重新下载更新。");
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        NSString *helper = [[NSBundle mainBundle] pathForResource:@"update-macos" ofType:@"command"];
        NSString *helperCopy = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"HopDesk-updater-%@.command", [[NSUUID UUID] UUIDString]]];
        NSString *appPath = [[NSBundle mainBundle] bundlePath];
        if (!helper || ![fm copyItemAtPath:helper toPath:helperCopy error:&fileError]) {
            pthread_mutex_lock(&s_lock);
            s_snap.phase = HOP_UPDATE_READY;
            snprintf(s_snap.message, sizeof(s_snap.message), "%s",
                     "当前安装缺少更新辅助程序 update-macos.command，请重新安装应用。");
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        if (!hop_update_install_begin(xfer_busy)) {
            [fm removeItemAtPath:helperCopy error:nil];
            pthread_mutex_lock(&s_lock);
            s_snap.phase = HOP_UPDATE_READY;
            snprintf(s_snap.message, sizeof(s_snap.message), "%s",
                     "当前仍有文件传输任务，请等待传输完成后再点击重启安装。");
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        NSString *nonce = [[[[NSUUID UUID] UUIDString]
            stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
        NSString *transaction = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"HopDesk-update-" stringByAppendingString:nonce]];
        if (![fm createDirectoryAtPath:transaction withIntermediateDirectories:NO
                            attributes:@{NSFilePosixPermissions: @0700} error:&fileError]) {
            hop_update_install_end();
            [fm removeItemAtPath:helperCopy error:nil];
            pthread_mutex_lock(&s_lock);
            s_snap.phase = HOP_UPDATE_READY;
            snprintf(s_snap.message, sizeof(s_snap.message), "%s",
                     "无法创建更新事务目录，当前版本继续运行。");
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        NSTask *installer = [[NSTask alloc] init];
        installer.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
        installer.arguments = @[
            helperCopy, archive, appPath,
            [NSString stringWithFormat:@"%d", getpid()],
            transaction, nonce,
            [NSString stringWithUTF8String:manifest.version]
        ];
        NSError *launchError = nil;
        BOOL launched = [installer launchAndReturnError:&launchError];
        BOOL prepared = NO;
        NSString *preparedFile = [transaction stringByAppendingPathComponent:@"prepared"];
        if (launched) {
            for (int i = 0; i < 240 && installer.running; ++i) {
                NSString *ack = [NSString stringWithContentsOfFile:preparedFile
                                                          encoding:NSUTF8StringEncoding error:nil];
                if ([[ack stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]]
                        isEqualToString:nonce]) {
                    prepared = YES;
                    break;
                }
                [NSThread sleepForTimeInterval:0.25];
            }
        }
        [fm removeItemAtPath:helperCopy error:nil];
        if (!prepared) {
            if (installer.running) [installer terminate];
            if (!launched) [fm removeItemAtPath:transaction error:nil];
            hop_update_install_end();
            pthread_mutex_lock(&s_lock);
            if ([fm fileExistsAtPath:archive]) {
                s_snap.phase = HOP_UPDATE_READY;
                snprintf(s_snap.message, sizeof(s_snap.message), "%s",
                         "安装准备失败，当前版本继续运行（可检查目录权限后重试）。");
            } else {
                clear_staged_locked();
                hop_update_snapshot_set_error(&s_snap, manifest.channel,
                    "安装准备失败，当前版本继续运行。");
            }
            s_busy = 0;
            pthread_mutex_unlock(&s_lock);
            return;
        }

        pthread_mutex_lock(&s_lock);
        s_applying_handoff = 1;
        pthread_mutex_unlock(&s_lock);

        dispatch_async(dispatch_get_main_queue(), ^{
            if (split_agent_is_active()) {
                split_agent_stop_for_update();
            } else {
                net_request_stop();
                [NSApp terminate:nil];
            }
        });
    });
}
