/* =====================================================================
 *  HopDesk - macOS 状态栏（ObjC 薄层）
 *  Accessory 策略（无 Dock 图标），SF Symbol 反映连接 / 共享状态。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include "mac_app.h"
#include "split_agent.h"
#include "split_ui.h"
#include "xfer.h"
#include "../core/update_state.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

/* ---- 退出动作的 target ----
 * 必须显式声明 NSMenuItemValidation：10.14 之后 NSMenu 只对声明了该协议的
 * target 调用 validateMenuItem:，否则"取消当前传输"永远是亮的。 */
static void statusbar_handle_click(NSEvent *event);
static void open_main_window(void);
@interface HopController : NSObject <NSApplicationDelegate, NSMenuItemValidation>
- (void)doOpen:(id)sender;
- (void)doInputPermissions:(id)sender;
- (void)doStatusItemClick:(id)sender;
- (void)doQuit:(id)sender;
- (void)doWake:(id)sender;
- (void)doInputWin:(id)sender;
- (void)doTransfers:(id)sender;
- (void)doCheckUpdates:(id)sender;
@end
@implementation HopController
- (void)doOpen:(id)sender { (void)sender; open_main_window(); }
- (void)doInputPermissions:(id)sender {
    (void)sender;
    if (split_agent_is_active()) { open_main_window(); return; }
    mac_request_accessibility_permission();
}
- (void)doStatusItemClick:(id)sender { (void)sender; statusbar_handle_click(NSApp.currentEvent); }
- (void)doQuit:(id)sender {
    (void)sender;
    if (split_agent_is_active()) {
        if (!split_agent_stop_if_idle()) NSBeep();
        return;
    }
    main_ui_shutdown();
    clipboard_shutdown();
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
    (void)disp_switch_target(0);
}
/* 传了一半改主意：键鼠已经切走了、或者发现选错了文件 */
- (void)doTransfers:(id)sender { (void)sender; statusbar_show_transfers(); }
- (void)doCheckUpdates:(id)sender { (void)sender; statusbar_check_updates(); }
/* 菜单打开时自动校验：没有传输在进行就把"取消"变灰。
 * NSMenu 默认 autoenablesItems，走的就是这个入口。 */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if ([item action] == @selector(doQuit:) && split_agent_is_active()) return !xfer_busy();
    return YES;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible {
    (void)app; (void)visible;
    open_main_window();
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (!split_agent_stop_if_idle()) NSBeep();
    return NSTerminateCancel;
}
@end

static NSStatusItem *s_item;
static NSMenu *s_menu;
static NSMenuItem *s_state;
static HopController *s_ctrl;
static int s_status_connected, s_status_sharing;
static atomic_bool s_cursor_restore_pending = ATOMIC_VAR_INIT(false);

/* Synthetic mouse moves do not always cancel macOS's hide-until-mouse-moves
 * state after remote typing. Coalesce requests onto AppKit's main queue. */
void mac_cursor_restore(void) {
    if (atomic_exchange_explicit(&s_cursor_restore_pending, true, memory_order_acq_rel))
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSCursor setHiddenUntilMouseMoves:NO];
        atomic_store_explicit(&s_cursor_restore_pending, false, memory_order_release);
    });
}

static int s_last_icon_state = -1, s_last_icon_progress = -999;
static int s_last_icon_direction = -1, s_last_icon_phase = -1;
static int s_notifiedDirection;
static uint32_t s_notifiedBatch;
static int s_notifiedPhase;

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

void statusbar_notify_paste_ready(void) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ statusbar_notify_paste_ready(); });
        return;
    }
    NSUserNotification *note = [[NSUserNotification alloc] init];
    note.title = @"文件已准备好";
    note.informativeText = @"请选择目标文件夹，然后再次按 Command+V 粘贴。";
    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:note];
}
void statusbar_notify_image_limit(void) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ statusbar_notify_image_limit(); });
        return;
    }
    NSUserNotification *note = [[NSUserNotification alloc] init];
    note.title = @"图片未同步";
    note.informativeText = @"图片解码所需内存超过上限，请缩小图片后重试。";
    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:note];
}

