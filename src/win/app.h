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
#define WM_APP_CLIP (WM_APP + 5) /* format + owned HGLOBAL; 0 = retry */
#define WM_APP_CLIP_PASTE (WM_APP + 6) /* wParam: expected clipboard sequence; lParam: owned CF_HDROP */
#define IDT_HOOK 5
#define IDT_CLIP_RETRY 3              /* 剪贴板读写/发送延迟重试 */

/* 切到 Mac 显示时，先唤醒再切源；这两步之间要等一会儿 */
#define IDT_DISP       1
#define DISP_WAKE_DELAY_MS 1200

/* 边缘移交的停留计时（对齐 Deskflow 的 switchDelay） */
#define IDT_EDGE       2
#define IDT_XFER_NOTIFY 4

typedef struct {
    hop_config     cfg;
    net_server    *net;
    HINSTANCE      inst;
    HWND           hwnd;          /* 隐藏消息窗口 */
    NOTIFYICONDATAW nid;
    int            tray_added;
    hop_link_state state;
    HICON          icons[3];      /* down / local / sharing */
    int            peer_w, peer_h; /* 客户端（Mac）屏幕尺寸，握手时取得 */
    char           config_path[1024]; /* UTF-8 canonical config path */
    char           log_path[1024];    /* UTF-8 active log path */
} win_app;

extern win_app g_app;
void local_addresses(char *out,size_t cap);

/* mouse_capture.c */
void capture_init(win_app *app);
void capture_shutdown(void);
void capture_check_hooks(void);
void capture_set_sharing(int sharing);
void capture_force_toggle(void);   /* 手动切换 */
void capture_on_edge_timer(void);  /* WM_TIMER(IDT_EDGE)：停留够时长才移交 */

/* kbd_capture.c（阶段 2） */
void kbd_init(win_app *app);
void kbd_shutdown(void);
DWORD kbd_hook_tick(void);
void kbd_reinstall(void);
void kbd_release_all(void);        /* 交还键鼠时松开对端仍按着的键 */
void kbd_begin_sharing(void);       /* 移交时把已按住的键从本机转交给对端 */
void kbd_end_sharing(void);         /* 收回时恢复仍物理按住的本机按键 */

void win_inject_message(uint8_t type,const uint8_t *p,uint32_t len);
void win_inject_reset(void);

/* clipboard.c */
void clip_init(HWND hwnd);
void clip_shutdown(HWND hwnd);
void clip_on_worker_message(UINT format, HGLOBAL h);
void clip_on_update(void);                              /* WM_CLIPBOARDUPDATE */
void clip_retry_pending(void);                          /* WM_TIMER(IDT_CLIP_RETRY) */
void clip_on_link(int connected);
void clip_on_remote_file_offer(const uint8_t *p, uint32_t len);
void clip_on_remote_file_offer_clear(const uint8_t *p, uint32_t len);
void clip_on_paste_request(const uint8_t *p, uint32_t len);
void clip_on_paste_failed(const uint8_t *p, uint32_t len);
int  clip_request_remote_paste(void);
void clip_on_paste_message(HGLOBAL h, DWORD expected_sequence);
void clip_complete_remote_paste(uint32_t batch, const wchar_t *const *paths, int n, int success);
void clip_apply_remote(const uint8_t *p, uint32_t len); /* 收到 CLIP_TEXT */
/* 数据通道收完一批后回调：把落盘的顶层条目挂到剪贴板 */
void clip_apply_remote_paths(const wchar_t *const *paths, int n);
void clip_apply_remote_image(const uint8_t *bgra, int w, int h, int stride);

/* display.c（阶段 4） */
int disp_set_input(uint16_t vcp);      /* DDC/CI 0x60 写输入源 */
int disp_get_input(uint16_t *out);     /* 读当前输入源，用于日志/排错 */

/* ui.c */
void ui_init(win_app *app);
void ui_shutdown(win_app *app);
void ui_refresh(win_app *app);
void ui_show_menu(win_app *app);
void ui_show_transfers(win_app *app);
void ui_show_main(win_app *app);
void main_ui_show_page(int page);
void main_ui_refresh(void);
void main_ui_shutdown(void);
void ui_transfer_poll(void);

#endif
