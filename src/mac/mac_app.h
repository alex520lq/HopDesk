/* =====================================================================
 *  HopDesk - macOS 应用全局状态（C 核心 + 少量 ObjC）
 * ===================================================================== */
#ifndef MAC_APP_H
#define MAC_APP_H

#include "hop.h"
#include "../core/display_catalog.h"
#include "../core/update_controller.h"

typedef struct {
    hop_config cfg; /* immutable after main startup; all threads read-only */
    /* Geometry initialized before threads; later refresh/read only in input/control dispatch. */
    double     screen_w, screen_h;   /* 本端 CoreGraphics 坐标空间（逻辑点） */
    char       config_path[1024];    /* canonical Application Support config */
    char       log_path[1024];       /* active log path */
} mac_app;

extern mac_app g_mac;
void local_addresses(char *out,size_t cap);
int  mac_apply_hot_config(const hop_config *next);

/* net_client.c */
void net_loop_run(void);                 /* 连接 + 重连 + 接收（后台线程） */
void net_loop_shutdown(void);             /* stop and join control workers */
int  net_send(uint8_t type, const void *p, uint32_t len);
int  net_send_session(const hop_session_id *expected, uint8_t type, const void *p, uint32_t len);
void net_request_stop(void);
void net_request_reconnect(void); /* close current socket; background loop reconnects */
void net_set_disconnected(int disconnected); /* pause/resume control & data link */
int  net_is_disconnected(void);
int  net_protocol_mismatch(void);
void net_send_capabilities(unsigned takeover_mask);
void net_send_capabilities_ex(unsigned takeover_mask, unsigned flags);
void net_apply_input_mode(unsigned mode, unsigned takeover_mask, int persist);
void net_apply_audio_mode(unsigned mode, int sync_peer, int persist);
unsigned net_get_input_mode(void);
void net_get_peer_capabilities(unsigned *peer_input, unsigned *peer_audio, int *known,
                               char *peer_name, size_t peer_name_cap);
/* 打开 TCP keepalive：空闲连接变黑洞时让内核在一分多钟内判定死亡，
 * 否则阻塞的 read 永远不返回，通道就此僵死。控制/数据通道都要调。 */
void net_set_keepalive(int s);
int net_connect_timeout(const char *host, uint16_t port, unsigned timeout_ms);

/* inject.c */
void mac_capture_init(void);
void mac_capture_set_input_mode(unsigned mode);
void mac_capture_connected(int connected); /* main queue */
void mac_capture_return(void); /* main queue */
void mac_capture_peer_geometry(uint32_t w,uint32_t h);
void inject_init(void);
void inject_set_peer_geometry(uint32_t w, uint32_t h);
void inject_enter(int y);          /* 服务端移交：在边缘对应 y 进入 */
void inject_move(int dx, int dy);
void inject_button(int button, int down, int click_count);
void inject_wheel(int dx, int dy);
void inject_reset(void);
void inject_key(uint16_t kc, int down);   /* 阶段 2：映射后的 Mac 键码 */
void inject_paste_shortcut(void); /* replay validated intercepted cross-device paste */
void inject_release_all(void);            /* 断线时松开所有修饰键 */

/* clipboard.m */
void clipboard_init(void);
int  clipboard_shutdown(void);
void clipboard_on_link(int connected);
void clipboard_on_remote_file_offer(const uint8_t *p, uint32_t len);
void clipboard_on_remote_file_offer_clear(const uint8_t *p, uint32_t len);
void clipboard_on_paste_request(const uint8_t *p, uint32_t len);
void clipboard_on_paste_failed(const uint8_t *p, uint32_t len);
int  clipboard_request_remote_paste(void);
int  clipboard_direct_paste_target(uint32_t batch, char *out, size_t capacity);
void clipboard_complete_remote_direct_paste(uint32_t batch, int success);
void clipboard_complete_remote_paste(uint32_t batch, const char *const *paths, int n, int success);
void clipboard_apply(const char *utf8);   /* 收到对端文本 */
/* 数据通道收完一批后回调：把落盘的顶层条目挂到剪贴板 */
void clipboard_apply_paths(const char *const *paths, int n);
void clipboard_apply_image(const uint8_t *bgra, int w, int h, int stride);

/* display.c（阶段 4） */
int disp_wake(int sec);
void disp_wake_clear(void);
void disp_sleep(void);
int disp_switch_target(int target);
void disp_switch_message(const uint8_t *p, uint32_t len);
void mac_display_enqueue(const uint8_t *p, uint32_t len);
void disp_switch_shutdown(void);
void disp_switch_status(char *out, size_t cap);
void disp_set_input(uint16_t vcp);
void mac_display_get_inventory(hop_display_inventory *out);
int  mac_display_probe_async(void);
int  mac_display_identify(void);

/* update.m */
void mac_update_get_snapshot(hop_update_snapshot *out);
void mac_update_check(int target_channel);
void mac_update_download(void);
void mac_update_apply(void);
void mac_update_shutdown(void);

/* system_integration.m */
void mac_get_app_path(char *out, size_t cap);
int  mac_autostart_is_enabled(void);
int  mac_autostart_set_enabled(int enabled);
int  mac_permission_action(int action);

/* statusbar.m */
void statusbar_init(void);
void statusbar_open_main(void);
void statusbar_update(int connected, int sharing);
void mac_cursor_restore(void); /* clear macOS hide-until-mouse-moves after remote input */
void statusbar_show_transfers(void);
void statusbar_show_main(void);
void statusbar_check_updates(void);
void statusbar_notify_paste_ready(void);
void statusbar_notify_image_limit(void);
void main_ui_show_page(int page);
void main_ui_set_status(int connected, int sharing);
void main_ui_refresh(void);
void main_ui_shutdown(void);
void mac_check_input_permissions(void); /* once per launch; headless agents never prompt */
void mac_request_accessibility_permission(void); /* explicit actions can always reopen guidance */
void app_run(void);                 /* 启动 NSApplication 主循环 */

#endif