static NSImage *make_status_image(int connected, int sharing, int progress) {
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(18.0, 18.0)];
    NSColor *color = sharing
        ? [NSColor colorWithSRGBRed:0.12 green:0.68 blue:0.40 alpha:1.0]
        : connected
            ? [NSColor colorWithSRGBRed:0.18 green:0.48 blue:0.92 alpha:1.0]
            : [NSColor colorWithSRGBRed:0.55 green:0.58 blue:0.62 alpha:1.0];
    [image lockFocus];
    NSBezierPath *network = [NSBezierPath bezierPath];
    network.lineWidth = 2.0;
    network.lineCapStyle = NSLineCapStyleRound;
    network.lineJoinStyle = NSLineJoinStyleRound;
    [network moveToPoint:NSMakePoint(4.0, 4.0)];
    [network lineToPoint:NSMakePoint(4.0, 14.0)];
    [network moveToPoint:NSMakePoint(14.0, 4.0)];
    [network lineToPoint:NSMakePoint(14.0, 14.0)];
    [network moveToPoint:NSMakePoint(4.0, 9.0)];
    [network lineToPoint:NSMakePoint(14.0, 9.0)];
    [color setStroke]; [network stroke]; [color setFill];
    for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) {
        CGFloat cx = x ? 14.0 : 4.0, cy = y ? 14.0 : 4.0;
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx-1.8, cy-1.8, 3.6, 3.6)] fill];
    }
    if (progress >= 0 || progress == -2) {
        NSPoint center = NSMakePoint(14.0, 4.0); CGFloat radius = 3.25;
        NSBezierPath *track = [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(center.x-radius, center.y-radius, radius*2.0, radius*2.0)];
        track.lineWidth = 1.25;
        [[NSColor colorWithSRGBRed:0.20 green:0.23 blue:0.28 alpha:0.75] setStroke]; [track stroke];
        CGFloat fraction = progress == -2 ? 0.28 : (CGFloat)progress / 100.0;
        NSBezierPath *arc = [NSBezierPath bezierPath];
        [arc appendBezierPathWithArcWithCenter:center radius:radius startAngle:90.0
            endAngle:90.0-360.0*fraction clockwise:YES];
        arc.lineWidth = 1.5; arc.lineCapStyle = NSLineCapStyleRound;
        [[NSColor colorWithSRGBRed:0.05 green:0.82 blue:0.96 alpha:1.0] setStroke]; [arc stroke];
    }
    [image unlockFocus]; image.template = NO; return image;
}

static void refresh_status_item(void) {
    if (!s_item || !s_item.button) return;
    int progress = -1, direction = 0, phase = 0;
    {
        hop_xfer_status_pair pair;
        xfer_get_status_pair(&pair);
        const hop_xfer_status *st = pair.primary_direction == HOP_XFER_DIR_SEND ? &pair.send : &pair.receive;
        if (st->active || st->queued_count > 0) {
            progress = st->bytes_total
                ? (int)MIN(100.0, 100.0*(double)st->bytes_done/(double)st->bytes_total) : -2;
            direction = st->direction;
            phase = st->phase;
        }
    }
    int disconnected = net_is_disconnected();
    int state = s_status_sharing ? 2 : (s_status_connected ? 1 : (disconnected ? -2 : 0));
    if (state == s_last_icon_state && progress == s_last_icon_progress &&
        direction == s_last_icon_direction && phase == s_last_icon_phase) return;
    s_last_icon_state=state; s_last_icon_progress=progress;
    s_last_icon_direction=direction; s_last_icon_phase=phase;
    s_item.button.image = make_status_image(s_status_connected, s_status_sharing, progress);
    s_item.button.imagePosition = NSImageLeft;
    s_item.button.title = progress >= 0 ? [NSString stringWithFormat:@"%d%%",progress] : (progress == -2 ? @"…" : @"");
    NSString *stateText = s_status_sharing ? @"键鼠共享中" :
        (s_status_connected ? @"已连接" : (disconnected ? @"已手动断开" : @"未连接"));
    if (progress >= 0) {
        NSString *directionText = direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收";
        s_item.button.toolTip = [NSString stringWithFormat:@"HopDesk · 正在%@ · %d%%",directionText,progress];
    } else if (progress == -2) s_item.button.toolTip = @"HopDesk · 正在准备传输";
    else s_item.button.toolTip = [NSString stringWithFormat:@"HopDesk · %@",stateText];
}

