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
    NSImage *base = [NSImage imageWithSystemSymbolName:sym
                                accessibilityDescription:nil];
    NSImageSymbolConfiguration *cfg =
        [NSImageSymbolConfiguration configurationWithPaletteColors:@[color]];
    s_item.button.image = [base imageWithSymbolConfiguration:cfg];

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

void app_run(void) {
    [NSApp run];
}
