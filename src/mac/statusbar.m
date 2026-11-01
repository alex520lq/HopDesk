/* =====================================================================
 *  HopDesk - macOS 状态栏（ObjC 薄层）
 *  Accessory 策略（无 Dock 图标），SF Symbol 反映连接 / 共享状态。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include "mac_app.h"
#include "xfer.h"

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
@end
static void retry_selected_mac_file(void);
static void toggle_selected_mac_folder(void);
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
/* 菜单打开时自动校验：没有传输在进行就把"取消"变灰。
 * NSMenu 默认 autoenablesItems，走的就是这个入口。 */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if ([item action] == @selector(doCancelXfer:)) return xfer_busy() != 0;
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
    [NSApp run];
}