static void open_main_window(void) {
    if (!split_agent_is_active()) {
        statusbar_show_main();
        return;
    }
    static BOOL opening;
    if (opening) return;

    NSBundle *bundle = NSBundle.mainBundle;
    NSURL *bundleURL = bundle.bundleURL.URLByResolvingSymlinksInPath;
    for (NSRunningApplication *running in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle.bundleIdentifier]) {
        if (running.activationPolicy == NSApplicationActivationPolicyRegular &&
            [running.bundleURL.URLByResolvingSymlinksInPath isEqual:bundleURL] &&
            [running activateWithOptions:NSApplicationActivateAllWindows]) return;
    }

    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.createsNewApplicationInstance = YES;
    configuration.addsToRecentItems = NO;
    configuration.arguments = @[@"--ui"];
    opening = YES;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:bundleURL configuration:configuration
        completionHandler:^(NSRunningApplication *application, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                opening = NO;
                if (!application || error)
                    hop_log(HOP_LOG_WARN, "statusbar: failed to open the split UI");
            });
        }];
}

void statusbar_open_main(void) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ open_main_window(); });
        return;
    }
    open_main_window();
}

static void statusbar_handle_click(NSEvent *event) {
    if (event && event.type == NSEventTypeRightMouseDown) {
        if (s_menu && s_item.button) [NSMenu popUpContextMenu:s_menu withEvent:event forView:s_item.button];
        return;
    }
    open_main_window();
}

void statusbar_init(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        s_ctrl = [[HopController alloc] init];
        const BOOL split = split_agent_is_active();
        if (split) app.delegate = s_ctrl;

        s_item = [[NSStatusBar systemStatusBar]
                  statusItemWithLength:NSVariableStatusItemLength];

        s_menu = [[NSMenu alloc] init];
        NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"打开" action:@selector(doOpen:) keyEquivalent:@""];
        open.target = s_ctrl; [s_menu addItem:open];
        NSMenuItem *permissions = [[NSMenuItem alloc] initWithTitle:@"检查键鼠权限…" action:@selector(doInputPermissions:) keyEquivalent:@""];
        permissions.target = s_ctrl; [s_menu addItem:permissions];
        [s_menu addItem:[NSMenuItem separatorItem]];
        NSString *quitTitle = split ? @"停止 HopDesk 后台" : @"退出";
        NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:quitTitle action:@selector(doQuit:) keyEquivalent:@"q"];
        quit.target = s_ctrl; [s_menu addItem:quit];

        /* Left click opens the main window; right click opens the context menu. */
        s_item.button.target = s_ctrl;
        s_item.button.action = @selector(doStatusItemClick:);
        [s_item.button sendActionOn:(NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown)];
        refresh_status_item();
        /* The agent owns transfer state and the tray; progress and terminal
         * notifications must work even when no UI process exists. */
        [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            hop_xfer_status snapshot; memset(&snapshot, 0, sizeof(snapshot));
            xfer_get_status(&snapshot);
            transfer_notify_if_terminal(&snapshot);
            refresh_status_item();
        }];
    }
}

void statusbar_update(int connected, int sharing) {
    dispatch_async(dispatch_get_main_queue(), ^{
        s_status_connected = connected != 0;
        s_status_sharing = sharing != 0;
        refresh_status_item();
        if (split_agent_is_active()) {
            split_agent_set_status(connected, sharing);
            mac_capture_connected(connected);
            return;
        }
        mac_capture_connected(connected);
        main_ui_set_status(connected, sharing);
    });
}

void statusbar_show_transfers(void) {
    if (split_agent_is_active()) open_main_window();
    else main_ui_show_page(2);
}

/* Update controls are part of the main window. */
void statusbar_check_updates(void) {
    (void)split_ui_update_action(HOP_UPDATE_ACTION_CHECK, g_mac.cfg.update_channel);
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
