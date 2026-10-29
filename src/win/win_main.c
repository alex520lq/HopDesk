/* =====================================================================
 *  HopDesk - Windows 入口
 *  单实例、隐藏消息窗口、托盘、网络、鼠标钩子。
 * ===================================================================== */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

win_app g_app;

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
        if (g_app.state == HOP_LINK_DOWN) g_app.state = HOP_LINK_LOCAL;
    } else {
        if (g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
        g_app.state = HOP_LINK_DOWN;
    }
    ui_refresh(&g_app);
}

static void on_recv(hop_recv *r) {
    const uint8_t *p = r->data;
    uint32_t len = r->len;
    switch (r->type) {
        case HOP_MSG_HELLO: {
            /* {tlen, token, nlen, name, w(4), h(4)} */
            uint32_t off = 0;
            if (len < 2) break;
            off = 1 + p[0];                 /* 跳过 token */
            if (off + 1 > len) break;
            off += 1 + p[off];              /* 跳过 name  */
            if (off + 8 <= len) {
                g_app.peer_w = (int)hop_get_be32(p + off);
                g_app.peer_h = (int)hop_get_be32(p + off + 4);
                hop_log(HOP_LOG_INFO, "peer screen %dx%d", g_app.peer_w, g_app.peer_h);
            } else {
                hop_log(HOP_LOG_WARN, "HELLO too short (len=%u off=%u)", len, off);
            }
            break;
        }
        case HOP_MSG_EDGE_LEAVE:
            if (g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
            break;
        default:
            break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_LINK:
            on_link((int)wp);
            return 0;
        case WM_APP_RECV: {
            hop_recv *r = (hop_recv *)lp;
            on_recv(r);
            free(r);
            return 0;
        }
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP)
                ui_show_menu(&g_app);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case 1001: capture_force_toggle(); return 0;
                case 1002: DestroyWindow(h); return 0;
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

    /* 单实例 */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\HopDesk");
    if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

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
    hop_config_load(&g_app.cfg, conf_utf8);

    wchar_t log_path[MAX_PATH];
    beside(log_path, MAX_PATH, L"hopdesk.log");
    char log_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, log_path, -1, log_utf8, sizeof(log_utf8), NULL, NULL);
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
    capture_init(&g_app);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    capture_shutdown();
    if (g_app.net) net_server_stop(g_app.net);
    ui_shutdown(&g_app);
    hop_log(HOP_LOG_INFO, "stopped");
    if (mtx) CloseHandle(mtx);
    return 0;
}
