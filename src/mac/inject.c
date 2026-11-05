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

static struct {
    int sharing, btn_held[3];
    double mouse_x, mouse_y, peer_w, peer_h;
} s_input; /* control-dispatch owner; never read by UI/data threads */
void inject_set_peer_geometry(uint32_t w, uint32_t h) { s_input.peer_w = w; s_input.peer_h = h; }
static double s_w, s_h;
static int    s_inited;
static int    s_permission_guidance_requested;

/* 越过出口边缘的累计量，回到屏内立即清零。
 * 必须累计而不是看单次位移：慢推时每次事件只有几个像素，
 * 单次判定永远过不了阈值，表现为"怎么推都出不去"。 */
static double s_over;

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
    if (!e) return;
    CGEventSetFlags(e, s_flags);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

/* 服务端（Windows 物理像素）-> 本端坐标的位移缩放。
 * 当前两端都是 3840×2160，scale 为 1；显示器模式变了会自动跟。
 * peer 尺寸未知时退化为 1:1。 */
static double scale_x(void) {
    return s_input.peer_w > 0 ? s_w / s_input.peer_w : 1.0;
}
static double scale_y(void) {
    return s_input.peer_h > 0 ? s_h / s_input.peer_h : 1.0;
}

/* 死区宽度：配置项以 Windows 物理像素为单位，换算到本端坐标空间。
 * 至少 1 个单位，且不超过半屏 —— 否则会变成"永远出不去"。 */
static double margin(void) {
    int vertical = g_mac.cfg.client_edge >= HOP_EDGE_TOP;
    double extent = vertical ? s_h : s_w;
    double scale = vertical ? scale_y() : scale_x();
    double m = (double)g_mac.cfg.edge_margin_px * scale;
    if (m < 1.0) m = 1.0;
    if (m > extent / 2) m = extent / 2;
    return m;
}

/* 进入位置：边缘内侧的死区点 */
static double enter_x(void) {
    double m = margin();
    return g_mac.cfg.client_edge == HOP_EDGE_LEFT ? m : s_w - 1 - m;
}

void inject_init(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    CGRect bounds=CGDisplayBounds(d);
    s_w=bounds.size.width;s_h=bounds.size.height;
    g_mac.screen_w = s_w;
    g_mac.screen_h = s_h;
    s_inited = 1;

    /* 注入权限预检。
     *
     * CGEventPost 在没有「辅助功能」权限时**静默丢弃**事件，返回 void，
     * 没有任何错误可查。表现就是"键鼠切过去了，Mac 毫无反应"，而日志里
     * 一个字都没有 —— 排查时会被误判成消息没送到。
     *
     * CGPreflightPostEventAccess 只查询、不弹窗（10.15+），适合放在初始化里。
     * 真缺权限时必须把话说清楚，否则用户根本不知道该去勾哪个开关。 */
    if (CGPreflightPostEventAccess()) {
        hop_log(HOP_LOG_INFO, "inject: 辅助功能 / 事件投递权限已授予");
    } else {
        hop_log(HOP_LOG_ERROR,
                "inject: 缺少 macOS 辅助功能 / 事件投递权限，CGEventPost 会被系统静默丢弃");
        if (!s_permission_guidance_requested) {
            s_permission_guidance_requested = 1;
            mac_request_accessibility_permission();
        }
    }
}

