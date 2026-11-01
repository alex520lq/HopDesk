/* =====================================================================
 *  HopDesk - macOS 状态栏（ObjC 薄层）
 *  Accessory 策略（无 Dock 图标），SF Symbol 反映连接 / 共享状态。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include "mac_app.h"
#include "xfer.h"
#include "update_manifest.h"
#include "../core/update_state.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

/* ---- 退出动作的 target ----
 * 必须显式声明 NSMenuItemValidation：10.14 之后 NSMenu 只对声明了该协议的
 * target 调用 validateMenuItem:，否则"取消当前传输"永远是亮的。 */
@interface HopController : NSObject <NSMenuItemValidation>
- (void)doQuit:(id)sender;
- (void)doWake:(id)sender;
- (void)doInputWin:(id)sender;
- (void)doCancelXfer:(id)sender;
- (void)doTransfers:(id)sender;
- (void)doRetryFile:(id)sender;
- (void)doToggleFolder:(id)sender;
- (void)doCheckUpdates:(id)sender;
@end
static void retry_selected_mac_file(void);
static void toggle_selected_mac_folder(void);
static BOOL s_updateBusy;
static void check_for_updates(void);
@implementation HopController
- (void)doQuit:(id)sender {
    (void)sender;
    net_request_stop();
    [NSApp terminate:nil];
}
/* 本机手动触发：连接没建立时也能单独验证显示控制 */
- (void)doWake:(id)sender {
    (void)sender;
    disp_wake(g_mac.cfg.disp_wake_sec);
}
- (void)doInputWin:(id)sender {
    (void)sender;
    disp_set_input(g_mac.cfg.disp_win_input);
}
/* 传了一半改主意：键鼠已经切走了、或者发现选错了文件 */
- (void)doCancelXfer:(id)sender {
    (void)sender;
    xfer_cancel();
}
- (void)doTransfers:(id)sender { (void)sender; statusbar_show_transfers(); }
- (void)doRetryFile:(id)sender { (void)sender; retry_selected_mac_file(); }
- (void)doToggleFolder:(id)sender { (void)sender; toggle_selected_mac_folder(); }
- (void)doCheckUpdates:(id)sender { (void)sender; check_for_updates(); }
/* 菜单打开时自动校验：没有传输在进行就把"取消"变灰。
 * NSMenu 默认 autoenablesItems，走的就是这个入口。 */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if ([item action] == @selector(doCancelXfer:)) return xfer_busy() != 0;
    if ([item action] == @selector(doCheckUpdates:)) return !s_updateBusy;
    return YES;
}
@end

static NSStatusItem *s_item;
static NSMenuItem   *s_state;
static HopController *s_ctrl;
static NSWindow *s_transferWindow;
static NSTextField *s_transferTitle, *s_transferDetail, *s_transferCounts;
static NSProgressIndicator *s_transferProgress;
static NSTextView *s_transferHistory, *s_transferFiles;
static NSButton *s_retryButton;
static hop_xfer_file_status *s_transferFileSnapshot;
static int s_collapsedFolders[HOP_XFER_FILE_LIMIT];
static int s_visibleFileIndices[HOP_XFER_FILE_LIMIT];
static int s_visibleFileCount;
static uint32_t s_collapsedBatch, s_fileBatch;
static int s_collapsedDirection, s_fileDirection, s_notifiedDirection;
static uint32_t s_notifiedBatch;
static int s_notifiedPhase;

static void retry_selected_mac_file(void) {
    if (!s_transferFiles) return;
    NSRange selected = [s_transferFiles selectedRange];
    NSString *content = s_transferFiles.string ?: @"";
    NSUInteger line = 0;
    NSUInteger limit = MIN(selected.location, content.length);
    for (NSUInteger i = 0; i < limit; ++i) if ([content characterAtIndex:i] == '\n') line++;
    if (s_fileDirection == HOP_XFER_DIR_SEND && line < (NSUInteger)s_visibleFileCount)
        (void)xfer_retry_file(s_fileBatch,s_visibleFileIndices[line]);
}
static void toggle_selected_mac_folder(void) {
    if (!s_transferFiles) return;
    NSRange selected = [s_transferFiles selectedRange];
    NSString *content = s_transferFiles.string ?: @"";
    NSUInteger line = 0, limit = MIN(selected.location, content.length);
    for (NSUInteger i = 0; i < limit; ++i) if ([content characterAtIndex:i] == '\n') line++;
    if (line >= (NSUInteger)s_visibleFileCount) return;
    int index = s_visibleFileIndices[line];
    if (s_transferFileSnapshot[index].is_directory)
        s_collapsedFolders[index] = !s_collapsedFolders[index];
}

