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
static DWORD s_hook_tick, s_last_reinstall;
static int s_buttons[3];             /* remote buttons currently held */
static BYTE s_physical_buttons[3];   /* physical buttons, including while sharing */
#define HOP_LOCAL_MOUSE_TAG ((ULONG_PTR)0x484F5044u)
static void send_button(int btn, int down);
static void inject_local_button(int btn,int down) {
    static const DWORD dn[]={MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_MIDDLEDOWN,MOUSEEVENTF_RIGHTDOWN};
    static const DWORD up[]={MOUSEEVENTF_LEFTUP,MOUSEEVENTF_MIDDLEUP,MOUSEEVENTF_RIGHTUP};
    if(btn<0||btn>2)return;
    INPUT in;memset(&in,0,sizeof(in));in.type=INPUT_MOUSE;
    in.mi.dwFlags=down?dn[btn]:up[btn];in.mi.dwExtraInfo=HOP_LOCAL_MOUSE_TAG;
    (void)SendInput(1,&in,sizeof(in));
}
static int     s_sharing;
static POINT   s_anchor;      /* SHARING 时鼠标锁定点 */
static POINT   s_last;
static int     s_have_last;
static int     s_edge_pending;    /* 贴边计时器已启动 */
static DWORD   s_cooldown_until;  /* 在此之前不判定边缘 */

/* ---- 枚举显示器：是否有 X<0 的屏 ---- */
struct mon_ctx { int has_left; };
static BOOL CALLBACK mon_proc(HMONITOR h, HDC hdc, LPRECT r, LPARAM lp) {
    (void)h; (void)hdc;
    struct mon_ctx *c = (struct mon_ctx *)lp;
    if (r->left < 0) c->has_left = 1;
    return TRUE;
}

/* 计算移交边缘的坐标与方向。两个出参总是被写入，返回值只表示"边缘移交是否启用"。
 * 这样调用方不需要先判断返回值再取坐标 —— 未初始化值没有生存空间。 */
static int edge_boundary(int *edge_x, int *is_left) {
    static struct mon_ctx c;
    static DWORD checked;
    static int cached;
    DWORD now = GetTickCount();
    if (!cached || (DWORD)(now - checked) >= 2000) {
        c.has_left = 0;
        EnumDisplayMonitors(NULL, NULL, mon_proc, (LPARAM)&c);
        checked = now; cached = 1;
    }

    *is_left = (g_app.cfg.server_edge == HOP_EDGE_LEFT || g_app.cfg.server_edge==HOP_EDGE_TOP);
    int vertical=g_app.cfg.server_edge>=HOP_EDGE_TOP;
    if (*is_left)
        *edge_x = GetSystemMetrics(vertical?SM_YVIRTUALSCREEN:SM_XVIRTUALSCREEN);
    else
        *edge_x = GetSystemMetrics(vertical?SM_YVIRTUALSCREEN:SM_XVIRTUALSCREEN) +
                  GetSystemMetrics(vertical?SM_CYVIRTUALSCREEN:SM_CXVIRTUALSCREEN) - 1;

    return vertical || !c.has_left; /* 左侧有实体屏时仅禁用水平左/右移交 */
}

static int axis(POINT p){return g_app.cfg.server_edge>=HOP_EDGE_TOP?p.y:p.x;}

static int on_edge(int x, int edge_x, int is_left) {
    return is_left ? (x <= edge_x) : (x >= edge_x);
}

/* 冷却期判定。用有符号差值比较，避免 GetTickCount 回绕；
 * 0 表示"从未设过冷却"，直接放行（否则开机超过 24.8 天后会被误判为冷却中）。 */
static int in_cooldown(void) {
    if (!s_cooldown_until) return 0;
    return (LONG)(GetTickCount() - s_cooldown_until) < 0;
}

/* ---- 进入 / 退出共享 ---- */
static void send_enter(int y) {
    uint8_t p[2];
    hop_put_be16(p, (uint16_t)(int16_t)y);
    net_server_send(g_app.net, HOP_MSG_MOUSE_ENTER, p, 2);
}

