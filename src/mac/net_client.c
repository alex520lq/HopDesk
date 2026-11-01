/* =====================================================================
 *  HopDesk - macOS 网络客户端（POSIX socket）
 *  后台线程：连接 -> 握手 -> 接收；断开指数退避重连。
 * ===================================================================== */
#include "mac_app.h"
#include "xfer.h"
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
static int             s_authed, s_rejected;
static uint64_t        s_last_rx;
static void set_connected(int on);
static hop_decoder    *s_dec;

static void dispatch_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;
    if (s_rejected) return;
    if (!s_authed) {
        int ok;
        uint32_t w, h;
        hop_session_id session;
        if (type != HOP_MSG_HELLO_ACK ||
            !hop_hello_ack_decode(p, len, &ok, &w, &h, &session) || !ok) {
            s_rejected = 1;
            return;
        }
        inject_set_peer_geometry(w, h);
        s_control_session = session; hop_session_publish(&session);
        s_authed = 1; s_last_rx = hop_monotonic_ms();
        set_connected(1);
        return;
    }
    s_last_rx = hop_monotonic_ms();
    switch (type) {
        case HOP_MSG_HELLO:
        case HOP_MSG_HELLO_ACK:
            s_rejected = 1;
            break;
        case HOP_MSG_PING:
            net_send(HOP_MSG_PONG, NULL, 0);
            break;
        case HOP_MSG_MOUSE_ENTER:
            if (len >= 2) inject_enter((int)hop_get_be16(p));
            break;
        case HOP_MSG_MOUSE_MOVE:
            if (len >= 4)
                inject_move((int16_t)hop_get_be16(p),
                           (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_MOUSE_BUTTON:
            if (len >= 2) inject_button(p[0], p[1]);
            break;
        case HOP_MSG_MOUSE_WHEEL:
            if (len >= 4)
                inject_wheel((int16_t)hop_get_be16(p),
                            (int16_t)hop_get_be16(p + 2));
            break;
        case HOP_MSG_KEY_DOWN:
            if (len >= 2) inject_key(hop_get_be16(p), 1);
            break;
        case HOP_MSG_KEY_UP:
            if (len >= 2) inject_key(hop_get_be16(p), 0);
            break;
        case HOP_MSG_CLIP_TEXT:
            /* origin(4) + UTF-8；自己发出去的不写回 */
            if (len > 4 && hop_clip_should_apply_remote(hop_get_be32(p), HOP_DEV_CLIENT)) {
                char *u = (char *)malloc((size_t)(len - 3));
                if (u) {
                    memcpy(u, p + 4, len - 4);
                    u[len - 4] = '\0';
                    clipboard_apply(u);
                    free(u);
                }
            }
            break;
        case HOP_MSG_DISP_WAKE:
            if (len >= 4) disp_wake((int)hop_get_be32(p));
            break;
        case HOP_MSG_DISP_INPUT:
            if (len >= 2) disp_set_input(hop_get_be16(p));
            break;
        case HOP_MSG_DISP_SLEEP:
            disp_sleep();
            break;
        default:
            break;
    }
}

static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    int guard = hop_watchdog_begin("control receive frame", HOP_IO_DEADLINE_MS);
    pthread_mutex_lock(&s_dispatch_lock);
    if (!s_stop) dispatch_message(user, type, p, len);
    pthread_mutex_unlock(&s_dispatch_lock);
    hop_watchdog_end(guard);
}

static int socket_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            /* 帧可能已经部分写出；必须让读线程感知断线，不能复用已损坏的流。 */
            shutdown(fd, SHUT_RDWR);
            return 0;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

int net_send(uint8_t type, const void *payload, uint32_t len) {
    int guard = hop_watchdog_begin("control frame send", 10000);
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);

    pthread_mutex_lock(&s_lock);
    int s = s_sock;
    int ok = 0;
    if (s >= 0 && (hop_session_current(NULL) || type == HOP_MSG_HELLO)) {
        ok = 1;
        ok = socket_send_all(s, hdr, HOP_HEADER_SIZE);
        if (ok && len) ok = socket_send_all(s, payload, len);
    }
    pthread_mutex_unlock(&s_lock);
    hop_watchdog_end(guard);
    return ok;
}

static void send_hello(void) {
    uint8_t buf[2 + HOP_TOKEN_MAX + 2 + HOP_NAME_MAX + 8];
    uint32_t n = 0;
    const char *tok = g_mac.cfg.token;
    uint8_t tl = (uint8_t)strlen(tok);
    buf[n++] = tl; memcpy(buf + n, tok, tl); n += tl;
    const char *nm = g_mac.cfg.device_name[0] ? g_mac.cfg.device_name : "Alex67deMac";
    uint8_t nl = (uint8_t)strlen(nm);
    buf[n++] = nl; memcpy(buf + n, nm, nl); n += nl;
    hop_put_be32(buf + n, (uint32_t)g_mac.screen_w); n += 4;
    hop_put_be32(buf + n, (uint32_t)g_mac.screen_h); n += 4;
    buf[n++] = HOP_CTRL_PROTO_VER;
    net_send(HOP_MSG_HELLO, buf, n);
}