static void transfer_notify_if_terminal(const hop_xfer_status *st) {
    if (!st || st->active || st->batch == 0) return;
    if (st->phase != HOP_XFER_COMPLETED && st->phase != HOP_XFER_PARTIAL &&
        st->phase != HOP_XFER_FAILED && st->phase != HOP_XFER_CANCELLED) return;
    if (s_notifiedBatch == st->batch && s_notifiedPhase == st->phase && s_notifiedDirection == st->direction) return;
    s_notifiedBatch = st->batch;
    s_notifiedPhase = st->phase; s_notifiedDirection=st->direction;
    NSString *title = st->phase == HOP_XFER_COMPLETED ? @"传输完成" :
                      st->phase == HOP_XFER_PARTIAL ? @"传输部分失败" :
                      st->phase == HOP_XFER_CANCELLED ? @"传输已取消" : @"传输失败";
    NSString *detail = st->detail[0] ? [NSString stringWithUTF8String:st->detail] : @"请打开传输管理器查看详情";
    NSUserNotification *note = [[NSUserNotification alloc] init];
    note.title = title;
    note.informativeText = detail ?: @"请打开传输管理器查看详情";
    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:note];
}

static void set_appearance(int connected, int sharing) {
    NSString *sym;
    NSColor  *color;
    if (sharing) {
        sym = @"computermouse.fill"; color = [NSColor systemGreenColor];
    } else if (connected) {
        sym = @"computermouse"; color = [NSColor systemBlueColor];
    } else {
        sym = @"circle"; color = [NSColor systemGrayColor];
    }
    NSString *iconPath = [[NSBundle mainBundle] pathForResource:@"hopdesk" ofType:@"png"];
    NSImage *base = iconPath ? [[NSImage alloc] initWithContentsOfFile:iconPath] : nil;
    if (!base) {
        base = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:nil];
        NSImageSymbolConfiguration *cfg =
            [NSImageSymbolConfiguration configurationWithPaletteColors:@[color]];
        base = [base imageWithSymbolConfiguration:cfg];
    } else {
        base.size = NSMakeSize(18.0, 18.0);
        base.template = NO;
    }
    s_item.button.image = base;

    s_state.title = sharing ? @"键鼠在 Mac"
                  : connected ? @"已连接（Windows 主控）"
                  : @"等待连接…";
}


static void update_alert(NSString *title, NSString *message, NSAlertStyle style) {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = title ?: @"HopDesk 更新";
    alert.informativeText = message ?: @"";
    alert.alertStyle = style;
    [alert addButtonWithTitle:@"好"];
    [alert runModal];
}