static void enter_sharing(POINT trigger) {
    int edge_x, is_left;
    int margin = g_app.cfg.edge_margin_px;

    edge_boundary(&edge_x, &is_left);        /* 出参总是被写入 */
    int vertical=g_app.cfg.server_edge>=HOP_EDGE_TOP;
    int extent=GetSystemMetrics(vertical?SM_CYVIRTUALSCREEN:SM_CXVIRTUALSCREEN);
    if(extent<=0)extent=GetSystemMetrics(vertical?SM_CYSCREEN:SM_CXSCREEN);
    if (margin < 0) margin = 0;
    if (margin > extent/2) margin = extent/2;

    s_sharing = 1;
    s_anchor = trigger;
    /* 钳制点内移到边缘内侧，留出继续推的空间 */
    if(g_app.cfg.server_edge>=HOP_EDGE_TOP)s_anchor.y=is_left?edge_x+margin:edge_x-margin;
    else s_anchor.x = is_left ? edge_x + margin : edge_x - margin;
    s_last = s_anchor;
    s_have_last = 1;
    g_app.state = HOP_LINK_SHARING;
    kbd_begin_sharing();
    for(int i=0;i<3;++i)if(s_physical_buttons[i]){
        send_button(i,1);
        inject_local_button(i,0);
    }
    SetCursorPos(s_anchor.x, s_anchor.y);

    /* 移交后短时间内不再判定边缘，避免在接缝处来回弹 */
    s_cooldown_until = GetTickCount() + (DWORD)g_app.cfg.switch_cooldown_ms;

    /* 把 Windows y 按屏幕高度比例换算成 Mac 坐标，让鼠标在对应高度进入 */
    int mac_y = g_app.cfg.server_edge>=HOP_EDGE_TOP?trigger.x:trigger.y;
    int vertical=g_app.cfg.server_edge>=HOP_EDGE_TOP;
    int remote=vertical?g_app.peer_w:g_app.peer_h,local=vertical?g_app.cfg.screen_w:g_app.cfg.screen_h;
    if(remote>0&&local>0)mac_y=(int)((long)mac_y*remote/local);
    send_enter(mac_y);

    ui_refresh(&g_app);
    hop_log(HOP_LOG_INFO, "mouse -> Mac (y=%d)", mac_y);
}
static void enter_local(void) {
    int edge_x, is_left;
    POINT p;

    /* 先结束对端输入，再把仍物理按住的键鼠恢复到本机。 */
    for (int i = 0; i < 3; ++i) if (s_buttons[i]) send_button(i, 0);
    s_sharing = 0;
    g_app.state = HOP_LINK_LOCAL;
    kbd_end_sharing();
    for(int i=0;i<3;++i)if(s_physical_buttons[i])inject_local_button(i,1);

    /* 把鼠标放回边缘内侧，避免立刻又触发移交。
     * 方向必须跟着 server_edge 走：右边缘时要往回退，不是往前推。 */
    edge_boundary(&edge_x, &is_left);
    p = s_anchor;
    if(g_app.cfg.server_edge>=HOP_EDGE_TOP)p.y+=is_left?2:-2;else p.x += is_left ? 2 : -2;
    SetCursorPos(p.x, p.y);
    s_have_last = 0;
    /* 收回后同样进入冷却：不然"推出去又被弹回来"会变成连续往返 */
    s_cooldown_until = GetTickCount() + (DWORD)g_app.cfg.switch_cooldown_ms;
    ui_refresh(&g_app);
    hop_log(HOP_LOG_INFO, "mouse -> local");
}

void capture_set_sharing(int sharing) {
    if(g_app.cfg.role!=HOP_ROLE_SERVER)return;
    if (sharing && !s_sharing) {
        POINT pt;
        GetCursorPos(&pt);
        enter_sharing(pt);
    } else if (!sharing && s_sharing) {
        enter_local();
    }
}

