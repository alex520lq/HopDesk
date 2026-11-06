/* =====================================================================
 * HopDesk - Windows tray shell. The full application lives in one main UI.
 * Tray actions are intentionally limited to Open and Exit.
 * ===================================================================== */
#include "app.h"
#include "resource.h"
#include "xfer.h"
#include "main_ui.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#define IDM_OPEN_MAIN 1090
#define IDM_EXIT_APP  1091

static int s_notified_direction;
static uint32_t s_notified_batch;
static int s_notified_phase;

static const wchar_t *tray_tip(win_app *app) {
    if (!app->net) return L"HopDesk · 网络服务未启动";
    if (!net_server_listening(app->net) || !xfer_listening()) return L"HopDesk · 端口不可用，正在重试";
    if (app->state == HOP_LINK_SHARING) return L"HopDesk · 键鼠正在 Mac";
    if (app->state == HOP_LINK_LOCAL) return L"HopDesk · 已连接";
    return L"HopDesk · 等待 Mac 连接";
}

void ui_init(win_app *app) {
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    for (int i=0;i<3;++i) {
        app->icons[i]=(HICON)LoadImageW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDI_HOPDESK),
                                        IMAGE_ICON,32,32,LR_DEFAULTCOLOR);
    }
    memset(&app->nid,0,sizeof(app->nid));
    app->nid.cbSize=sizeof(app->nid);
    app->nid.hWnd=app->hwnd;
    app->nid.uID=1;
    app->nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    app->nid.uCallbackMessage=WM_APP_TRAY;
    app->nid.hIcon=app->icons[0];
    wcscpy_s(app->nid.szTip,ARRAYSIZE(app->nid.szTip),L"HopDesk");
    app->tray_added=Shell_NotifyIconW(NIM_ADD,&app->nid);
    SetTimer(app->hwnd,IDT_XFER_NOTIFY,500,NULL);
}

void ui_show_menu(win_app *app) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,IDM_OPEN_MAIN,L"打开");
    AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,IDM_EXIT_APP,L"退出");
    SetForegroundWindow(app->hwnd);
    TrackPopupMenu(menu,TPM_RIGHTBUTTON|TPM_BOTTOMALIGN,pt.x,pt.y,0,app->hwnd,NULL);
    PostMessageW(app->hwnd,WM_NULL,0,0);
    DestroyMenu(menu);
}

void ui_show_main(win_app *app) {
    (void)app;
    main_ui_show_page(0);
}
void ui_show_transfers(win_app *app) {
    (void)app;
    main_ui_show_page(3);
}
void ui_refresh(win_app *app) {
    if (app->tray_added) {
        int idx=app->state==HOP_LINK_SHARING?2:app->state==HOP_LINK_LOCAL?1:0;
        app->nid.hIcon=app->icons[idx];
        wcscpy_s(app->nid.szTip,ARRAYSIZE(app->nid.szTip),tray_tip(app));
        Shell_NotifyIconW(NIM_MODIFY,&app->nid);
    }
    main_ui_refresh();
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
    main_ui_shutdown();
    if(app->tray_added){Shell_NotifyIconW(NIM_DELETE,&app->nid);app->tray_added=0;}
    for(int i=0;i<3;++i)if(app->icons[i]){DestroyIcon(app->icons[i]);app->icons[i]=NULL;}
}