static BOOL update_file_sha256(NSString *path, const char *expected) {
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

static void start_update_download(hop_update_manifest manifest) {
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:manifest.macos_url]];
    if (!url) {
        s_updateBusy = NO;
        update_alert(@"更新失败", @"更新下载地址无效。", NSAlertStyleCritical);
        return;
    }

    NSURLSessionDownloadTask *download = [[NSURLSession sharedSession]
        downloadTaskWithURL:url completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        if (error || !location || !http || http.statusCode != 200) {
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"下载失败", error.localizedDescription ?: @"GitHub 没有返回有效的安装包。", NSAlertStyleCritical);
            });
            return;
        }

        NSError *fileError = nil;
        NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:location.path error:&fileError];
        unsigned long long size = [attrs[NSFileSize] unsignedLongLongValue];
        if (fileError || size == 0 || size > 512ull * 1024ull * 1024ull) {
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"下载失败", @"安装包大小无效或超过 512 MB，已拒绝安装。", NSAlertStyleCritical);
            });
            return;
        }

        NSString *archive = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"HopDesk-update-%@.zip", [[NSUUID UUID] UUIDString]]];
        if (![[NSFileManager defaultManager] moveItemAtURL:location
            toURL:[NSURL fileURLWithPath:archive] error:&fileError]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"下载失败", fileError.localizedDescription ?: @"无法保存安装包。", NSAlertStyleCritical);
            });
            return;
        }
        if (!update_file_sha256(archive, manifest.macos_sha256)) {
            [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"安全校验失败", @"安装包 SHA-256 与更新清单不一致。旧版本未被修改。", NSAlertStyleCritical);
            });
            return;
        }

        NSString *helper = [[NSBundle mainBundle] pathForResource:@"update-macos" ofType:@"command"];
        NSString *helperCopy = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"HopDesk-updater-%@.command", [[NSUUID UUID] UUIDString]]];
        NSString *appPath = [[NSBundle mainBundle] bundlePath];
        if (!helper || ![[NSFileManager defaultManager] copyItemAtPath:helper toPath:helperCopy error:&fileError]) {
            [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"更新失败", @"当前安装缺少更新辅助程序，请先安装包含自动更新功能的版本。", NSAlertStyleCritical);
            });
            return;
        }

        if (!hop_update_install_begin(xfer_busy)) {
            [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
            [[NSFileManager defaultManager] removeItemAtPath:helperCopy error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"暂不能安装", @"传输已开始或仍在扫描，请等待完成后重试。", NSAlertStyleWarning);
            });
            return;
        }
        NSString *nonce = [[[NSUUID UUID] UUIDString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
        nonce = [nonce lowercaseString];
        NSString *transaction = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"HopDesk-update-" stringByAppendingString:nonce]];
        if (![[NSFileManager defaultManager] createDirectoryAtPath:transaction withIntermediateDirectories:NO
            attributes:@{NSFilePosixPermissions:@0700} error:&fileError]) {
            hop_update_install_end();
            [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
            [[NSFileManager defaultManager] removeItemAtPath:helperCopy error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"更新失败", @"无法创建更新事务。", NSAlertStyleCritical);
            });
            return;
        }
        NSTask *installer = [[NSTask alloc] init];
        installer.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
        installer.arguments = @[helperCopy, archive, appPath,
                                [NSString stringWithFormat:@"%d", getpid()], transaction, nonce,
                                [NSString stringWithUTF8String:manifest.version]];
        NSError *launchError = nil;
        BOOL launched = [installer launchAndReturnError:&launchError];
        BOOL prepared = NO;
        NSString *preparedFile = [transaction stringByAppendingPathComponent:@"prepared"];
        if (launched) {
            for (int i=0;i<240 && installer.running;i++) {
                NSString *ack = [NSString stringWithContentsOfFile:preparedFile encoding:NSUTF8StringEncoding error:nil];
                if ([[ack stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]] isEqualToString:nonce]) {
                    prepared = YES; break;
                }
                [NSThread sleepForTimeInterval:0.25];
            }
        }
        [[NSFileManager defaultManager] removeItemAtPath:helperCopy error:nil];
        if (!prepared) {
            if (installer.running) [installer terminate];
            if (!launched) [[NSFileManager defaultManager] removeItemAtPath:transaction error:nil];
            hop_update_install_end();
            // The old process is alive: the helper cannot have replaced it.
            [[NSFileManager defaultManager] removeItemAtPath:archive error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"更新失败", launchError.localizedDescription ?: @"安装准备失败，当前版本继续运行。", NSAlertStyleCritical);
            });
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            net_request_stop();
            [NSApp terminate:nil];
        });
    }];
    [download resume];
}

static void check_for_updates(void) {
    if (s_updateBusy) return;
    if (xfer_busy()) {
        update_alert(@"暂不能更新", @"当前仍有文件传输任务。请等待传输结束后再更新，避免中断文件传输。",
                     NSAlertStyleWarning);
        return;
    }
    s_updateBusy = YES;
    NSURL *url = [NSURL URLWithString:@HOP_UPDATE_MANIFEST_URL];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:20.0];
    NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        hop_update_manifest manifest;
        BOOL parsed = !error && data && data.length > 0 &&
            data.length <= HOP_UPDATE_MANIFEST_MAX &&
            http && http.statusCode == 200 &&
            hop_update_manifest_parse((const char *)data.bytes, data.length, &manifest);
        if (!parsed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                s_updateBusy = NO;
                update_alert(@"检查更新失败",
                    error.localizedDescription ?: @"无法读取有效的更新清单。请检查网络，或稍后重试。",
                    NSAlertStyleWarning);
            });
            return;
        }

        int valid = 0;
        int comparison = hop_version_compare(HOP_VERSION, manifest.version, &valid);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!valid) {
                s_updateBusy = NO;
                update_alert(@"无法更新", @"当前构建版本不是正式版本，无法执行自动更新。", NSAlertStyleWarning);
                return;
            }
            if (comparison >= 0) {
                s_updateBusy = NO;
                update_alert(@"HopDesk 已是最新版本",
                    [NSString stringWithFormat:@"当前版本：v%s", HOP_VERSION], NSAlertStyleInformational);
                return;
            }

            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = [NSString stringWithFormat:@"发现 HopDesk 新版本 v%s", manifest.version];
            alert.informativeText = [NSString stringWithFormat:
                @"当前版本：v%s\n\n确认后将自动下载、校验并替换应用，然后重新启动 HopDesk。",
                HOP_VERSION];
            alert.alertStyle = NSAlertStyleInformational;
            [alert addButtonWithTitle:@"下载并安装"];
            [alert addButtonWithTitle:@"稍后"];
            if ([alert runModal] == NSAlertFirstButtonReturn) {
                start_update_download(manifest);
            } else {
                s_updateBusy = NO;
            }
        });
    }];
    [task resume];
}

