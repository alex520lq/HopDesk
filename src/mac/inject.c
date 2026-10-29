/* =====================================================================
 *  HopDesk - macOS 鼠标注入（纯 C，CoreGraphics）
 *
 *  维护虚拟坐标，按相对位移累加，用 CGEvent 注入。
 *  鼠标到达 client_edge（默认右边缘）并继续向外 -> EDGE_LEAVE 收回。
 *  需要 Accessibility 权限。
 * ===================================================================== */
#include "mac_app.h"

#include <ApplicationServices/ApplicationServices.h>
#include <math.h>

static double s_w, s_h;
static int    s_inited;

static double edge_x(void) {
    return g_mac.cfg.client_edge == HOP_EDGE_LEFT ? 0 : s_w - 1;
}

void inject_init(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    s_w = CGDisplayPixelsWide(d);
    s_h = CGDisplayPixelsHigh(d);
    g_mac.screen_w = s_w;
    g_mac.screen_h = s_h;
    s_inited = 1;
}

void inject_reset(void) {
    g_mac.sharing = 0;
    s_inited = 0;
    g_mac.mouse_x = 0;
    g_mac.mouse_y = 0;
    for (int i = 0; i < 3; ++i) g_mac.btn_held[i] = 0;
}

static void post_move(CGPoint pt, CGMouseButton btn) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, pt, btn);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

/* 进入共享：edge 处、指定 y（已是本端坐标），并立即定位物理鼠标 */
void inject_enter(int y) {
    if (!s_inited) inject_init();
    g_mac.mouse_x = edge_x();
    g_mac.mouse_y = y;
    if (g_mac.mouse_y < 0) g_mac.mouse_y = 0;
    if (g_mac.mouse_y > s_h - 1) g_mac.mouse_y = s_h - 1;
    g_mac.sharing = 1;
    post_move(CGPointMake(g_mac.mouse_x, g_mac.mouse_y), kCGMouseButtonLeft);
    statusbar_update(1, 1);
}

static void leave_edge(void) {
    g_mac.sharing = 0;
    net_send(HOP_MSG_EDGE_LEAVE,
             (uint8_t[]){ (uint8_t)g_mac.cfg.client_edge }, 1);
    statusbar_update(1, 0);
}

void inject_move(int dx, int dy) {
    if (!g_mac.sharing) inject_enter((int)(s_h / 2));  /* 未收到 ENTER 的兜底 */
    if (!s_inited) inject_init();

    /* 边缘离开判定（在更新位置前） */
    int left = g_mac.cfg.client_edge == HOP_EDGE_LEFT;
    if (left) {
        if (g_mac.mouse_x <= 0 && dx < 0) { leave_edge(); return; }
    } else {
        if (g_mac.mouse_x >= s_w - 1 && dx > 0) { leave_edge(); return; }
    }

    g_mac.mouse_x += dx;
    g_mac.mouse_y += dy;
    if (g_mac.mouse_x < 0) g_mac.mouse_x = 0;
    if (g_mac.mouse_x > s_w - 1) g_mac.mouse_x = s_w - 1;
    if (g_mac.mouse_y < 0) g_mac.mouse_y = 0;
    if (g_mac.mouse_y > s_h - 1) g_mac.mouse_y = s_h - 1;

    CGPoint pt = CGPointMake(g_mac.mouse_x, g_mac.mouse_y);
    post_move(pt, kCGMouseButtonLeft);
}

void inject_button(int button, int down) {
    if (!g_mac.sharing) inject_enter((int)(s_h / 2));
    CGPoint pt = CGPointMake(g_mac.mouse_x, g_mac.mouse_y);

    CGMouseButton b = button == HOP_BTN_RIGHT ? kCGMouseButtonRight
                    : button == HOP_BTN_MIDDLE ? kCGMouseButtonCenter
                    : kCGMouseButtonLeft;
    CGEventType t;
    if (b == kCGMouseButtonRight)
        t = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    else if (b == kCGMouseButtonCenter)
        t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    else
        t = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;

    g_mac.btn_held[button] = down;
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, pt, b);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

void inject_wheel(int dx, int dy) {
    if (!g_mac.sharing) inject_enter((int)(s_h / 2));
    /* Windows WHEEL_DELTA=120 ≈ 3 行；按像素单位换算 */
    int32_t py = (int32_t)(dy * 1.0);
    int32_t px = (int32_t)(dx * 1.0);
    CGEventRef e = CGEventCreateScrollWheelEvent2(
        NULL, kCGScrollEventUnitPixel, 2, py, px, 0);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}
