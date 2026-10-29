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
    double     screen_w, screen_h;
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

/* statusbar.m */
void statusbar_init(void);
void statusbar_update(int connected, int sharing);
void app_run(void);                 /* 启动 NSApplication 主循环 */

#endif