/* 把仍按着的修饰键全部松开：断线时不能让 Mac 卡住 Cmd */
static uint8_t s_held_keys[65536 / 8];
static uint8_t s_paste_suppressed[65536 / 8];
void inject_release_all(void) {
    static const uint16_t pair[4][2] = {
        { 0x38, 0x3C },   /* shift L/R   */
        { 0x3B, 0x3E },   /* control L/R */
        { 0x3A, 0x3D },   /* option L/R  */
        { 0x37, 0x36 }    /* command L/R */
    };
    int i, j;
    for (unsigned kc = 0; kc < 65536u; ++kc) {
        if (!(s_held_keys[kc / 8] & (1u << (kc % 8)))) continue;
        s_held_keys[kc / 8] &= (uint8_t)~(1u << (kc % 8));
        if (hop_mac_key_is_mod((uint16_t)kc)) continue;
        CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, 0);
        post(e);
    }
    for (i = 0; i < 3; ++i) {
        if (!s_input.btn_held[i]) continue;
        CGPoint pt = CGPointMake(s_input.mouse_x, s_input.mouse_y);
        CGMouseButton button = i == HOP_BTN_RIGHT ? kCGMouseButtonRight :
                               i == HOP_BTN_MIDDLE ? kCGMouseButtonCenter : kCGMouseButtonLeft;
        CGEventType type = i == HOP_BTN_RIGHT ? kCGEventRightMouseUp :
                           i == HOP_BTN_MIDDLE ? kCGEventOtherMouseUp : kCGEventLeftMouseUp;
        post(CGEventCreateMouseEvent(NULL, type, pt, button));
        s_input.btn_held[i] = 0;
    }

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
    memset(s_paste_suppressed,0,sizeof(s_paste_suppressed));
    s_input.sharing = 0;
    s_inited = 0;
    s_over = 0;
    s_input.mouse_x = 0;
    s_input.mouse_y = 0;
    for (int i = 0; i < 3; ++i) s_input.btn_held[i] = 0;
}

/* 按住左键移动必须发 LeftMouseDragged，否则在 Mac 上拖不动东西 */
static CGEventType move_type(void) {
    if (s_input.btn_held[HOP_BTN_LEFT])   return kCGEventLeftMouseDragged;
    if (s_input.btn_held[HOP_BTN_RIGHT])  return kCGEventRightMouseDragged;
    if (s_input.btn_held[HOP_BTN_MIDDLE]) return kCGEventOtherMouseDragged;
    return kCGEventMouseMoved;
}

static void post_move(CGPoint pt, CGMouseButton btn) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, pt, btn);
    CGEventSetType(e, move_type());
    post(e);
}

/* 进入共享：边缘内侧死区、指定 y（已是本端坐标），并立即定位物理鼠标 */
void inject_enter(int y) {
    if (!s_inited) inject_init();
    s_over = 0;
    s_input.mouse_x = enter_x();
    s_input.mouse_y = y;
    if(g_mac.cfg.client_edge>=HOP_EDGE_TOP){s_input.mouse_x=y;s_input.mouse_y=g_mac.cfg.client_edge==HOP_EDGE_TOP?margin():s_h-1-margin();}
    if (s_input.mouse_x < 0) s_input.mouse_x = 0;
    if (s_input.mouse_x > s_w - 1) s_input.mouse_x = s_w - 1;
    if (s_input.mouse_y < 0) s_input.mouse_y = 0;
    if (s_input.mouse_y > s_h - 1) s_input.mouse_y = s_h - 1;
    s_input.sharing = 1;
    post_move(CGPointMake(s_input.mouse_x, s_input.mouse_y), kCGMouseButtonLeft);
    statusbar_update(1, 1);

    /* 打出落点：这是"键鼠到底有没有切过来"唯一的服务端可观测证据。
     * 没有这行说明 MOUSE_ENTER 没送到，问题在传输而不是注入。 */
    hop_log(HOP_LOG_INFO, "inject: enter (%.0f, %.0f) screen %.0fx%.0f",
            s_input.mouse_x, s_input.mouse_y, s_w, s_h);
}

static void leave_edge(void) {
    s_over = 0;
    inject_release_all();
    s_input.sharing = 0;
    net_send(HOP_MSG_EDGE_LEAVE,
             (uint8_t[]){ (uint8_t)g_mac.cfg.client_edge }, 1);
    statusbar_update(1, 0);
}

