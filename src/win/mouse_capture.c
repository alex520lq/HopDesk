/* =====================================================================
 *  HopDesk - Windows 鼠标捕获 / 边缘移交
 *
 *  WH_MOUSE_LL 全局低级钩子：
 *    LOCAL   状态：放行，同时检测"鼠标贴边缘继续推" -> 移交
 *    SHARING 状态：吞掉事件，转发相对移动 / 点击 / 滚轮，鼠标钳回锁定点
 *
 *  边缘启用条件：枚举显示器，没有 X<0 的屏（左屏已切给 Mac，Windows 单屏）。
 * ===================================================================== */
#include "app.h"

#include <stdlib.h>
#include <string.h>

static HHOOK   s_hook;
static int     s_sharing;
static POINT   s_anchor;      /* SHARING 时鼠标锁定点 */
static POINT   s_last;
static int     s_have_last;
static DWORD   s_push_start;

/* ---- 枚举显示器：是否有 X<0 的屏 ---- */
struct mon_ctx { int has_left; };
static BOOL CALLBACK mon_proc(HMONITOR h, HDC hdc, LPRECT r, LPARAM lp) {
    (void)h; (void)hdc;
    struct mon_ctx *c = (struct mon_ctx *)lp;
    if (r->left < 0) c->has_left = 1;
    return TRUE;
}
static int edge_active(int *edge_x) {
    struct mon_ctx c; c.has_left = 0;
    EnumDisplayMonitors(NULL, NULL, mon_proc, (LPARAM)&c);
    if (c.has_left) return 0;                 /* 双屏：左屏在 Windows，不启用 */
    *edge_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    return 1;
}

/* ---- 进入 / 退出共享 ---- */
static void send_enter(int y) {
    uint8_t p[2];
    hop_put_be16(p, (uint16_t)(int16_t)y);
    net_server_send(g_app.net, HOP_MSG_MOUSE_ENTER, p, 2);
}

static void enter_sharing(POINT anchor) {
    s_sharing = 1;
    s_anchor = anchor;
    s_last = anchor;
    s_have_last = 1;
    g_app.state = HOP_LINK_SHARING;
    SetCursorPos(anchor.x, anchor.y);

    /* 把 Windows y 按屏幕高度比例换算成 Mac 坐标，让鼠标在对应高度进入 */
    int mac_y = anchor.y;
    if (g_app.peer_h > 0 && g_app.cfg.screen_h > 0)
        mac_y = (int)((long)anchor.y * g_app.peer_h / g_app.cfg.screen_h);
    send_enter(mac_y);

    ui_refresh(&g_app);
    hop_log(HOP_LOG_INFO, "mouse -> Mac (y=%d)", mac_y);
}
static void enter_local(void) {
    s_sharing = 0;
    g_app.state = HOP_LINK_LOCAL;
    /* 把鼠标放回边缘内侧，避免立刻又触发移交 */
    POINT p = s_anchor;
    p.x += 2;
    SetCursorPos(p.x, p.y);
    s_have_last = 0;
    ui_refresh(&g_app);
    hop_log(HOP_LOG_INFO, "mouse -> local");
}

void capture_set_sharing(int sharing) {
    if (sharing && !s_sharing) {
        POINT pt;
        GetCursorPos(&pt);
        enter_sharing(pt);
    } else if (!sharing && s_sharing) {
        enter_local();
    }
}

void capture_force_toggle(void) {
    if (!net_server_has_client(g_app.net)) {
        hop_log(HOP_LOG_WARN, "no client, cannot toggle");
        return;
    }
    if (s_sharing) enter_local();
    else {
        POINT pt; GetCursorPos(&pt);
        enter_sharing(pt);
    }
}

