/* =====================================================================
 *  HopDesk - macOS 输入注入（纯 C，CoreGraphics）
 *
 *  维护虚拟坐标，按相对位移累加，用 CGEvent 注入。
 *  鼠标到达 client_edge（默认右边缘）并继续向外 -> EDGE_LEAVE 收回。
 *  需要 Accessibility 权限。
 *
 *  修饰键处理（阶段 2）：
 *    服务端把物理 Ctrl 映射成 Command 键码，所以这里收到的是"普通按键"。
 *    修饰键必须发 kCGEventFlagsChanged 而不是 keyDown —— 否则系统不会更新
 *    全局修饰状态，Cmd+C 这类快捷键不生效。
 *    同时维护 flags 掩码并挂到每个事件上（含鼠标），这样 Cmd+拖动 也对。
 * ===================================================================== */
#include "mac_app.h"

#include <ApplicationServices/ApplicationServices.h>

static double s_w, s_h;
static int    s_inited;

/* 修饰键按下计数（左右各一，不能只用布尔 —— 同时按住两个同类修饰键时
 * 松开一个不能把整个修饰位清掉） */
static uint8_t      s_modn[4];   /* shift / control / alt / command */
static int          s_caps;
static CGEventFlags s_flags;

static void recompute_flags(void) {
    CGEventFlags f = 0;
    if (s_modn[0]) f |= kCGEventFlagMaskShift;
    if (s_modn[1]) f |= kCGEventFlagMaskControl;
    if (s_modn[2]) f |= kCGEventFlagMaskAlternate;
    if (s_modn[3]) f |= kCGEventFlagMaskCommand;
    if (s_caps)    f |= kCGEventFlagMaskAlphaShift;
    s_flags = f;
}

static int mod_index(int bit) {
    switch (bit) {
        case HOP_MOD_SHIFT:   return 0;
        case HOP_MOD_CONTROL: return 1;
        case HOP_MOD_ALT:     return 2;
        case HOP_MOD_COMMAND: return 3;
        default:              return -1;
    }
}

static void post(CGEventRef e) {
    CGEventSetFlags(e, s_flags);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

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

/* 把仍按着的修饰键全部松开：断线时不能让 Mac 卡住 Cmd */
void inject_release_all(void) {
    static const uint16_t pair[4][2] = {
        { 0x38, 0x3C },   /* shift L/R   */
        { 0x3B, 0x3E },   /* control L/R */
        { 0x3A, 0x3D },   /* option L/R  */
        { 0x37, 0x36 }    /* command L/R */
    };
    int i, j;

    for (i = 0; i < 4; ++i) {
        if (!s_modn[i]) continue;
        s_modn[i] = 0;
        recompute_flags();
        for (j = 0; j < 2; ++j) {
            CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)pair[i][j], 0);
            CGEventSetType(e, kCGEventFlagsChanged);
            post(e);
        }
    }
}

void inject_reset(void) {
    inject_release_all();
    s_modn[0] = s_modn[1] = s_modn[2] = s_modn[3] = 0;
    s_caps = 0;
    recompute_flags();
    g_mac.sharing = 0;
    s_inited = 0;
    g_mac.mouse_x = 0;
    g_mac.mouse_y = 0;
    for (int i = 0; i < 3; ++i) g_mac.btn_held[i] = 0;
}

/* 按住左键移动必须发 LeftMouseDragged，否则在 Mac 上拖不动东西 */
static CGEventType move_type(void) {
    if (g_mac.btn_held[HOP_BTN_LEFT])   return kCGEventLeftMouseDragged;
    if (g_mac.btn_held[HOP_BTN_RIGHT])  return kCGEventRightMouseDragged;
    if (g_mac.btn_held[HOP_BTN_MIDDLE]) return kCGEventOtherMouseDragged;
    return kCGEventMouseMoved;
}

static void post_move(CGPoint pt, CGMouseButton btn) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, pt, btn);
    CGEventSetType(e, move_type());
    post(e);
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
    if (g_mac.cfg.client_edge == HOP_EDGE_LEFT) {
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

    post_move(CGPointMake(g_mac.mouse_x, g_mac.mouse_y), kCGMouseButtonLeft);
}

void inject_button(int button, int down) {
    CGPoint pt;
    CGMouseButton b;
    CGEventType t;
    CGEventRef e;

    if (!g_mac.sharing) inject_enter((int)(s_h / 2));
    pt = CGPointMake(g_mac.mouse_x, g_mac.mouse_y);

    b = button == HOP_BTN_RIGHT  ? kCGMouseButtonRight
      : button == HOP_BTN_MIDDLE ? kCGMouseButtonCenter
      : kCGMouseButtonLeft;

    if (b == kCGMouseButtonRight)
        t = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    else if (b == kCGMouseButtonCenter)
        t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    else
        t = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;

    if (button >= 0 && button < 3) g_mac.btn_held[button] = down;

    e = CGEventCreateMouseEvent(NULL, t, pt, b);
    post(e);
}

void inject_wheel(int dx, int dy) {
    CGEventRef e;
    if (!g_mac.sharing) inject_enter((int)(s_h / 2));
    /* Windows WHEEL_DELTA=120 ≈ 3 行；按像素单位换算 */
    e = CGEventCreateScrollWheelEvent2(NULL, kCGScrollEventUnitPixel, 2,
                                       (int32_t)dy, (int32_t)dx, 0);
    post(e);
}

/* ---- 阶段 2：键盘 ---- */
void inject_key(uint16_t kc, int down) {
    CGEventRef e;
    int bit, i;

    if (!s_inited) inject_init();
    bit = hop_mac_key_is_mod(kc);

    /* 大写锁定是"切换"语义，只在按下时翻转 AlphaShift */
    if (bit == HOP_MOD_CAPS) {
        if (!down) return;
        s_caps = !s_caps;
        recompute_flags();
        e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, 1);
        CGEventSetType(e, kCGEventFlagsChanged);
        post(e);
        return;
    }

    if (bit) {
        i = mod_index(bit);
        if (i >= 0) {
            if (down) { if (s_modn[i] < 255) s_modn[i]++; }
            else      { if (s_modn[i]) s_modn[i]--; }
            recompute_flags();
        }
        e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, down ? 1 : 0);
        CGEventSetType(e, kCGEventFlagsChanged);
        post(e);
        return;
    }

    e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, down ? 1 : 0);
    post(e);
}