/* DNS resolution uses the OS resolver; the TCP attempts share a bounded deadline. */
int net_connect_timeout(const char *host, uint16_t port, unsigned timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    int guard = hop_watchdog_begin("DNS resolution", 5000);
    int resolved = getaddrinfo(host, portstr, &hints, &res);
    hop_watchdog_end(guard);
    if (resolved) return -1;
    int fd = -1;
    uint64_t deadline = hop_monotonic_ms() + timeout_ms;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        if (hop_monotonic_ms() >= deadline) break;
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        int on = 1, flags = fcntl(fd, F_GETFL, 0);
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) ||
            flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) goto next;
        int rc = connect(fd, r->ai_addr, r->ai_addrlen);
        if (rc && errno != EINPROGRESS) goto next;
        if (rc) {
            for (;;) {
                uint64_t now = hop_monotonic_ms();
                if (now >= deadline) goto next;
                uint64_t left = deadline - now;
                struct timeval tv = {(time_t)(left / 1000), (suseconds_t)(left % 1000) * 1000};
                fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
                rc = select(fd + 1, NULL, &wr, NULL, &tv);
                if (rc < 0 && errno == EINTR) continue;
                if (rc <= 0) goto next;
                int error = 0; socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) goto next;
                break;
            }
        }
        if (fcntl(fd, F_SETFL, flags)) goto next;
        break;
next:
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host[0] ? g_mac.cfg.peer_host : "192.168.31.8";
    return net_connect_timeout(host, g_mac.cfg.port, HOP_CONNECT_TIMEOUT_MS);
}

/* 给已连接的 socket 打开 TCP keepalive（控制通道与数据通道都用）。
 *
 * 没有它，链路中间设备把空闲连接的表项丢掉之后 socket 会变成黑洞：
 * write 只进本地发送缓冲并返回成功，read 永远等不到数据，两端都以为
 * 还连着。实测空闲数小时后正是这个状态 —— 对端发文件过来完全收不到，
 * 日志里连 "data channel closed" 都没有，只能重启进程。
 * keepalive 让内核约 90 秒内判定连接死亡，read 返回错误后走既有重连。
 *
 * macOS 的空闲时长选项叫 TCP_KEEPALIVE（不是 Linux 的 TCP_KEEPIDLE）。 */
void net_set_keepalive(int s) {
    int on = 1;
    int idle = 60, intvl = 10, cnt = 3;

    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) {
        hop_log(HOP_LOG_WARN, "net: SO_KEEPALIVE 失败 (errno=%d)", errno);
        return;
    }
    setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

static void set_connected(int on) {
    if (!on) {
        hop_session_clear(&s_control_session);
        pthread_mutex_lock(&s_dispatch_lock);
        inject_reset();
        pthread_mutex_unlock(&s_dispatch_lock);
    }
    hop_log(HOP_LOG_INFO, "net: control channel %s", on ? "connected" : "disconnected");
    statusbar_update(on, 0);
}

void net_request_reconnect(void) {
    pthread_mutex_lock(&s_lock);
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
    pthread_mutex_unlock(&s_lock);
    hop_log(HOP_LOG_INFO, "net: reconnect requested from UI");
}

void net_request_stop(void) {
    s_stop = 1;
    pthread_mutex_lock(&s_lock);
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
    pthread_mutex_unlock(&s_lock);
    hop_session_id current;
    if (hop_session_current(&current)) hop_session_clear(&current);
    /* The menu can terminate the process before the reader gets scheduled. */
    pthread_mutex_lock(&s_dispatch_lock);
    inject_reset();
    pthread_mutex_unlock(&s_dispatch_lock);
}

static void *net_thread(void *arg) {
    (void)arg;
    int backoff = 1;
    while (!s_stop) {
        int s = do_connect();
        if (s < 0) {
            sleep(backoff);
            backoff = backoff < 10 ? backoff * 2 : 10;
            continue;
        }
        pthread_mutex_lock(&s_lock);
        s_sock = s;
        pthread_mutex_unlock(&s_lock);
        backoff = 1;

        net_set_keepalive(s);
        struct timeval rcv = {0, 500000}, snd = {HOP_CTRL_SEND_TIMEOUT_MS / 1000, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        hop_decoder_reset(s_dec);
        s_authed = s_rejected = 0;
        uint64_t started = hop_monotonic_ms(), last_ping = started;
        s_last_rx = started;
        send_hello();
        char buf[2048];
        while (!s_stop && !s_rejected) {
            uint64_t now = hop_monotonic_ms();
            if ((!s_authed && hop_handshake_expired(now, started)) ||
                (s_authed && hop_peer_silent(now, s_last_rx))) break;
            if (s_authed && hop_heartbeat_due(now, last_ping)) {
                if (!net_send(HOP_MSG_PING, NULL, 0)) break;
                last_ping = now;
            }
            ssize_t r = read(s, buf, sizeof(buf));
            if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (r <= 0 || !hop_decoder_feed(s_dec, (uint8_t *)buf, (uint32_t)r)) break;
        }

        pthread_mutex_lock(&s_lock);
        s_sock = -1;
        close(s);
        pthread_mutex_unlock(&s_lock);
        set_connected(0);
        xfer_kick();
        if (!s_stop) sleep(backoff);
        backoff = backoff < 10 ? backoff * 2 : 10;
    }
    return NULL;
}

void net_loop_run(void) {
    s_dec = hop_decoder_new(NULL, on_message);
    if (!s_dec) { hop_log(HOP_LOG_ERROR, "net: decoder allocation failed"); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, net_thread, NULL)) {
        hop_decoder_free(s_dec); s_dec = NULL;
        hop_log(HOP_LOG_ERROR, "net: thread creation failed");
        return;
    }
    pthread_detach(t);
}