/* ---- 发送辅助 ---- */
static void send_move(int dx, int dy) {
    uint8_t p[4];
    hop_put_be16(p,     (uint16_t)(int16_t)dx);
    hop_put_be16(p + 2, (uint16_t)(int16_t)dy);
    net_server_send(g_app.net, HOP_MSG_MOUSE_MOVE, p, 4);
}
static void send_button(int btn, int down) {
    uint8_t p[2] = { (uint8_t)btn, (uint8_t)down };
    net_server_send(g_app.net, HOP_MSG_MOUSE_BUTTON, p, 2);
}
static void send_wheel(int dx, int dy) {
    uint8_t p[4];
    hop_put_be16(p,     (uint16_t)(int16_t)dx);
    hop_put_be16(p + 2, (uint16_t)(int16_t)dy);
    net_server_send(g_app.net, HOP_MSG_MOUSE_WHEEL, p, 4);
}

/* ---- 钩子 ---- */
static LRESULT CALLBACK mouse_proc_hook(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) goto pass;
    MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
    POINT pt = m->pt;

    if (s_sharing) {
        switch (wp) {
            case WM_MOUSEMOVE: {
                int dx = pt.x - s_anchor.x;
                int dy = pt.y - s_anchor.y;
                if (dx || dy) {
                    send_move(dx, dy);
                    SetCursorPos(s_anchor.x, s_anchor.y);
                }
                return 1;   /* 吞掉 */
            }
            case WM_LBUTTONDOWN: send_button(HOP_BTN_LEFT, 1); return 1;
            case WM_LBUTTONUP:   send_button(HOP_BTN_LEFT, 0); return 1;
            case WM_RBUTTONDOWN: send_button(HOP_BTN_RIGHT, 1); return 1;
            case WM_RBUTTONUP:   send_button(HOP_BTN_RIGHT, 0); return 1;
            case WM_MBUTTONDOWN: send_button(HOP_BTN_MIDDLE, 1); return 1;
            case WM_MBUTTONUP:   send_button(HOP_BTN_MIDDLE, 0); return 1;
            case WM_MOUSEWHEEL: {
                short wz = GET_WHEEL_DELTA_WPARAM(m->mouseData);
                send_wheel(0, (int)wz);
                return 1;
            }
            case WM_MOUSEHWHEEL: {
                short wz = GET_WHEEL_DELTA_WPARAM(m->mouseData);
                send_wheel((int)wz, 0);
                return 1;
            }
        }
        return 1;   /* 其余鼠标事件在共享时也不落到本机 */
    }

    /* LOCAL：边缘检测 */
    if (wp == WM_MOUSEMOVE) {
        int edge_x;
        if (edge_active(&edge_x) && net_server_has_client(g_app.net)) {
            int dx = s_have_last ? (pt.x - s_last.x) : 0;
            int toward = (g_app.cfg.server_edge == HOP_EDGE_LEFT) ? (dx < 0)
                                                                  : (dx > 0);
            if (pt.x <= edge_x && toward) {
                if (g_app.cfg.switch_push_ms > 0) {
                    DWORD now = GetTickCount();
                    if (!s_push_start) s_push_start = now;
                    if ((int)(now - s_push_start) >= g_app.cfg.switch_push_ms) {
                        s_push_start = 0;
                        enter_sharing(pt);
                        return 1;
                    }
                } else {
                    enter_sharing(pt);
                    return 1;
                }
            } else {
                s_push_start = 0;
            }
        }
        s_last = pt;
        s_have_last = 1;
    }

pass:
    return CallNextHookEx(s_hook, code, wp, lp);
}

void capture_init(win_app *app) {
    (void)app;
    s_sharing = 0;
    s_have_last = 0;
    s_push_start = 0;
    s_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc_hook,
                               g_app.inst, 0);
    if (s_hook) hop_log(HOP_LOG_INFO, "mouse hook installed");
    else hop_log(HOP_LOG_ERROR, "mouse hook failed: %lu", GetLastError());
}

void capture_shutdown(void) {
    if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = NULL; }
}
