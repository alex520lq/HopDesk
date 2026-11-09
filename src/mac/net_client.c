/* =====================================================================
 *  HopDesk - macOS 网络客户端（POSIX socket）
 *  后台线程：连接 -> 握手 -> 接收；断开指数退避重连。
 * ===================================================================== */
#include "mac_app.h"
#include "xfer.h"
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdatomic.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mac_app g_mac;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_sock = -1; /* exclusively under s_lock */
static hop_session_id s_control_session; /* control reader owns; registry publishes snapshots */
static _Atomic int     s_stop;
static _Atomic int s_drop_requested;
static int             s_authed, s_rejected;
static uint64_t        s_last_rx;
static void set_connected(int on);
static hop_decoder    *s_dec;

#include "net_client_protocol.inc"

static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    int guard = hop_watchdog_begin("control receive frame", HOP_IO_DEADLINE_MS);
    pthread_mutex_lock(&s_dispatch_lock);
    if (!s_stop) dispatch_message(user, type, p, len);
    pthread_mutex_unlock(&s_dispatch_lock);
    hop_watchdog_end(guard);
}

#include "net_client_transport.inc"
