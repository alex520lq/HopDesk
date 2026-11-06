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

static void dispatch_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;
    if (s_rejected) return;
    if (!s_authed && g_mac.cfg.role==HOP_ROLE_SERVER) {
        hop_hello_msg hello;
        if(type!=HOP_MSG_HELLO || !hop_hello_decode(p,len,&hello)){
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid HELLO or incompatible protocol");s_rejected=1;return;
        }
        if(!hop_token_matches(g_mac.cfg.token,hello.token,hello.token_len)){
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: token mismatch");s_rejected=1;return;
        }
        if(!hop_session_generate(&s_control_session)){
            hop_log(HOP_LOG_ERROR,"net: unable to create control session");s_rejected=1;return;
        }
        hop_session_publish(&s_control_session);s_authed=1;s_last_rx=hop_monotonic_ms();
        uint32_t pw=hello.width,ph=hello.height;
        dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});
        uint8_t ack[2+HOP_NAME_MAX+9+HOP_SESSION_BYTES];size_t nl=strlen(g_mac.cfg.device_name);
        ack[0]=1;ack[1]=(uint8_t)nl;memcpy(ack+2,g_mac.cfg.device_name,nl);
        hop_put_be32(ack+2+nl,(uint32_t)g_mac.screen_w);hop_put_be32(ack+6+nl,(uint32_t)g_mac.screen_h);
        ack[10+nl]=HOP_CTRL_PROTO_VER;memcpy(ack+11+nl,s_control_session.bytes,HOP_SESSION_BYTES);
        net_send(HOP_MSG_HELLO_ACK,ack,(uint32_t)nl+11+HOP_SESSION_BYTES);set_connected(1);return;
    }
    if (!s_authed) {
        int ok;
        uint32_t w, h;
        hop_session_id session;
        if (type != HOP_MSG_HELLO_ACK ||
            !hop_hello_ack_decode(p, len, &ok, &w, &h, &session) || !ok) {
            hop_log(HOP_LOG_WARN,"net: control handshake rejected: invalid ACK or incompatible protocol");
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
    if(g_mac.cfg.role==HOP_ROLE_SERVER && type==HOP_MSG_EDGE_LEAVE){
        dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});return;
    }
    if(g_mac.cfg.role==HOP_ROLE_SERVER && type>=HOP_MSG_MOUSE_MOVE && type<=HOP_MSG_KEY_MODS)return;
    switch (type) {
        case HOP_MSG_HELLO:
        case HOP_MSG_HELLO_ACK:
            s_rejected = 1;
            break;
        case HOP_MSG_PING:
            net_send(HOP_MSG_PONG, NULL, 0);
            break;
        case HOP_MSG_EDGE_LEAVE:
            if (g_mac.cfg.role == HOP_ROLE_CLIENT && len == 0) {
                inject_reset();
                statusbar_update(1, 0);
            }
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
        case HOP_MSG_CLIP_FILE_OFFER:
            clipboard_on_remote_file_offer(p, len);
            break;
        case HOP_MSG_CLIP_FILE_OFFER_CLEAR:
            clipboard_on_remote_file_offer_clear(p, len);
            break;
        case HOP_MSG_CLIP_PASTE_REQUEST:
            clipboard_on_paste_request(p, len);
            break;
        case HOP_MSG_CLIP_PASTE_FAILED:
            clipboard_on_paste_failed(p, len);
            break;
        case HOP_MSG_CLIP_TEXT:
            /* origin(4) + UTF-8；自己发出去的不写回 */
            if (len > 4 && hop_clip_should_apply_remote(hop_get_be32(p), (g_mac.cfg.role==HOP_ROLE_SERVER?HOP_DEV_SERVER:HOP_DEV_CLIENT))) {
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

typedef struct control_item {struct control_item *next;uint8_t type;uint32_t len;unsigned long generation;uint8_t data[];} control_item;
static pthread_mutex_t s_qlock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_qcond=PTHREAD_COND_INITIALIZER;
static control_item *s_qhead,*s_qtail;
static int s_qcount,s_online;
static size_t s_qbytes;
static _Atomic unsigned long s_generation;
static void queue_clear_locked(void){
    while(s_qhead){control_item *n=s_qhead->next;free(s_qhead);s_qhead=n;}
    s_qtail=NULL;s_qcount=0;s_qbytes=0;
}
int net_send(uint8_t type,const void *payload,uint32_t len){
    if(len>HOP_MAX_PAYLOAD || (len&&!payload))return 0;
    pthread_mutex_lock(&s_qlock);
    if(!s_online || (!hop_session_current(NULL)&&type!=HOP_MSG_HELLO)){pthread_mutex_unlock(&s_qlock);return 0;}
    hop_queue_verdict verdict=hop_ctrl_queue_admit(type,s_qcount,s_qbytes,len);
    if(verdict==HOP_Q_DROP){pthread_mutex_unlock(&s_qlock);return 1;}
    if(verdict==HOP_Q_OVERFLOW){queue_clear_locked();s_online=0;pthread_mutex_unlock(&s_qlock);
        net_request_reconnect();return 0;}
    control_item *m=malloc(sizeof(*m)+len);
    if(!m){pthread_mutex_unlock(&s_qlock);net_request_reconnect();return 0;}
    m->next=NULL;m->type=type;m->len=len;m->generation=s_generation;if(len)memcpy(m->data,payload,len);
    if(s_qtail)s_qtail->next=m;else s_qhead=m;s_qtail=m;s_qcount++;s_qbytes+=len;
    pthread_cond_signal(&s_qcond);pthread_mutex_unlock(&s_qlock);return 1;
}
static void *control_sender(void *unused){
    (void)unused;
    for(;;){
        pthread_mutex_lock(&s_qlock);
        while(!s_stop&&!s_qhead)pthread_cond_wait(&s_qcond,&s_qlock);
        if(s_stop){queue_clear_locked();pthread_mutex_unlock(&s_qlock);return NULL;}
        control_item *m=s_qhead;s_qhead=m->next;if(!s_qhead)s_qtail=NULL;s_qcount--;s_qbytes-=m->len;
        pthread_mutex_unlock(&s_qlock);
        pthread_mutex_lock(&s_lock);
        if(s_sock>=0 && m->generation==s_generation){
            uint8_t hdr[HOP_HEADER_SIZE];hop_encode_header(hdr,m->type,m->len);
            if(!socket_send_all(s_sock,hdr,sizeof(hdr)) || (m->len&&!socket_send_all(s_sock,m->data,m->len)))shutdown(s_sock,SHUT_RDWR);
        }
        pthread_mutex_unlock(&s_lock);free(m);
    }
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
static int s_listener=-1;
static int accept_control(void){
    if(s_listener<0){
        s_listener=socket(AF_INET,SOCK_STREAM,0);if(s_listener<0)return -1;
        int yes=1;setsockopt(s_listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(g_mac.cfg.port);a.sin_addr.s_addr=htonl(INADDR_ANY);
        if(bind(s_listener,(struct sockaddr*)&a,sizeof(a))||listen(s_listener,8)){
            hop_log(HOP_LOG_WARN,"net: control port %u unavailable (errno=%d)",(unsigned)g_mac.cfg.port,errno);
            close(s_listener);s_listener=-1;return -1;
        }
        hop_log(HOP_LOG_INFO,"net: listening on control port %u",(unsigned)g_mac.cfg.port);
    }
    while(!s_stop){fd_set rd;FD_ZERO(&rd);FD_SET(s_listener,&rd);struct timeval tv={0,200000};
        int n=select(s_listener+1,&rd,NULL,NULL,&tv);
        if(n>0){
            int peer=accept(s_listener,NULL,NULL);
            if(peer>=0){int yes=1;if(setsockopt(peer,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))){close(peer);return -1;}}
            return peer;
        }
        if(n<0&&errno!=EINTR){close(s_listener);s_listener=-1;return -1;}
    }
    return -1;
}
static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host;
    if(!host[0])return -1;
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
    clipboard_on_link(on);
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
    s_drop_requested=1;
    hop_log(HOP_LOG_INFO,"net: reconnect requested");
}

void net_request_stop(void) {
    s_stop = 1;
    pthread_mutex_lock(&s_qlock);pthread_cond_broadcast(&s_qcond);pthread_mutex_unlock(&s_qlock);
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
        int s = g_mac.cfg.role==HOP_ROLE_SERVER?accept_control():do_connect();
        if (s < 0) {
            if(g_mac.cfg.role==HOP_ROLE_CLIENT && g_mac.cfg.peer_host[0])
                hop_log(HOP_LOG_WARN,"net: TCP control connection failed: host=%s port=%u; check address, network and firewall",
                        g_mac.cfg.peer_host,(unsigned)g_mac.cfg.port);
            for(int i=0;i<backoff*10&&!s_stop;++i)usleep(100000);
            backoff = backoff < 10 ? backoff * 2 : 10;
            continue;
        }
        pthread_mutex_lock(&s_lock);
        s_sock = s;
        ++s_generation;
        pthread_mutex_unlock(&s_lock);
        backoff = 1;
        hop_log(HOP_LOG_INFO,"net: control TCP connected; awaiting handshake");

        net_set_keepalive(s);
        struct timeval rcv = {0, 500000}, snd = {HOP_CTRL_SEND_TIMEOUT_MS / 1000, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        hop_decoder_reset(s_dec);
        s_authed = s_rejected = 0;
        s_drop_requested=0;
        uint64_t started = hop_monotonic_ms(), last_ping = started;
        s_last_rx = started;
        pthread_mutex_lock(&s_qlock);queue_clear_locked();s_online=1;pthread_mutex_unlock(&s_qlock);
        if(g_mac.cfg.role==HOP_ROLE_CLIENT)send_hello();
        char buf[2048];
        while (!s_stop && !s_rejected && !s_drop_requested) {
            uint64_t now = hop_monotonic_ms();
            if (!s_authed && hop_handshake_expired(now, started)) {
                hop_log(HOP_LOG_WARN,"net: control handshake timed out");break;
            }
            if (s_authed && hop_peer_silent(now, s_last_rx)) break;
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
        ++s_generation;
        close(s);
        pthread_mutex_unlock(&s_lock);
        pthread_mutex_lock(&s_qlock);s_online=0;queue_clear_locked();pthread_mutex_unlock(&s_qlock);
        set_connected(0);
        xfer_kick();
        for(int i=0;i<backoff*10&&!s_stop;++i)usleep(100000);
        backoff = backoff < 10 ? backoff * 2 : 10;
    }
    if(s_listener>=0){close(s_listener);s_listener=-1;}
    return NULL;
}

void net_loop_run(void) {
    s_dec = hop_decoder_new(NULL, on_message);
    if (!s_dec) { hop_log(HOP_LOG_ERROR, "net: decoder allocation failed"); return; }
    pthread_t sender;
    if(pthread_create(&sender,NULL,control_sender,NULL)){hop_log(HOP_LOG_ERROR,"control sender creation failed");return;}
    pthread_detach(sender);
    pthread_t t;
    if (pthread_create(&t, NULL, net_thread, NULL)) {
        hop_decoder_free(s_dec); s_dec = NULL;
        hop_log(HOP_LOG_ERROR, "net: thread creation failed");
        return;
    }
    pthread_detach(t);
}