void statusbar_init(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        s_ctrl = [[HopController alloc] init];

        s_item = [[NSStatusBar systemStatusBar]
                  statusItemWithLength:NSVariableStatusItemLength];

        NSMenu *menu = [[NSMenu alloc] init];
        s_state = [[NSMenuItem alloc] initWithTitle:@"等待连接…"
                                             action:nil keyEquivalent:@""];
        [s_state setEnabled:NO];
        [menu addItem:s_state];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *wake = [[NSMenuItem alloc] initWithTitle:@"唤醒显示"
                                                      action:@selector(doWake:)
                                               keyEquivalent:@""];
        wake.target = s_ctrl;
        [menu addItem:wake];

        NSMenuItem *inwin = [[NSMenuItem alloc] initWithTitle:@"显示器输入 → Windows"
                                                       action:@selector(doInputWin:)
                                                keyEquivalent:@""];
        inwin.target = s_ctrl;
        [menu addItem:inwin];

        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *cancel = [[NSMenuItem alloc] initWithTitle:@"取消当前传输"
                                                        action:@selector(doCancelXfer:)
                                                 keyEquivalent:@""];
        cancel.target = s_ctrl;
        [menu addItem:cancel];
        NSMenuItem *transfers = [[NSMenuItem alloc] initWithTitle:@"传输管理器…"
                                                           action:@selector(doTransfers:)
                                                    keyEquivalent:@""];
        transfers.target = s_ctrl;
        [menu addItem:transfers];

        [menu addItem:[NSMenuItem separatorItem]];


        NSMenuItem *update = [[NSMenuItem alloc] initWithTitle:@"检查更新…"
                                                        action:@selector(doCheckUpdates:)
                                                 keyEquivalent:@""];
        update.target = s_ctrl;
        [menu addItem:update];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出 HopDesk"
                                                      action:@selector(doQuit:)
                                               keyEquivalent:@"q"];
        quit.target = s_ctrl;
        [menu addItem:quit];
        s_item.menu = menu;

        set_appearance(0, 0);
        [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            hop_xfer_status snapshot; memset(&snapshot, 0, sizeof(snapshot));
            xfer_get_status(&snapshot);
            transfer_notify_if_terminal(&snapshot);
        }];
    }
}

void statusbar_update(int connected, int sharing) {
    dispatch_async(dispatch_get_main_queue(), ^{
        set_appearance(connected, sharing);
    });
}

static NSTextField *transfer_label(NSString *text, NSRect frame, CGFloat size) {
    NSTextField *f = [[NSTextField alloc] initWithFrame:frame];
    f.stringValue = text; f.editable = NO; f.bezeled = NO; f.drawsBackground = NO;
    f.selectable = YES; f.font = [NSFont systemFontOfSize:size];
    return f;
}

