/* =====================================================================
 *  HopDesk - Windows 网络服务端（Winsock）
 *  单客户端、后台线程 accept/recv，事件经窗口消息回主线程。
 * ===================================================================== */
#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <windows.h>
#include "hop.h"

/* 自定义窗口消息（发给主窗口） */
#define WM_APP_LINK   (WM_APP + 1)   /* wParam: 1=已连接 0=断开 */
#define WM_APP_RECV   (WM_APP + 2)   /* wParam: type, lParam: hop_recv*  */

/* 随 WM_APP_RECV 传递，主线程处理后 free */
typedef struct {
    uint8_t  type;
    uint32_t len;
    uint8_t  data[1];
} hop_recv;

typedef struct net_server net_server;

net_server *net_server_start(HWND hwnd, const hop_config *cfg);
void        net_server_stop(net_server *s);

/* 线程安全发送；返回 0 失败（无连接或发送错误） */
int         net_server_send(net_server *s, uint8_t type,
                           const void *payload, uint32_t len);
int         net_server_has_client(net_server *s);

/* 主动断开当前客户端（例如握手 token 不匹配） */
void        net_server_drop(net_server *s);

#endif
