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
@end
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
static NSTextView *s_transferHistory;
static uint32_t s_historyBatch;
static uint32_t s_notifiedBatch;
static int s_notifiedPhase;

static void transfer_notify_if_terminal(const hop_xfer_status *st) {
    if (!st || st->active || st->batch == 0) return;
    if (st->phase != HOP_XFER_COMPLETED && st->phase != HOP_XFER_PARTIAL &&
        st->phase != HOP_XFER_FAILED && st->phase != HOP_XFER_CANCELLED) return;
    if (s_notifiedBatch == st->batch && s_notifiedPhase == st->phase) return;
    s_notifiedBatch = st->batch;
    s_notifiedPhase = st->phase;
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

        set_appearance(0, 0);    }
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
    hop_xfer_history_entry history[HOP_XFER_HISTORY_LIMIT];
    int historyCount = xfer_get_history(history, HOP_XFER_HISTORY_LIMIT);
    if (s_transferHistory && historyCount > 0 && s_historyBatch != history[0].batch) {
        s_historyBatch = history[0].batch;
        NSMutableString *rows = [NSMutableString string];
        for (int i = 0; i < historyCount; ++i) {
            NSString *name = history[i].name[0] ? [NSString stringWithUTF8String:history[i].name] : @"文件任务";
            NSString *phaseText = history[i].phase == HOP_XFER_COMPLETED ? @"完成" :
                                  history[i].phase == HOP_XFER_PARTIAL ? @"部分失败" :
                                  history[i].phase == HOP_XFER_CANCELLED ? @"已取消" : @"失败";
            NSString *dirText = history[i].direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收";
            [rows appendFormat:@"%@  %@  %@  文件 %u/%u  %@\n", dirText, phaseText, name,
                history[i].files_done, history[i].files_total,
                [NSByteCountFormatter stringFromByteCount:(long long)history[i].bytes_total countStyle:NSByteCountFormatterFile]];
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
    s_transferCounts.stringValue = [NSString stringWithFormat:@"文件 %u / %u    数据 %@ / %@",
        st.files_done, st.files_total,
        [NSByteCountFormatter stringFromByteCount:(long long)st.bytes_done countStyle:NSByteCountFormatterCountStyleFile],
        [NSByteCountFormatter stringFromByteCount:(long long)st.bytes_total countStyle:NSByteCountFormatterCountStyleFile]];
    s_transferProgress.doubleValue = st.bytes_total ? MIN(100.0, 100.0 * (double)st.bytes_done / (double)st.bytes_total) : (st.active ? 0 : 100);
}

void statusbar_show_transfers(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!s_transferWindow) {
            s_transferWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,620,430)
                styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable)
                backing:NSBackingStoreBuffered defer:NO];
            s_transferWindow.title = @"HopDesk · 传输管理器";
            NSView *v = s_transferWindow.contentView;
            s_transferTitle = transfer_label(@"暂无传输任务", NSMakeRect(20,390,580,22), 15);
            s_transferDetail = transfer_label(@"等待文件传输", NSMakeRect(20,360,580,20), 12);
            s_transferCounts = transfer_label(@"文件 0 / 0    数据 0 B / 0 B", NSMakeRect(20,335,580,20), 12);
            s_transferProgress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20,310,580,12)];
            s_transferProgress.minValue=0; s_transferProgress.maxValue=100; s_transferProgress.indeterminate=NO;
            NSScrollView *historyScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20,55,580,240)];
            historyScroll.hasVerticalScroller = YES;
            s_transferHistory = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,560,220)];
            s_transferHistory.editable = NO; s_transferHistory.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
            historyScroll.documentView = s_transferHistory;
            NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(500,12,100,28)];
            cancel.title=@"取消传输"; cancel.bezelStyle=NSBezelStyleRounded;
            cancel.target=s_ctrl; cancel.action=@selector(doCancelXfer:);
            [v addSubview:s_transferTitle]; [v addSubview:s_transferDetail];
            [v addSubview:s_transferCounts]; [v addSubview:s_transferProgress]; [v addSubview:historyScroll]; [v addSubview:cancel];
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
