/* =====================================================================
 *  HopDesk - Windows 应用全局状态
 * ===================================================================== */
#ifndef WIN_APP_H
#define WIN_APP_H

#include <windows.h>
#include "hop.h"
#include "net_server.h"
#include "../core/display_catalog.h"

#define WM_APP_TRAY    (WM_APP + 3)
#define WM_APP_STATE   (WM_APP + 4)   /* 状态变化，刷新 UI */
#define WM_APP_CLIP (WM_APP + 5) /* format + owned HGLOBAL; 0 = retry */
#define WM_APP_CLIP_PASTE (WM_APP + 6) /* wParam: paste request ID; lParam: owned CF_HDROP */
#define WM_APP_SPLIT_REQUEST (WM_APP + 7) /* agent-owned request, no message pointers */
#define WM_APP_DISPLAY_DONE (WM_APP + 8)
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
    HICON          icon_base;     /* immutable app icon resource */
    HICON          tray_icon;     /* current dynamically rendered status icon */
    int            peer_w, peer_h; /* 对端屏幕尺寸，握手时取得 */
    char           peer_name[HOP_NAME_MAX]; /* 对端设备名称 */
    unsigned       peer_input_mode; /* 对端当前提供的键鼠能力 */
    unsigned       peer_audio_mode; /* 对端当前配置的声音能力 */
    int            peer_capabilities_known;
    char           config_path[1024]; /* UTF-8 canonical config path */
    char           log_path[1024];    /* UTF-8 active log path */
} win_app;

extern win_app g_app;
void local_addresses(char *out,size_t cap);
void win_send_capabilities(unsigned takeover_mask);
void win_send_capabilities_ex(unsigned takeover_mask, unsigned flags);
void win_apply_input_mode(unsigned mode, unsigned takeover_mask, int persist);
void win_apply_audio_mode(unsigned mode, int sync_peer, int persist);
int  win_apply_hot_config(const hop_config *next);

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
void kbd_forget_physical_state(void); /* 钩子丢事件后丢弃不可信的物理按键快照 */
void kbd_retry_pending(void);       /* 重试未成功投递到本机的合成 KeyUp / KeyDown */
void kbd_begin_sharing(void);       /* 移交时把已按住的键从本机转交给对端 */
void kbd_end_sharing(void);         /* 收回时恢复仍物理按住的本机按键 */

void win_inject_message(uint8_t type,const uint8_t *p,uint32_t len);
void win_inject_reset(void);
void win_inject_retry_pending(void); /* 重试未成功投递的远端 KeyUp / ButtonUp */

/* clipboard.c */
void clip_init(HWND hwnd);
int clip_shutdown(HWND hwnd);
void clip_on_worker_message(UINT format, HGLOBAL h);
void clip_on_update(void);                              /* WM_CLIPBOARDUPDATE */
void clip_retry_pending(void);                          /* WM_TIMER(IDT_CLIP_RETRY) */
void clip_on_link(int connected);
void clip_on_remote_file_offer(const uint8_t *p, uint32_t len);
void clip_on_remote_file_offer_clear(const uint8_t *p, uint32_t len);
void clip_on_paste_request(const uint8_t *p, uint32_t len);
void clip_on_paste_failed(const uint8_t *p, uint32_t len);
int  clip_request_remote_paste(void);
void clip_on_paste_message(HGLOBAL h, uint32_t request_id);
void clip_complete_remote_paste(uint32_t batch, const wchar_t *const *paths, int n, int success);
void clip_apply_remote(const uint8_t *p, uint32_t len); /* 收到 CLIP_TEXT */
/* 数据通道收完一批后回调：把落盘的顶层条目挂到剪贴板 */
void clip_apply_remote_paths(const wchar_t *const *paths, int n);
void clip_apply_remote_image(const uint8_t *bgra, int w, int h, int stride);

/* display.c（阶段 4） */
int disp_set_input(uint16_t vcp);      /* DDC/CI 0x60 写输入源 */
int disp_get_input(uint16_t *out);     /* 读当前输入源，用于日志/排错 */

int disp_input_read(const wchar_t *device, uint16_t *out);
int disp_input_verified(const wchar_t *device, uint16_t vcp,
                        int retries, int timeout_ms, int poll_ms);
int disp_input_busy(void);
int disp_input_matches(const wchar_t *device, uint16_t vcp);
int shared_display_start(int target); /* main-thread admission; immutable config snapshot */
void shared_display_message(const uint8_t *p, uint32_t len);
void shared_display_shutdown(void); /* join before stopping network */
int shared_display_busy(void);
void shared_display_status(char *out, size_t cap);
void shared_display_devices(char *out, size_t cap);
void shared_display_get_inventory(hop_display_inventory *out);
int  shared_display_probe_async(void);
int  shared_display_identify_async(void);
void capture_topology_changed(void);

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
int  win_ui_display_action(int action);
void ui_notify_paste_ready(void);

#endif