void capture_force_toggle(void) {
    if(g_app.cfg.role!=HOP_ROLE_SERVER)return;
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
    if (btn >= 0 && btn < 3) s_buttons[btn] = down;
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
    s_hook_tick = GetTickCount();
    MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
    POINT pt = m->pt;
    if((m->flags&LLMHF_INJECTED) && m->dwExtraInfo==HOP_LOCAL_MOUSE_TAG)goto pass;
    if(!(m->flags&LLMHF_INJECTED)){
        switch(wp){
            case WM_LBUTTONDOWN:s_physical_buttons[HOP_BTN_LEFT]=1;break;
            case WM_LBUTTONUP:s_physical_buttons[HOP_BTN_LEFT]=0;break;
            case WM_MBUTTONDOWN:s_physical_buttons[HOP_BTN_MIDDLE]=1;break;
            case WM_MBUTTONUP:s_physical_buttons[HOP_BTN_MIDDLE]=0;break;
            case WM_RBUTTONDOWN:s_physical_buttons[HOP_BTN_RIGHT]=1;break;
            case WM_RBUTTONUP:s_physical_buttons[HOP_BTN_RIGHT]=0;break;
        }
    }

    if (s_sharing) {
        switch (wp) {
            case WM_MOUSEMOVE: {
                /* SetCursorPos 钳回锚点会产生注入事件，必须忽略，
                 * 否则会把"拉回"误当成真实移动，造成抖动/自动偏移。 */
                if (m->flags & LLMHF_INJECTED) return 1;
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

    /* LOCAL：边缘检测
     *
     * 贴边后不是立刻移交，而是启动 IDT_EDGE 计时器，够 switch_delay_ms
     * 仍然停在边缘才移交 —— 这就是 Deskflow 的 switchDelay 语义。
     *
     * 为什么必须用计时器而不能就地判断时间：
     * 光标顶到 x=0 之后位置不再变化，系统不再投递 WM_MOUSEMOVE，
     * 事件驱动的"累计停留时长"永远等不到下一次事件，计时就死了。
     * （旧实现 switch_push_ms 正是这么坏掉的：设成正值等于彻底禁用移交。） */
    if (wp == WM_MOUSEMOVE) {
        int edge_x, is_left;
        int delay = g_app.cfg.switch_delay_ms;

        if (edge_boundary(&edge_x, &is_left) &&
            net_server_has_client(g_app.net) &&
            !in_cooldown()) {

            int dx = s_have_last ? (axis(pt) - axis(s_last)) : 0;
            int toward = is_left ? (dx < 0) : (dx > 0);

            int at_edge = on_edge(axis(pt), edge_x, is_left);

            if (at_edge && toward) {
                if (delay <= 0) {
                    enter_sharing(pt);
                    return 1;
                }
                if (!s_edge_pending) {
                    s_edge_pending = 1;
                    SetTimer(g_app.hwnd, IDT_EDGE, (UINT)delay, NULL);
                }
            } else if (!at_edge && s_edge_pending) {
                /* 只有真的**离开**边缘才取消计时。
                 *
                 * 旧代码是 "else if (s_edge_pending)"，只要 toward 为假就取消。
                 * 但光标顶到边缘后位置被系统钳住不再变化，后续每个事件算出的
                 * dx 都是 0（toward 恒为假），于是计时器在启动后的下一个鼠标
                 * 事件就被杀掉 —— 移交永远等不到，键盘也就永远不跟过去。
                 * 停在边缘上必须保持计时。 */
                s_edge_pending = 0;
                KillTimer(g_app.hwnd, IDT_EDGE);
            }
        }

        s_last = pt;
        s_have_last = 1;
    }

pass:
    return CallNextHookEx(s_hook, code, wp, lp);
}

/* 停留时间到：再确认一次光标是否仍在边缘，是则移交 */
void capture_on_edge_timer(void) {
    POINT pt;
    int edge_x, is_left;

    s_edge_pending = 0;
    if (g_app.state != HOP_LINK_LOCAL) return;
    if (!net_server_has_client(g_app.net)) return;
    if (!edge_boundary(&edge_x, &is_left)) return;
    if (in_cooldown()) return;

    GetCursorPos(&pt);
    if (!on_edge(axis(pt), edge_x, is_left)) return;

    enter_sharing(pt);
}

void capture_init(win_app *app) {
    (void)app;
    s_hook_tick = GetTickCount();
    s_sharing = 0;
    s_have_last = 0;
    s_edge_pending = 0;
    s_cooldown_until = 0;
    s_physical_buttons[HOP_BTN_LEFT]=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    s_physical_buttons[HOP_BTN_MIDDLE]=(GetAsyncKeyState(VK_MBUTTON)&0x8000)!=0;
    s_physical_buttons[HOP_BTN_RIGHT]=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
    s_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc_hook,
                               g_app.inst, 0);
    if (s_hook) hop_log(HOP_LOG_INFO, "mouse hook installed");
    else hop_log(HOP_LOG_ERROR, "mouse hook failed: %lu", GetLastError());
}

void capture_shutdown(void) {
    if(s_sharing)enter_local();
    if (s_edge_pending) {
        s_edge_pending = 0;
        KillTimer(g_app.hwnd, IDT_EDGE);
    }
    if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = NULL; }
}

/* Best-effort recovery: only reinstall after input newer than both callbacks.
 * Idle machines must not churn hooks. All state stays on the UI thread. */
void capture_check_hooks(void) {
    LASTINPUTINFO info;
    info.cbSize = sizeof(info);
    DWORD now = GetTickCount();
    DWORD last = s_hook_tick;
    DWORD kt = kbd_hook_tick();
    if ((LONG)(kt - last) > 0) last = kt;
    if (!GetLastInputInfo(&info)) return;
    int stale = (DWORD)(now - info.dwTime) < 5000 && (LONG)(info.dwTime - last) > 2000;
    if ((!s_hook || stale) && (!s_last_reinstall || (DWORD)(now - s_last_reinstall) >= 30000)) {
        capture_set_sharing(0);
        if (s_hook) UnhookWindowsHookEx(s_hook);
        s_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc_hook, g_app.inst, 0);
        kbd_reinstall();
        s_hook_tick = s_last_reinstall = now;
        hop_log(s_hook ? HOP_LOG_WARN : HOP_LOG_ERROR, "capture: reinstalled input hooks (%s)",
                s_hook ? "mouse ok" : "mouse failed");
    }
}
