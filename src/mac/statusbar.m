/* =====================================================================
 *  HopDesk - macOS 状态栏（ObjC 薄层）
 *  Accessory 策略（无 Dock 图标），SF Symbol 反映连接 / 共享状态。
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include "mac_app.h"

/* ---- 退出动作的 target ---- */
@interface HopController : NSObject
- (void)doQuit:(id)sender;
- (void)doWake:(id)sender;
- (void)doInputWin:(id)sender;
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
