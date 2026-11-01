/* =====================================================================
 *  HopDesk - Windows 入口
 *  单实例、隐藏消息窗口、托盘、网络、鼠标钩子。
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "update.h"
#include "../core/update_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

win_app g_app;
static int s_ui_guard;

/* 声明为 PerMonitorV2 DPI aware：统一使用物理像素，避免坐标虚拟化混乱。
 * 必须在创建任何窗口 / 取坐标之前调用。 */
static void set_dpi_aware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *set_ctx_t)(void *);
    set_ctx_t set_ctx = (set_ctx_t)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (set_ctx) {
        if (set_ctx((void *)-4)) return;   /* PER_MONITOR_AWARE_V2 */
        set_ctx((void *)-3);               /* PER_MONITOR_AWARE    */
        return;
    }
    typedef BOOL (WINAPI *set_aware_t)(void);
    set_aware_t set_aware = (set_aware_t)GetProcAddress(u, "SetProcessDPIAware");
    if (set_aware) set_aware();
}

/* exe 同目录下的文件路径 */
static void beside(wchar_t *out, int n, const wchar_t *name) {
    wchar_t *p;
    GetModuleFileNameW(g_app.inst, out, n);
    p = wcsrchr(out, L'\\');
    if (p) wcscpy(p + 1, name);
}