static void transfer_window_refresh(void) {
    if (!s_transferWindow) return;
    hop_xfer_status st; memset(&st, 0, sizeof(st)); xfer_get_status(&st);
    transfer_notify_if_terminal(&st);
    if (s_collapsedBatch != st.batch || s_collapsedDirection != st.direction) {
        memset(s_collapsedFolders, 0, sizeof(s_collapsedFolders));
        s_collapsedBatch = st.batch; s_collapsedDirection = st.direction;
    }
    if (!s_transferFileSnapshot) s_transferFileSnapshot = calloc(HOP_XFER_FILE_LIMIT, sizeof(*s_transferFileSnapshot));
    int fileCount = s_transferFileSnapshot ? xfer_get_files(st.direction, st.batch, s_transferFileSnapshot, HOP_XFER_FILE_LIMIT) : 0;
    s_fileBatch=st.batch; s_fileDirection=st.direction;
    if (s_transferFiles) {
        NSMutableString *fileRows = [NSMutableString string];
        s_visibleFileCount = 0;
        for (int i = 0; i < fileCount; ++i) {
            NSString *path = [NSString stringWithUTF8String:s_transferFileSnapshot[i].relative_path] ?: @"(未知文件)";
            BOOL hidden = NO;
            for (int parent = 0; parent < i; ++parent) {
                if (!s_collapsedFolders[parent] || !s_transferFileSnapshot[parent].is_directory) continue;
                NSString *parentPath = [NSString stringWithUTF8String:s_transferFileSnapshot[parent].relative_path] ?: @"";
                if ([path hasPrefix:[parentPath stringByAppendingString:@"/"]]) { hidden = YES; break; }
            }
            if (hidden) continue;
            s_visibleFileIndices[s_visibleFileCount++] = i;
            NSUInteger depth = [[path componentsSeparatedByString:@"/"] count] - 1;
            NSString *indent = [@"" stringByPaddingToLength:MIN(depth * 2, 24) withString:@" " startingAtIndex:0];
            NSString *state = s_transferFileSnapshot[i].phase == HOP_XFER_FILE_COMPLETED ? @"完成" :
                              s_transferFileSnapshot[i].phase == HOP_XFER_FILE_FAILED ? @"失败" :
                              s_transferFileSnapshot[i].phase == HOP_XFER_FILE_SENDING ? @"发送中" :
                              s_transferFileSnapshot[i].phase == HOP_XFER_FILE_RECEIVING ? @"接收中" :
                              s_transferFileSnapshot[i].phase == HOP_XFER_FILE_VERIFYING ? @"校验中" : @"排队中";
            NSString *kind = s_transferFileSnapshot[i].is_directory ? (s_collapsedFolders[i] ? @"▸ 📁" : @"▾ 📁") : @"📄";
            [fileRows appendFormat:@"%@%@ %@  %llu/%llu B  %@\n", indent, kind, path,
                (unsigned long long)s_transferFileSnapshot[i].bytes_done, (unsigned long long)s_transferFileSnapshot[i].bytes_total, state];
        }
        NSString *nextFiles = fileRows.length ? fileRows : @"当前没有文件明细";
        if (![s_transferFiles.string isEqualToString:nextFiles]) s_transferFiles.string = nextFiles;
    }
    hop_xfer_history_entry history[HOP_XFER_HISTORY_LIMIT];
    int historyCount = xfer_get_history(history, HOP_XFER_HISTORY_LIMIT);
    if (s_transferHistory && historyCount > 0) {
        NSMutableString *rows = [NSMutableString string];
        for (int i = 0; i < historyCount; ++i) {
            NSString *name = history[i].name[0] ? [NSString stringWithUTF8String:history[i].name] : @"文件任务";
            NSString *phaseText = history[i].phase == HOP_XFER_COMPLETED ? @"完成" :
                                  history[i].phase == HOP_XFER_PARTIAL ? @"部分失败" :
                                  history[i].phase == HOP_XFER_CANCELLED ? @"已取消" : @"失败";
            NSString *dirText = history[i].direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收";
            [rows appendFormat:@"%@  %@  %@  文件 %u/%u  %@\n", dirText, phaseText, name,
                history[i].files_done, history[i].files_total,
                [NSByteCountFormatter stringFromByteCount:(long long)history[i].bytes_total countStyle:NSByteCountFormatterCountStyleFile]];
        }
        s_transferHistory.string = rows;
    }
    NSString *title = st.name[0] ? [NSString stringWithUTF8String:st.name] : @"暂无传输任务";
    NSString *phase = @"空闲";
    switch (st.phase) {
        case HOP_XFER_QUEUED: phase=@"排队中"; break;
        case HOP_XFER_SCANNING: phase=@"扫描中"; break;
        case HOP_XFER_SENDING: phase=@"发送中"; break;
        case HOP_XFER_RECEIVING: phase=@"接收中"; break;
        case HOP_XFER_VERIFYING: phase=@"校验中"; break;
        case HOP_XFER_COMPLETED: phase=@"已完成"; break;
        case HOP_XFER_PARTIAL: phase=@"部分失败"; break;
        case HOP_XFER_FAILED: phase=@"失败"; break;
        case HOP_XFER_CANCELLED: phase=@"已取消"; break;
    }
    s_transferTitle.stringValue = title ?: @"HopDesk";
    NSString *direction = st.direction == HOP_XFER_DIR_SEND ? @"发送" :
                          st.direction == HOP_XFER_DIR_RECEIVE ? @"接收" : @"";
    s_transferDetail.stringValue = [NSString stringWithFormat:@"%@ · %@ · %@",
        direction, phase, st.detail[0] ? [NSString stringWithUTF8String:st.detail] : @""];
    s_transferCounts.stringValue = [NSString stringWithFormat:@"文件 %u / %u    排队 %u    数据 %@ / %@",
        st.files_done, st.files_total, st.queued_count,
        [NSByteCountFormatter stringFromByteCount:(long long)st.bytes_done countStyle:NSByteCountFormatterCountStyleFile],
        [NSByteCountFormatter stringFromByteCount:(long long)st.bytes_total countStyle:NSByteCountFormatterCountStyleFile]];
    s_transferProgress.doubleValue = st.bytes_total ? MIN(100.0, 100.0 * (double)st.bytes_done / (double)st.bytes_total) : (st.active ? 0 : 100);
    s_retryButton.enabled = (st.direction == HOP_XFER_DIR_SEND);
}

