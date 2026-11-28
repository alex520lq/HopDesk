/* =====================================================================
 * HopDesk - Windows tray shell. The full application lives in one main UI.
 * Tray actions open the main UI or exit; persistent audio settings live in Settings.
 * ===================================================================== */
#include "app.h"
#include "resource.h"
#include "xfer.h"
#include "split_ui.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IDM_DISPLAY_TOGGLE 1010
#define IDM_OPEN_MAIN 1090
#define IDM_EXIT_APP  1091

static int s_notified_direction;
static uint32_t s_notified_batch;
static int s_notified_phase;

static int s_last_tray_state = -1;
static int s_last_tray_progress = -999;
static int s_last_tray_direction = -1;
static int s_last_tray_phase = -1;
static wchar_t s_last_tray_tip[128];

static void tray_draw_dot(DWORD *pixels, int cx, int cy, int radius, DWORD argb) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            int dx = x - cx, dy = y - cy;
            if (x >= 0 && x < 32 && y >= 0 && y < 32 && dx*dx + dy*dy <= radius*radius)
                pixels[y * 32 + x] = argb;
        }
    }
}

static HICON tray_make_status_icon(HICON base, int state, int progress) {
    if (!base) return NULL;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD *pixels = NULL;
    HBITMAP color = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!color || !pixels) {
        if (color) DeleteObject(color);
        return CopyIcon(base);
    }
    memset(pixels, 0, 32u * 32u * sizeof(*pixels));
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        DeleteObject(color);
        return CopyIcon(base);
    }
    HGDIOBJ previous = SelectObject(dc, color);
    DrawIconEx(dc, 0, 0, base, 32, 32, 0, NULL, DI_NORMAL);
    SelectObject(dc, previous);
    DeleteDC(dc);

    if (state == 0) {
        for (int i = 0; i < 32 * 32; ++i) {
            BYTE a = (BYTE)(pixels[i] >> 24);
            if (!a) continue;
            BYTE r = (BYTE)(pixels[i] >> 16);
            BYTE g = (BYTE)(pixels[i] >> 8);
            BYTE b = (BYTE)pixels[i];
            BYTE gray = (BYTE)((30u * r + 59u * g + 11u * b) / 100u);
            pixels[i] = ((DWORD)a << 24) | ((DWORD)gray << 16) |
                        ((DWORD)gray << 8) | (DWORD)gray;
        }
    }

    if (progress >= 0 || progress == -2) {
        static const POINT ring[12] = {
            {25,18},{28,19},{30,22},{31,25},{30,28},{28,30},
            {25,31},{22,30},{20,28},{19,25},{20,22},{22,19}
        };
        for (int i = 0; i < 12; ++i)
            tray_draw_dot(pixels, ring[i].x, ring[i].y, 1, 0xB0506070u);
        int segments = progress == -2 ? 3 : (progress * 12 + 99) / 100;
        if (segments > 12) segments = 12;
        for (int i = 0; i < segments; ++i)
            tray_draw_dot(pixels, ring[i].x, ring[i].y, 1, 0xFF18D8F2u);
    } else {
        DWORD badge = state == 2 ? 0xFF25B86Au :
                      state == 1 ? 0xFF2785F5u : 0xFF9299A3u;
        tray_draw_dot(pixels, 26, 26, 3, 0xFF20242Au);
        tray_draw_dot(pixels, 26, 26, 2, badge);
    }

    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    if (!mask) {
        DeleteObject(color);
        return CopyIcon(base);
    }
    HDC mask_dc = CreateCompatibleDC(NULL);
    if (!mask_dc) {
        DeleteObject(mask);
        DeleteObject(color);
        return CopyIcon(base);
    }
    HGDIOBJ mask_previous = SelectObject(mask_dc, mask);
    PatBlt(mask_dc, 0, 0, 32, 32, BLACKNESS);
    SelectObject(mask_dc, mask_previous);
    DeleteDC(mask_dc);
    ICONINFO info;
    memset(&info, 0, sizeof(info));
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return result ? result : CopyIcon(base);
}

static void tray_refresh(win_app *app) {
    if (!app || !app->tray_added) return;

    hop_xfer_status_pair pair;
    memset(&pair, 0, sizeof(pair));
    xfer_get_status_pair(&pair);
    const hop_xfer_status *st = pair.primary_direction == HOP_XFER_DIR_SEND
        ? &pair.send : &pair.receive;
    int busy = st->active || st->queued_count > 0;
    int progress = -1;
    if (busy) {
        progress = st->bytes_total
            ? (int)((st->bytes_done >= st->bytes_total) ? 100 :
                (100.0 * (double)st->bytes_done) / (double)st->bytes_total)
            : -2;
    }
    int state = app->state == HOP_LINK_SHARING ? 2 :
                app->state == HOP_LINK_LOCAL ? 1 : 0;
    int direction = busy ? st->direction : 0;
    int phase = busy ? st->phase : 0;

    wchar_t tip[128];
    if (busy && progress >= 0) {
        const wchar_t *verb = direction == HOP_XFER_DIR_SEND ? L"发送" : L"接收";
        swprintf_s(tip, ARRAYSIZE(tip), L"HopDesk · 正在%ls · %d%%", verb, progress);
    } else if (busy) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 正在准备传输");
    } else if (!app->net) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 网络服务未启动");
    } else if (net_server_is_disconnected(app->net)) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 已手动断开");
    } else if (app->cfg.role == HOP_ROLE_SERVER &&
               (!net_server_listening(app->net) || !xfer_listening())) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 端口不可用，正在重试");
    } else if (state == 2) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 键鼠已切换到对端");
    } else if (state == 1) {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 已连接");
    } else {
        wcscpy_s(tip, ARRAYSIZE(tip), L"HopDesk · 等待对端连接");
    }

    int changed = state != s_last_tray_state ||
        progress != s_last_tray_progress || direction != s_last_tray_direction ||
        phase != s_last_tray_phase || wcscmp(tip, s_last_tray_tip) != 0;
    if (!changed) return;

    HICON next = tray_make_status_icon(app->icon_base, state, progress);
    if (next) {
        HICON previous = app->tray_icon;
        app->tray_icon = next;
        app->nid.hIcon = next;
        wcscpy_s(app->nid.szTip, ARRAYSIZE(app->nid.szTip), tip);
        Shell_NotifyIconW(NIM_MODIFY, &app->nid);
        if (previous) DestroyIcon(previous);
    } else {
        wcscpy_s(app->nid.szTip, ARRAYSIZE(app->nid.szTip), tip);
        Shell_NotifyIconW(NIM_MODIFY, &app->nid);
    }

    s_last_tray_state = state;
    s_last_tray_progress = progress;
    s_last_tray_direction = direction;
    s_last_tray_phase = phase;
    wcscpy_s(s_last_tray_tip, ARRAYSIZE(s_last_tray_tip), tip);
}

