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
    unsigned long epoch;
    uint8_t  data[1];
} hop_recv;

typedef struct net_server net_server;

net_server *net_server_start(HWND hwnd, const hop_config *cfg);
void        net_server_stop(net_server *s);

/* 线程安全发送；返回 0 失败（无连接或发送错误） */
int         net_server_send(net_server *s, uint8_t type,
                           const void *payload, uint32_t len);
int         net_server_has_client(net_server *s);
int         net_server_listening(net_server *s);
unsigned long net_server_epoch(net_server *s);

/* 主动断开当前客户端（例如握手 token 不匹配） */
void        net_server_drop(net_server *s);

/* 给已连接的 socket 打开 TCP keepalive（控制通道与数据通道都用）。
 *
 * 没有它，链路中间设备把空闲连接的表项丢掉之后 socket 会变成黑洞：
 * send 只进本地发送缓冲并返回成功，recv 永远等不到数据，两端都以为
 * 还连着。实测空闲数小时后正是这个状态 —— 文件发出去 10 秒 ACK 超时、
 * 作业级重连也拉不起来，只能重启进程。keepalive 让内核约 90 秒内判定
 * 连接死亡，上层才有机会走既有的重连逻辑。 */
void        net_set_keepalive(SOCKET s);
SOCKET net_connect_timeout_win(const char *host, uint16_t port, HANDLE stop);

#endif
