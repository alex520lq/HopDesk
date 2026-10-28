/* =====================================================================
 *  HopDesk - Windows 托盘 UI
 * ===================================================================== */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>

#define IDM_TOGGLE  1001
#define IDM_EXIT    1002

/* 用纯色圆生成一个 16x16 图标 */
static HICON make_icon(COLORREF c) {
    const int S = 16;
    HDC scr = GetDC(NULL);
    HDC dc = CreateCompatibleDC(scr);
    HBITMAP color = CreateCompatibleBitmap(scr, S, S);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, NULL);
    HBITMAP old = SelectObject(dc, color);

    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    RECT all = { 0, 0, S, S };
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    HBRUSH b = CreateSolidBrush(c);
    HBRUSH ob = SelectObject(dc, b);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN open = SelectObject(dc, pen);
    Ellipse(dc, 2, 2, S - 2, S - 2);
    SelectObject(dc, ob);
    SelectObject(dc, open);
    DeleteObject(b);
    DeleteObject(pen);

    SelectObject(dc, old);
    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(NULL, scr);
    return ic;
}

static const char *state_text(win_app *app) {
    switch (app->state) {
        case HOP_LINK_SHARING: return "HopDesk · 键鼠在 Mac";
        case HOP_LINK_LOCAL:   return "HopDesk · 已连接（Windows）";
        default:               return "HopDesk · 等待连接";
    }
}

void ui_refresh(win_app *app) {
    if (!app->tray_added) return;
    int idx = app->state == HOP_LINK_SHARING ? 2
            : app->state == HOP_LINK_LOCAL   ? 1 : 0;
    app->nid.hIcon = app->icons[idx];
    MultiByteToWideChar(CP_UTF8, 0, state_text(app), -1,
                        app->nid.szTip, (int)(sizeof(app->nid.szTip) / 2));
    Shell_NotifyIconW(NIM_MODIFY, &app->nid);
}

static void show_menu(win_app *app) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();
    char buf[96];
    EnableMenuItem(m, 0, MF_GRAYED | MF_BYPOSITION);
    AppendMenuA(m, MF_GRAYED | MF_STRING, 0, state_text(app));
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);

    const char *toggle = app->state == HOP_LINK_SHARING ? "收回键鼠（到 Windows）"
                                                       : "切换键鼠到 Mac";
    snprintf(buf, sizeof(buf), "%s", toggle);
    int can = net_server_has_client(app->net);
    AppendMenuA(m, (can ? 0 : MF_GRAYED) | MF_STRING, IDM_TOGGLE, buf);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_EXIT, "退出");

    SetForegroundWindow(app->hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(m);
}
void ui_show_menu(win_app *app) { show_menu(app); }

void ui_init(win_app *app) {
    app->icons[0] = make_icon(RGB(150, 150, 150));
    app->icons[1] = make_icon(RGB(70, 130, 220));
    app->icons[2] = make_icon(RGB(60, 180, 110));

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