void ui_init(win_app *app) {
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    app->icon_base=(HICON)LoadImageW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDI_HOPDESK),
                                    IMAGE_ICON,32,32,LR_DEFAULTCOLOR);
    app->tray_icon=app->icon_base?CopyIcon(app->icon_base):NULL;
    memset(&app->nid,0,sizeof(app->nid));
    app->nid.cbSize=sizeof(app->nid);
    app->nid.hWnd=app->hwnd;
    app->nid.uID=1;
    app->nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    app->nid.uCallbackMessage=WM_APP_TRAY;
    app->nid.hIcon=app->tray_icon;
    wcscpy_s(app->nid.szTip,ARRAYSIZE(app->nid.szTip),L"HopDesk");
    app->tray_added=Shell_NotifyIconW(NIM_ADD,&app->nid);
    SetTimer(app->hwnd,IDT_XFER_NOTIFY,500,NULL);
    if (app->tray_added) {
        s_last_tray_state=-1; s_last_tray_progress=-999;
        s_last_tray_direction=-1; s_last_tray_phase=-1; s_last_tray_tip[0]=L'\0';
        tray_refresh(app);
    }
}

void ui_show_menu(win_app *app) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,IDM_OPEN_MAIN,L"打开");
    if (app->cfg.disp_shared)
        AppendMenuW(menu,MF_STRING,IDM_DISPLAY_TOGGLE,L"一键切换显示器");
    AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,IDM_EXIT_APP,L"退出");
    SetForegroundWindow(app->hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, app->hwnd, NULL);
    PostMessageW(app->hwnd,WM_NULL,0,0);
    DestroyMenu(menu);
    if (cmd) PostMessageW(app->hwnd, WM_COMMAND, cmd, 0);
}

void ui_show_main(win_app *app) {
    (void)app;
    win_split_agent_open(HOPDESK_UI_OVERVIEW);
}
void ui_show_transfers(win_app *app) {
    (void)app;
    win_split_agent_open(HOPDESK_UI_TRANSFER);
}
void ui_refresh(win_app *app) {
    tray_refresh(app);
}
void ui_notify_paste_ready(void) {
    if (!g_app.tray_added) return;
    NOTIFYICONDATAW nid=g_app.nid;
    nid.uFlags=NIF_INFO;
    wcscpy_s(nid.szInfoTitle,ARRAYSIZE(nid.szInfoTitle),L"文件已准备好");
    wcscpy_s(nid.szInfo,ARRAYSIZE(nid.szInfo),L"请选择目标文件夹，然后再次按 Ctrl+V 粘贴。");
    nid.dwInfoFlags=NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY,&nid);
}
void ui_transfer_poll(void) {
    tray_refresh(&g_app);
    hop_xfer_status st; memset(&st,0,sizeof(st)); xfer_get_status(&st);
    if (st.active || !st.batch) return;
    if (st.phase!=HOP_XFER_COMPLETED && st.phase!=HOP_XFER_PARTIAL &&
        st.phase!=HOP_XFER_FAILED && st.phase!=HOP_XFER_CANCELLED) return;
    if (s_notified_batch==st.batch && s_notified_phase==st.phase &&
        s_notified_direction==st.direction) return;
    s_notified_batch=st.batch; s_notified_phase=st.phase; s_notified_direction=st.direction;
    NOTIFYICONDATAW nid=g_app.nid;
    nid.uFlags=NIF_INFO;
    wcscpy_s(nid.szInfoTitle,ARRAYSIZE(nid.szInfoTitle),
        st.phase==HOP_XFER_COMPLETED?L"传输完成":
        st.phase==HOP_XFER_PARTIAL?L"传输部分失败":
        st.phase==HOP_XFER_CANCELLED?L"传输已取消":L"传输失败");
    MultiByteToWideChar(CP_UTF8,0,st.detail,-1,nid.szInfo,ARRAYSIZE(nid.szInfo));
    nid.dwInfoFlags=st.phase==HOP_XFER_COMPLETED?NIIF_INFO:NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY,&nid);
}
void ui_shutdown(win_app *app) {
    KillTimer(app->hwnd,IDT_XFER_NOTIFY);
    if(app->tray_added){Shell_NotifyIconW(NIM_DELETE,&app->nid);app->tray_added=0;}
    if (app->tray_icon) { DestroyIcon(app->tray_icon); app->tray_icon=NULL; }
    if (app->icon_base) { DestroyIcon(app->icon_base); app->icon_base=NULL; }
}
