/* =====================================================================
 *  HopDesk - Windows 应用全局状态
 * ===================================================================== */
#ifndef WIN_APP_H
#define WIN_APP_H

#include <windows.h>
#include "hop.h"
#include "net_server.h"

#define WM_APP_TRAY    (WM_APP + 3)
#define WM_APP_STATE   (WM_APP + 4)   /* 状态变化，刷新 UI */

typedef struct {
    hop_config     cfg;
    net_server    *net;
    HINSTANCE      inst;
    HWND           hwnd;          /* 隐藏消息窗口 */
    NOTIFYICONDATAW nid;
    int            tray_added;
    hop_link_state state;
    HICON          icons[3];      /* down / local / sharing */
} win_app;

extern win_app g_app;

/* mouse_capture.c */
void capture_init(win_app *app);
void capture_shutdown(void);
void capture_set_sharing(int sharing);
void capture_force_toggle(void);   /* 手动切换 */

/* ui.c */
void ui_init(win_app *app);
void ui_shutdown(win_app *app);
void ui_refresh(win_app *app);
void ui_show_menu(win_app *app);

#endif