void statusbar_show_transfers(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!s_transferWindow) {
            s_transferWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,620,620)
                styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable)
                backing:NSBackingStoreBuffered defer:NO];
            s_transferWindow.title = @"HopDesk · 传输管理器";
            NSView *v = s_transferWindow.contentView;
            s_transferTitle = transfer_label(@"暂无传输任务", NSMakeRect(20,580,580,22), 15);
            s_transferDetail = transfer_label(@"等待文件传输", NSMakeRect(20,550,580,20), 12);
            s_transferCounts = transfer_label(@"文件 0 / 0    数据 0 B / 0 B", NSMakeRect(20,525,580,20), 12);
            s_transferProgress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20,500,580,12)];
            s_transferProgress.minValue=0; s_transferProgress.maxValue=100; s_transferProgress.indeterminate=NO;
            NSScrollView *fileScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20,255,580,220)];
            fileScroll.hasVerticalScroller = YES;
            s_transferFiles = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,560,200)];
            s_transferFiles.editable = NO; s_transferFiles.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
            fileScroll.documentView = s_transferFiles;
            NSScrollView *historyScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20,55,580,180)];
            historyScroll.hasVerticalScroller = YES;
            s_transferHistory = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,560,160)];
            s_transferHistory.editable = NO; s_transferHistory.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
            historyScroll.documentView = s_transferHistory;
            NSButton *toggleFolder = [[NSButton alloc] initWithFrame:NSMakeRect(250,12,120,28)];
            toggleFolder.title=@"展开/折叠目录"; toggleFolder.bezelStyle=NSBezelStyleRounded;
            toggleFolder.target=s_ctrl; toggleFolder.action=@selector(doToggleFolder:);
            s_retryButton = [[NSButton alloc] initWithFrame:NSMakeRect(380,12,110,28)];
            s_retryButton.title=@"重试选中文件"; s_retryButton.bezelStyle=NSBezelStyleRounded;
            s_retryButton.target=s_ctrl; s_retryButton.action=@selector(doRetryFile:);
            NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(500,12,100,28)];
            cancel.title=@"取消传输"; cancel.bezelStyle=NSBezelStyleRounded;
            cancel.target=s_ctrl; cancel.action=@selector(doCancelXfer:);
            [v addSubview:s_transferTitle]; [v addSubview:s_transferDetail];
            [v addSubview:s_transferCounts]; [v addSubview:s_transferProgress]; [v addSubview:fileScroll]; [v addSubview:historyScroll]; [v addSubview:toggleFolder]; [v addSubview:s_retryButton]; [v addSubview:cancel];
            [NSTimer scheduledTimerWithTimeInterval:0.4 repeats:YES block:^(NSTimer *timer) {
                (void)timer; transfer_window_refresh();
            }];
        }
        transfer_window_refresh();
        [NSApp activateIgnoringOtherApps:YES];
        [s_transferWindow makeKeyAndOrderFront:nil];
    });
}

void app_run(void) {
    int guard = hop_watchdog_begin("macOS main run loop", 30000);
    NSTimer *timer = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) {
        (void)t; hop_update_report_ready(HOP_VERSION); hop_watchdog_ping(guard);
    }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    [NSApp run];
    [timer invalidate];
    hop_watchdog_end(guard);
}
