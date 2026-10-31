/* =====================================================================
 *  HopDesk - Windows 托盘 UI
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "resource.h"

#include <stdio.h>
#include <commctrl.h>
#include <stdlib.h>

#define IDM_TOGGLE  1001
#define IDM_EXIT    1002
#define IDM_DISP_MAC   1003
#define IDM_DISP_WIN   1004
#define IDM_DISP_SLEEP 1005
#define IDM_XFER_CANCEL 1006
#define IDM_XFER_MANAGER 1007
#define ID_XFER_TIMER 20
#define IDC_XFER_TITLE 2101
#define IDC_XFER_DETAIL 2102
#define IDC_XFER_COUNTS 2103
#define IDC_XFER_PROGRESS 2104
#define IDC_XFER_CANCEL 2105

static HWND s_xfer_window, s_xfer_title, s_xfer_detail, s_xfer_counts, s_xfer_progress;

static void xfer_window_refresh(void) {
    if (!s_xfer_window) return;
    hop_xfer_status st; memset(&st, 0, sizeof(st)); xfer_get_status(&st);
    const wchar_t *phase = L"空闲";
    switch (st.phase) {
        case HOP_XFER_QUEUED: phase=L"排队中"; break;
        case HOP_XFER_SCANNING: phase=L"扫描中"; break;
        case HOP_XFER_SENDING: phase=L"发送中"; break;
        case HOP_XFER_RECEIVING: phase=L"接收中"; break;
        case HOP_XFER_VERIFYING: phase=L"校验中"; break;
        case HOP_XFER_COMPLETED: phase=L"已完成"; break;
        case HOP_XFER_PARTIAL: phase=L"部分失败"; break;
        case HOP_XFER_FAILED: phase=L"失败"; break;
        case HOP_XFER_CANCELLED: phase=L"已取消"; break;
    }
    wchar_t name[HOP_XFER_STATUS_NAME], detail[HOP_XFER_STATUS_DETAIL], counts[256];
    MultiByteToWideChar(CP_UTF8,0,st.name,-1,name,HOP_XFER_STATUS_NAME);
    MultiByteToWideChar(CP_UTF8,0,st.detail,-1,detail,HOP_XFER_STATUS_DETAIL);
    SetWindowTextW(s_xfer_title, name[0] ? name : L"暂无传输任务");
    const wchar_t *dir = st.direction==HOP_XFER_DIR_SEND?L"发送":st.direction==HOP_XFER_DIR_RECEIVE?L"接收":L"";
    swprintf(detail, HOP_XFER_STATUS_DETAIL, L"%ls · %ls · %hs", dir, phase, st.detail);
    SetWindowTextW(s_xfer_detail, detail);
    swprintf(counts, 256, L"文件 %u / %u    数据 %llu / %llu MB",
        st.files_done, st.files_total,
        (unsigned long long)(st.bytes_done/1048576ULL),
        (unsigned long long)(st.bytes_total/1048576ULL));
    SetWindowTextW(s_xfer_counts, counts);
    int pct=st.bytes_total?(int)((st.bytes_done*100)/st.bytes_total):(st.active?0:100);
    SendMessageW(s_xfer_progress, PBM_SETPOS, (WPARAM)(pct>100?100:pct), 0);
}

static LRESULT CALLBACK xfer_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch(msg) {
        case WM_CREATE:
            s_xfer_title=CreateWindowW(L"STATIC",L"暂无传输任务",WS_CHILD|WS_VISIBLE,20,18,520,26,h,(HMENU)IDC_XFER_TITLE,g_app.inst,NULL);
            s_xfer_detail=CreateWindowW(L"STATIC",L"等待文件传输",WS_CHILD|WS_VISIBLE,20,55,520,22,h,(HMENU)IDC_XFER_DETAIL,g_app.inst,NULL);
            s_xfer_counts=CreateWindowW(L"STATIC",L"文件 0 / 0    数据 0 / 0 MB",WS_CHILD|WS_VISIBLE,20,88,520,22,h,(HMENU)IDC_XFER_COUNTS,g_app.inst,NULL);
            s_xfer_progress=CreateWindowW(PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE,20,122,520,18,h,(HMENU)IDC_XFER_PROGRESS,g_app.inst,NULL);
            CreateWindowW(L"BUTTON",L"取消传输",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,430,158,110,30,h,(HMENU)IDC_XFER_CANCEL,g_app.inst,NULL);
            SetTimer(h,ID_XFER_TIMER,400,NULL); xfer_window_refresh(); return 0;
        case WM_TIMER: xfer_window_refresh(); return 0;
        case WM_COMMAND: if(LOWORD(wp)==IDC_XFER_CANCEL){xfer_cancel();return 0;} break;
        case WM_CLOSE: KillTimer(h,ID_XFER_TIMER); DestroyWindow(h); return 0;
        case WM_DESTROY: s_xfer_window=NULL; return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

void ui_show_transfers(win_app *app) {
    if (!s_xfer_window) {
        WNDCLASSW wc; memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=xfer_wnd_proc;
        wc.hInstance=app->inst; wc.lpszClassName=L"HopDeskTransferManager";
        wc.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        RegisterClassW(&wc);
        INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic);
        s_xfer_window=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"HopDesk · 传输管理器",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,580,250,NULL,NULL,app->inst,NULL);
    }
    if(s_xfer_window){ShowWindow(s_xfer_window,SW_SHOWNORMAL);SetForegroundWindow(s_xfer_window);xfer_window_refresh();}
}

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
    AppendMenuW(m, MF_STRING, IDM_XFER_MANAGER, L"传输管理器…");
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
