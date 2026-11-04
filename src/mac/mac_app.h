/* =====================================================================
 *  HopDesk - macOS 应用全局状态（C 核心 + 少量 ObjC）
 * ===================================================================== */
#ifndef MAC_APP_H
#define MAC_APP_H

#include "hop.h"

typedef struct {
    hop_config cfg; /* immutable after main startup; all threads read-only */
    /* Geometry initialized before threads; later refresh/read only in input/control dispatch. */
    double     screen_w, screen_h;   /* 本端 CoreGraphics 坐标空间（逻辑点） */
    char       config_path[1024];    /* canonical Application Support config */
    char       log_path[1024];       /* active log path */
} mac_app;

extern mac_app g_mac;
void local_addresses(char *out,size_t cap);

/* net_client.c */
void net_loop_run(void);                 /* 连接 + 重连 + 接收（后台线程） */
int  net_send(uint8_t type, const void *p, uint32_t len);
void net_request_stop(void);
void net_request_reconnect(void); /* close current socket; background loop reconnects */
/* 打开 TCP keepalive：空闲连接变黑洞时让内核在一分多钟内判定死亡，
 * 否则阻塞的 read 永远不返回，通道就此僵死。控制/数据通道都要调。 */
void net_set_keepalive(int s);
int net_connect_timeout(const char *host, uint16_t port, unsigned timeout_ms);

/* inject.c */
void mac_capture_init(void);
void mac_capture_connected(int connected); /* main queue */
void mac_capture_return(void); /* main queue */
void mac_capture_peer_geometry(uint32_t w,uint32_t h);
void inject_init(void);
void inject_set_peer_geometry(uint32_t w, uint32_t h);
void inject_enter(int y);          /* 服务端移交：在边缘对应 y 进入 */
void inject_move(int dx, int dy);
void inject_button(int button, int down);
void inject_wheel(int dx, int dy);
void inject_reset(void);
void inject_key(uint16_t kc, int down);   /* 阶段 2：映射后的 Mac 键码 */
void inject_release_all(void);            /* 断线时松开所有修饰键 */

/* clipboard.m */
void clipboard_init(void);
void clipboard_apply(const char *utf8);   /* 收到对端文本 */
/* 数据通道收完一批后回调：把落盘的顶层条目挂到剪贴板 */
void clipboard_apply_paths(const char *const *paths, int n);
void clipboard_apply_image(const uint8_t *bgra, int w, int h, int stride);

/* display.c（阶段 4） */
void disp_wake(int sec);
void disp_sleep(void);
void disp_set_input(uint16_t vcp);

/* statusbar.m */
void statusbar_init(void);
void statusbar_update(int connected, int sharing);
void statusbar_show_transfers(void);
void statusbar_show_main(void);
void statusbar_check_updates(void);
void main_ui_show_page(int page);
void main_ui_set_status(int connected, int sharing);
void main_ui_refresh(void);
void main_ui_shutdown(void);
void mac_request_accessibility_permission(void); /* main queue: request and open the Accessibility settings pane */
void app_run(void);                 /* 启动 NSApplication 主循环 */

#endif
