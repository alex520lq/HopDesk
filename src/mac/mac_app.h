/* =====================================================================
 *  HopDesk - macOS 应用全局状态（C 核心 + 少量 ObjC）
 * ===================================================================== */
#ifndef MAC_APP_H
#define MAC_APP_H

#include "hop.h"

typedef struct {
    hop_config cfg;
    int        sock;
    int        connected;
    int        sharing;        /* 鼠标当前是否在 Mac（被服务端移交） */
    double     mouse_x, mouse_y;
    double     screen_w, screen_h;   /* 本端 CoreGraphics 坐标空间（逻辑点） */
    double     peer_w, peer_h;       /* 服务端（Windows）坐标空间，用于位移缩放 */
    int        btn_held[3];
} mac_app;

extern mac_app g_mac;

/* net_client.c */
void net_loop_run(void);                 /* 连接 + 重连 + 接收（后台线程） */
int  net_send(uint8_t type, const void *p, uint32_t len);
void net_request_stop(void);

/* inject.c */
void inject_init(void);
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
void app_run(void);                 /* 启动 NSApplication 主循环 */

#endif