static void on_link(int up) {
    if (up) {
        if (up == 2 && g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
        if (g_app.state == HOP_LINK_DOWN) g_app.state = HOP_LINK_LOCAL;
    } else {
        if (g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
        g_app.state = HOP_LINK_DOWN;
        xfer_kick();
    }
    ui_refresh(&g_app);
}

/* HELLO: {tlen, token, nlen, name, w(4), h(4)} */
static void on_hello(const uint8_t *p, uint32_t len) {
    hop_hello_msg hello;
    /* The network thread validates the token before publishing a session. */
    if (!hop_hello_decode(p, len, &hello)) return;
    g_app.peer_w = (int)hello.width;
    g_app.peer_h = (int)hello.height;

    hop_log(HOP_LOG_INFO, "peer screen %dx%d", g_app.peer_w, g_app.peer_h);
}

static void on_recv(hop_recv *r) {
    switch (r->type) {
        case HOP_MSG_HELLO:
            on_hello(r->data, r->len);
            break;
        case HOP_MSG_EDGE_LEAVE:
            if (g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
            break;
        case HOP_MSG_CLIP_TEXT:
            clip_apply_remote(r->data, r->len);
            break;
        default:
            break;
    }
}

/* ---- 阶段 4：显示切换 ---- */

/* 切到 Mac 显示：先让 Mac 醒，再切显示器输入源。
 * 两步之间必须留时间 —— 输入源一切走，Windows 就看不到画面了。 */
static void disp_switch_to_mac(void) {
    uint8_t p[4];
    if (!net_server_has_client(g_app.net)) {
        hop_log(HOP_LOG_WARN, "no client, cannot switch display");
        return;
    }
    hop_put_be32(p, (uint32_t)g_app.cfg.disp_wake_sec);
    net_server_send(g_app.net, HOP_MSG_DISP_WAKE, p, 4);
    capture_set_sharing(0);          /* 画面即将走掉，键鼠先收回本机 */
    SetTimer(g_app.hwnd, IDT_DISP, DISP_WAKE_DELAY_MS, NULL);
}

/* 切回 Windows 显示：让 Mac 把输入源切回来；Mac 不可达就本机 DDC 硬切 */
static void disp_switch_to_win(void) {
    uint8_t p[2];
    hop_put_be16(p, g_app.cfg.disp_win_input);
    if (!net_server_send(g_app.net, HOP_MSG_DISP_INPUT, p, 2)) {
        hop_log(HOP_LOG_WARN, "client unreachable, falling back to local DDC");
        disp_set_input(g_app.cfg.disp_win_input);
    }
    capture_set_sharing(0);
}

static void disp_wake_mac(void) {
    uint8_t p[4];
    if (!net_server_has_client(g_app.net)) {
        hop_log(HOP_LOG_WARN, "no client, cannot wake display");
        return;
    }
    hop_put_be32(p, (uint32_t)g_app.cfg.disp_wake_sec);
    net_server_send(g_app.net, HOP_MSG_DISP_WAKE, p, 4);
}

static void disp_sleep_mac(void) {
    net_server_send(g_app.net, HOP_MSG_DISP_SLEEP, NULL, 0);
    capture_set_sharing(0);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_LINK:
            if ((unsigned long)lp == net_server_epoch(g_app.net)) on_link((int)wp);
            return 0;
        case WM_APP_RECV: {
            hop_recv *r = (hop_recv *)lp;
            if (r->epoch == net_server_epoch(g_app.net)) on_recv(r);
            free(r);
            return 0;
        }
        case WM_APP_CLIP:
            clip_on_worker_message((UINT)wp, (HGLOBAL)lp);
            return 0;
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT ||
                LOWORD(lp) == NIN_KEYSELECT) {
                ui_show_main(&g_app);
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                ui_show_menu(&g_app);
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            clip_on_update();
            return 0;
        case WM_TIMER:
            if (wp == IDT_HOOK) { hop_update_report_ready(HOP_VERSION); hop_watchdog_ping(s_ui_guard); capture_check_hooks(); ui_refresh(&g_app); return 0; }
            if (wp == IDT_XFER_NOTIFY) { ui_transfer_poll(); return 0; }
            if (wp == IDT_CLIP_RETRY) {
                clip_retry_pending();
                clip_on_update(); /* retry the latest local clipboard snapshot too */
                return 0;
            }
            if (wp == IDT_DISP) {
                KillTimer(h, IDT_DISP);
                disp_set_input(g_app.cfg.disp_mac_input);
                return 0;
            }
            if (wp == IDT_EDGE) {
                KillTimer(h, IDT_EDGE);
                capture_on_edge_timer();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case 1090: ui_show_main(&g_app); return 0;
                case 1091: DestroyWindow(h); return 0;
                case 1001: capture_force_toggle(); return 0;
                case 1002: DestroyWindow(h); return 0;
                case 1003: disp_switch_to_mac(); return 0;
                case 1004: disp_switch_to_win(); return 0;
                case 1005: disp_sleep_mac(); return 0;
                case 1006: xfer_cancel(); return 0;
                case 1007: ui_show_transfers(&g_app); return 0;
                case 1008: update_check_async(g_app.hwnd); return 0;
                case 1009: disp_wake_mac(); return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;

    set_dpi_aware();

    /* ---- 单实例 ----
     * 两个实例会同时 bind 24817/24818，而 Windows 的 SO_REUSEADDR 允许
     * 后来者绑上同一个端口并把连接抢走 —— 表现是"对端连上了，但键鼠时灵时不灵"，
     * 极难排查。用命名互斥量在最早的时刻拦掉第二个实例。
     *
     * 句柄故意不关：互斥量要活到进程结束，进程一退内核自动释放，
     * 所以崩溃也不会留下"需要手工清理"的僵尸锁。 */
    static HANDLE s_instance_mtx = NULL;
    s_instance_mtx = CreateMutexW(NULL, TRUE, L"Local\\HopDesk.SingleInstance");
    if (s_instance_mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        /* 托盘里已经有一个在跑，告诉用户去哪儿找，别让他以为程序没启动 */
        MessageBoxW(NULL,
                    L"HopDesk 已经在运行。\n\n"
                    L"请到任务栏通知区域（托盘）里操作它的图标。",
                    L"HopDesk", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        CloseHandle(s_instance_mtx);
        s_instance_mtx = NULL;
        return 0;
    }

    int startup_guard = hop_watchdog_begin("Windows startup", 30000);
    g_app.inst = inst;
    g_app.state = HOP_LINK_DOWN;

    hop_config_defaults(&g_app.cfg);
    snprintf(g_app.cfg.device_name, HOP_NAME_MAX, "%s", "Alex");
    g_app.cfg.screen_w = GetSystemMetrics(SM_CXSCREEN);
    g_app.cfg.screen_h = GetSystemMetrics(SM_CYSCREEN);

    wchar_t conf_path[MAX_PATH];
    beside(conf_path, MAX_PATH, L"hopdesk.conf");
    char conf_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, conf_path, -1, conf_utf8, sizeof(conf_utf8), NULL, NULL);
    snprintf(g_app.config_path, sizeof(g_app.config_path), "%s", conf_utf8);
    hop_config_load(&g_app.cfg, conf_utf8);

    wchar_t log_path[MAX_PATH];
    beside(log_path, MAX_PATH, L"hopdesk.log");
    char log_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, log_path, -1, log_utf8, sizeof(log_utf8), NULL, NULL);
    snprintf(g_app.log_path, sizeof(g_app.log_path), "%s", log_utf8);
    hop_log_open(log_utf8);

    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);

    /* 隐藏消息窗口 */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HopDeskMsg";
    RegisterClassW(&wc);
    g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"HopDesk",
                                 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);

    ui_init(&g_app);
    g_app.net = net_server_start(g_app.hwnd, &g_app.cfg);
    if (!g_app.net || !xfer_init(g_app.hwnd, &g_app.cfg)) {
        hop_log(HOP_LOG_ERROR, "network worker initialization failed");
        if (g_app.net) net_server_stop(g_app.net);
        ui_shutdown(&g_app);
        DestroyWindow(g_app.hwnd);
        return 1; /* supervisor may restart; do not call failed subsystem locks */
    }
    clip_init(g_app.hwnd);
    capture_init(&g_app);
    kbd_init(&g_app);
    hop_watchdog_end(startup_guard);
    s_ui_guard = hop_watchdog_begin("Windows message loop", 30000);
    SetTimer(g_app.hwnd, IDT_HOOK, 3000, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    hop_watchdog_end(s_ui_guard);
    KillTimer(g_app.hwnd, IDT_HOOK);
    kbd_shutdown();
    capture_shutdown();
    clip_shutdown(g_app.hwnd);
    xfer_shutdown();
    if (g_app.net) net_server_stop(g_app.net);
    ui_shutdown(&g_app);
    hop_log(HOP_LOG_INFO, "stopped");
    /* 正常退出时也释放互斥量，别指望"反正进程要退了" —— 万一有别的
     * 退出路径（比如宿主进程 fork 出来的子进程）还活着，锁就悬着了 */
    if (s_instance_mtx) CloseHandle(s_instance_mtx);
    return 0;
}
