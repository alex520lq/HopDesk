/* =====================================================================
 *  HopDesk - Windows 托盘 UI
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "resource.h"

#include <stdio.h>
#include <stdlib.h>

#define IDM_TOGGLE  1001
#define IDM_EXIT    1002
#define IDM_DISP_MAC   1003
#define IDM_DISP_WIN   1004
#define IDM_DISP_SLEEP 1005
#define IDM_XFER_CANCEL 1006

static const wchar_t *state_text_w(win_app *app) {
    switch (app->state) {
        case HOP_LINK_SHARING: return L"HopDesk · 键鼠在 Mac";
        case HOP_LINK_LOCAL:   return L"HopDesk · 已连接（Windows）";
        default:               return L"HopDesk · 等待连接";
    }
}

void ui_refresh(win_app *app) {
    if (!app->tray_added) return;
    int idx = app->state == HOP_LINK_SHARING ? 2
            : app->state == HOP_LINK_LOCAL   ? 1 : 0;
    app->nid.hIcon = app->icons[idx];
    wcscpy(app->nid.szTip, state_text_w(app));
    Shell_NotifyIconW(NIM_MODIFY, &app->nid);
}

static void show_menu(win_app *app) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();
    EnableMenuItem(m, 0, MF_GRAYED | MF_BYPOSITION);
    AppendMenuW(m, MF_GRAYED | MF_STRING, 0, state_text_w(app));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    const wchar_t *toggle = app->state == HOP_LINK_SHARING
                                ? L"收回键鼠（到 Windows）"
                                : L"切换键鼠到 Mac";
    int can = net_server_has_client(app->net);
    AppendMenuW(m, (can ? 0 : MF_GRAYED) | MF_STRING, IDM_TOGGLE, toggle);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /* 阶段 4：显示切换（替代原来的 display-switch.ps1 + SSH） */
    AppendMenuW(m, (can ? 0 : MF_GRAYED) | MF_STRING, IDM_DISP_MAC,
                L"显示器 → Mac（唤醒 + 切源）");
    AppendMenuW(m, (can ? 0 : MF_GRAYED) | MF_STRING, IDM_DISP_WIN,
                L"显示器 → Windows");
    AppendMenuW(m, (can ? 0 : MF_GRAYED) | MF_STRING, IDM_DISP_SLEEP,
                L"Mac 显示睡眠");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /* 传了一半改主意：键鼠已经切走了、或者发现选错了文件 */
    AppendMenuW(m, (xfer_busy() ? 0 : MF_GRAYED) | MF_STRING,
                IDM_XFER_CANCEL, L"取消当前传输");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(app->hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(m);
}
void ui_show_menu(win_app *app) { show_menu(app); }

void ui_init(win_app *app) {
    /* All tray states use the exact same brand icon; state is shown in tooltip/menu. */
    for (int i = 0; i < 3; ++i) {
        app->icons[i] = (HICON)LoadImageW(GetModuleHandleW(NULL),
            MAKEINTRESOURCEW(IDI_HOPDESK), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    }

    memset(&app->nid, 0, sizeof(app->nid));
    app->nid.cbSize = sizeof(app->nid);
    app->nid.hWnd = app->hwnd;
    app->nid.uID = 1;
    app->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app->nid.uCallbackMessage = WM_APP_TRAY;
    app->nid.hIcon = app->icons[0];
    wcscpy(app->nid.szTip, L"HopDesk");

    app->tray_added = Shell_NotifyIconW(NIM_ADD, &app->nid);
}

void ui_shutdown(win_app *app) {
    if (app->tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &app->nid);
        app->tray_added = 0;
    }
    for (int i = 0; i < 3; ++i)
        if (app->icons[i]) DestroyIcon(app->icons[i]);
}
