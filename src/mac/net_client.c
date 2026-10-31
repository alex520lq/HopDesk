/* =====================================================================
 *  HopDesk - macOS 网络客户端（POSIX socket）
 *  后台线程：连接 -> 握手 -> 接收；断开指数退避重连。
 * ===================================================================== */
#include "mac_app.h"

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
static int             s_stop;
static hop_decoder    *s_dec;

static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;
    switch (type) {
        case HOP_MSG_PING:
            net_send(HOP_MSG_PONG, NULL, 0);
            break;
        case HOP_MSG_HELLO_ACK: {
            /* {ok, nlen, name, w, h} —— w,h 是服务端（Windows）尺寸，
             * 存进 peer_* 用于位移缩放，不能覆盖本端自己的 screen_*。 */
            if (len >= 2) {
                uint32_t off = 2 + p[1];
                if (off + 8 <= len) {
                    g_mac.peer_w = (double)hop_get_be32(p + off);
                    g_mac.peer_h = (double)hop_get_be32(p + off + 4);
                }
            }
            break;
        }
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
            if (len > 4 && hop_get_be32(p) != HOP_DEV_CLIENT) {
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
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);

    pthread_mutex_lock(&s_lock);
    int s = g_mac.sock;
    int ok = 0;
    if (s >= 0) {
        ok = 1;
        ok = socket_send_all(s, hdr, HOP_HEADER_SIZE);
        if (ok && len) ok = socket_send_all(s, payload, len);
    }
    pthread_mutex_unlock(&s_lock);
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
    net_send(HOP_MSG_HELLO, buf, n);
}

static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host[0] ? g_mac.cfg.peer_host : "192.168.31.8";
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)g_mac.cfg.port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int s = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        s = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (s < 0) continue;
        int no_sigpipe = 1;
        if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            close(s);
            s = -1;
            continue;
        }
        if (connect(s, r->ai_addr, r->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
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
    g_mac.connected = on;
    if (on) {
        g_mac.sharing = 0;
        send_hello();
    } else {
        g_mac.sharing = 0;
        inject_reset();
    }
    statusbar_update(on, 0);
}

void net_request_stop(void) {
    s_stop = 1;
    pthread_mutex_lock(&s_lock);
    if (g_mac.sock >= 0) { shutdown(g_mac.sock, SHUT_RDWR); close(g_mac.sock); g_mac.sock = -1; }
    pthread_mutex_unlock(&s_lock);
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
        g_mac.sock = s;
        pthread_mutex_unlock(&s_lock);
        backoff = 1;

        /* 下面的 read 是阻塞的：连接变黑洞时它会永远不返回，
         * 控制通道就此彻底僵死。keepalive 是唯一能让它返回的机制。 */
        net_set_keepalive(s);

        hop_decoder_reset(s_dec);
        set_connected(1);

        char buf[2048];
        for (;;) {
            ssize_t r = read(s, buf, sizeof(buf));
            if (r <= 0) break;
            hop_decoder_feed(s_dec, (uint8_t *)buf, (uint32_t)r);
        }

        pthread_mutex_lock(&s_lock);
        g_mac.sock = -1;
        close(s);
        pthread_mutex_unlock(&s_lock);
        if (!s_stop) set_connected(0);
        if (!s_stop) sleep(backoff);
        backoff = backoff < 10 ? backoff * 2 : 10;
    }
    return NULL;
}

void net_loop_run(void) {
    s_dec = hop_decoder_new(NULL, on_message);
    pthread_t t;
    pthread_create(&t, NULL, net_thread, NULL);
    pthread_detach(t);
}