void inject_move(int dx, int dy) {
    double sx, sy, nx, ny, m;

    if (!s_input.sharing) inject_enter((int)(s_h / 2));  /* 未收到 ENTER 的兜底 */
    if (!s_inited) inject_init();

    /* 把 Windows 物理像素增量换算成本端坐标 */
    sx = scale_x();
    sy = scale_y();
    nx = s_input.mouse_x + dx * sx;
    ny = s_input.mouse_y + dy * sy;
    m  = margin();

    /* 退出边缘：越界量累计够 margin 才收回；中途回到屏内立即清零。
     * 这是 Deskflow switchDelay 的等价物 —— 那边用"停留时长"，
     * 这边用"越界距离"，因为本端坐标由我们自己维护，距离是确定量，
     * 不需要引入定时器。两者效果一致：贴边不会误触发，必须真的推出去。 */
    int edge=g_mac.cfg.client_edge;
    double beyond=edge==0?-nx:edge==1?nx-s_w+1:edge==2?-ny:ny-s_h+1;
    if(beyond>0){s_over+=beyond;if(s_over>=m){leave_edge();return;}}else s_over=0;
    if(nx<0)nx=0;if(nx>s_w-1)nx=s_w-1;
    s_input.mouse_x = nx;
    s_input.mouse_y = ny;
    if (s_input.mouse_y < 0) s_input.mouse_y = 0;
    if (s_input.mouse_y > s_h - 1) s_input.mouse_y = s_h - 1;

    post_move(CGPointMake(s_input.mouse_x, s_input.mouse_y), kCGMouseButtonLeft);
}

void inject_button(int button, int down) {
    CGPoint pt;
    CGMouseButton b;
    CGEventType t;
    CGEventRef e;

    if (button < 0 || button >= 3) return;
    if (!s_input.sharing && down) inject_enter((int)(s_h / 2));
    pt = CGPointMake(s_input.mouse_x, s_input.mouse_y);

    b = button == HOP_BTN_RIGHT  ? kCGMouseButtonRight
      : button == HOP_BTN_MIDDLE ? kCGMouseButtonCenter
      : kCGMouseButtonLeft;

    if (b == kCGMouseButtonRight)
        t = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    else if (b == kCGMouseButtonCenter)
        t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    else
        t = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;

    if (button >= 0 && button < 3) s_input.btn_held[button] = down;

    e = CGEventCreateMouseEvent(NULL, t, pt, b);
    post(e);
}

void inject_wheel(int dx, int dy) {
    CGEventRef e;
    if (!s_input.sharing) inject_enter((int)(s_h / 2));
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
    uint8_t mask=(uint8_t)(1u<<(kc%8));
    if(!down && (s_paste_suppressed[kc/8]&mask)) {
        s_paste_suppressed[kc/8]&=(uint8_t)~mask;
        return;
    }
    if(down && kc==0x09 && (s_modn[1]||s_modn[3]) && clipboard_request_remote_paste()) {
        const uint16_t mods[]={0x37,0x36,0x3B,0x3E};
        s_paste_suppressed[kc/8]|=mask;
        for(unsigned mi=0;mi<sizeof(mods)/sizeof(mods[0]);++mi) {
            uint16_t mk=mods[mi];uint8_t mm=(uint8_t)(1u<<(mk%8));
            if(s_held_keys[mk/8]&mm) {
                int mb=hop_mac_key_is_mod(mk),ix=mod_index(mb);
                s_held_keys[mk/8]&=(uint8_t)~mm;
                if(ix>=0&&s_modn[ix])s_modn[ix]--;
                recompute_flags();
                CGEventRef up=CGEventCreateKeyboardEvent(NULL,(CGKeyCode)mk,0);
                if(up){CGEventSetType(up,kCGEventFlagsChanged);post(up);}
                s_paste_suppressed[mk/8]|=mm;
            }
        }
        return;
    }
    bit = hop_mac_key_is_mod(kc);
    int was_held = (s_held_keys[kc / 8] & (1u << (kc % 8))) != 0;
    if (down) s_held_keys[kc / 8] |= (uint8_t)(1u << (kc % 8));
    else s_held_keys[kc / 8] &= (uint8_t)~(1u << (kc % 8));

    /* 大写锁定是"切换"语义，只在按下时翻转 AlphaShift */
    if (bit == HOP_MOD_CAPS) {
        if (!down || was_held) return;
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
            if (down && !was_held) { if (s_modn[i] < 255) s_modn[i]++; }
            else if (!down && was_held) { if (s_modn[i]) s_modn[i]--; }
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
