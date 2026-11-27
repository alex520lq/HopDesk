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
    if (type == HOP_MSG_MOUSE_MOVE && len == 4 && s_qtail &&
        s_qtail->type == type && s_qtail->len == 4 &&
        s_qtail->generation == s_generation) {
        int dx = (int16_t)hop_get_be16(payload) + (int16_t)hop_get_be16(s_qtail->data);
        int dy = (int16_t)hop_get_be16((const uint8_t *)payload + 2) +
                 (int16_t)hop_get_be16(s_qtail->data + 2);
        if (dx >= -32768 && dx <= 32767 && dy >= -32768 && dy <= 32767) {
            hop_put_be16(s_qtail->data, (uint16_t)(int16_t)dx);
            hop_put_be16(s_qtail->data + 2, (uint16_t)(int16_t)dy);
            pthread_mutex_unlock(&s_qlock);
            return 1;
        }
    }
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
            int ok;
            if (m->len <= 1024u) {
                uint8_t frame[HOP_HEADER_SIZE + 1024u];
                uint32_t total = hop_encode_frame(m->type, m->data, m->len, frame);
                ok = socket_send_all(s_sock, frame, total);
            } else {
                uint8_t hdr[HOP_HEADER_SIZE];
                hop_encode_header(hdr, m->type, m->len);
                ok = socket_send_all(s_sock, hdr, sizeof(hdr)) &&
                     socket_send_all(s_sock, m->data, m->len);
            }
            if(!ok)shutdown(s_sock,SHUT_RDWR);
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
    int resolved = getaddrinfo(host, portstr, &hints, &res);
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
    while(!s_stop && !s_manual_disconnect){fd_set rd;FD_ZERO(&rd);FD_SET(s_listener,&rd);struct timeval tv={0,200000};
        int n=select(s_listener+1,&rd,NULL,NULL,&tv);
        if(n>0){
            int peer=accept(s_listener,NULL,NULL);
            if(peer>=0){int yes=1;if(setsockopt(peer,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))){close(peer);return -1;}}
            return peer;
        }
        if(n<0&&errno!=EINTR){close(s_listener);s_listener=-1;return -1;}
    }
    if(s_manual_disconnect && s_listener>=0){close(s_listener);s_listener=-1;}
    return -1;
}
static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host;
    if(!host[0])return -1;
    unsigned connect_ms = g_mac.cfg.net_connect_timeout_ms >= 500
        ? (unsigned)g_mac.cfg.net_connect_timeout_ms : (unsigned)HOP_CONNECT_TIMEOUT_MS;
    return net_connect_timeout(host, g_mac.cfg.port, connect_ms);
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

unsigned net_get_input_mode(void) {
    return atomic_load_explicit(&s_local_input_mode, memory_order_acquire) & HOP_INPUT_MODES;
}

void net_get_peer_capabilities(unsigned *peer_input, unsigned *peer_audio, int *known,
                               char *peer_name, size_t peer_name_cap) {
    if (peer_input) *peer_input = atomic_load_explicit(&s_peer_input_mode, memory_order_acquire) & HOP_INPUT_MODES;
    if (peer_audio) *peer_audio = atomic_load_explicit(&s_peer_audio_mode, memory_order_acquire) & HOP_AUDIO_MODES;
    if (known) *known = atomic_load_explicit(&s_peer_caps_known, memory_order_acquire) != 0;
    if (peer_name && peer_name_cap) {
        pthread_mutex_lock(&s_lock);
        snprintf(peer_name, peer_name_cap, "%s", s_peer_name);
        pthread_mutex_unlock(&s_lock);
    }
}

void net_send_capabilities(unsigned takeover_mask) {
    uint8_t payload[HOP_CAPABILITIES_BYTES];
    unsigned local_input = net_get_input_mode();
    if (hop_capabilities_encode(payload, local_input, takeover_mask & HOP_INPUT_MODES,
                                g_mac.cfg.audio_mode & HOP_AUDIO_MODES))
        (void)net_send(HOP_MSG_CAPABILITIES, payload, sizeof(payload));
}

void net_apply_input_mode(unsigned mode, unsigned takeover_mask, int persist) {
    mode &= HOP_INPUT_MODES;
    atomic_store_explicit(&s_local_input_mode, mode, memory_order_release);
    g_mac.cfg.input_mode = mode;
    mac_capture_set_input_mode(mode);
    if (persist && g_mac.config_path[0]) {
        hop_config saved = g_mac.cfg;
        (void)hop_config_load(&saved, g_mac.config_path);
        saved.input_mode = mode;
        (void)hop_config_save_atomic(&saved, g_mac.config_path);
    }
    net_send_capabilities(takeover_mask & HOP_INPUT_MODES);
}

static void set_connected(int on) {
    clipboard_on_link(on);
    if (on) {
        atomic_store_explicit(&s_peer_caps_known, 0, memory_order_release);
        atomic_store_explicit(&s_peer_input_mode, 0, memory_order_release);
        atomic_store_explicit(&s_peer_audio_mode, 0, memory_order_release);
        net_send_capabilities(0);
    } else {
        atomic_store_explicit(&s_peer_caps_known, 0, memory_order_release);
        atomic_store_explicit(&s_peer_input_mode, 0, memory_order_release);
        atomic_store_explicit(&s_peer_audio_mode, 0, memory_order_release);
        pthread_mutex_lock(&s_lock);
        s_peer_name[0] = '\0';
        pthread_mutex_unlock(&s_lock);
        hop_session_clear(&s_control_session);
        pthread_mutex_lock(&s_dispatch_lock);
        inject_reset();
        pthread_mutex_unlock(&s_dispatch_lock);
    }
    hop_log(HOP_LOG_INFO, "net: control channel %s", on ? "connected" : "disconnected");
    statusbar_update(on, 0);
}

int net_is_disconnected(void) {
    return atomic_load_explicit(&s_manual_disconnect, memory_order_acquire) != 0;
}

void net_set_disconnected(int disconnected) {
    int pause = disconnected ? 1 : 0;
    int prev = atomic_exchange_explicit(&s_manual_disconnect, pause, memory_order_acq_rel);
    if (pause) {
        s_drop_requested = 1;
        pthread_mutex_lock(&s_qlock); s_online = 0; queue_clear_locked(); pthread_mutex_unlock(&s_qlock);
        pthread_mutex_lock(&s_lock);
        if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
        pthread_mutex_unlock(&s_lock);
        hop_session_id current;
        if (hop_session_current(&current)) hop_session_clear(&current);
        xfer_kick();
        if (!prev) hop_log(HOP_LOG_INFO, "net: manually disconnected");
        statusbar_update(0, 0);
    } else if (prev) {
        hop_log(HOP_LOG_INFO, "net: connection resumed");
        statusbar_update(0, 0);
    }
}

void net_request_reconnect(void) {
    atomic_store_explicit(&s_manual_disconnect, 0, memory_order_release);
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
        if (s_manual_disconnect) {
            if (s_listener >= 0) { close(s_listener); s_listener = -1; }
            backoff = 1;
            usleep(100000);
            continue;
        }
        s_drop_requested = 0;
        int s = g_mac.cfg.role==HOP_ROLE_SERVER?accept_control():do_connect();
        if (s_manual_disconnect) {
            if (s >= 0) close(s);
            if (s_listener >= 0) { close(s_listener); s_listener = -1; }
            backoff = 1;
            continue;
        }
        if (s < 0) {
            if(g_mac.cfg.role==HOP_ROLE_CLIENT && g_mac.cfg.peer_host[0])
                hop_log(HOP_LOG_WARN,"net: TCP control connection failed: host=%s port=%u; check address, network and firewall",
                        g_mac.cfg.peer_host,(unsigned)g_mac.cfg.port);
            for(int i=0;i<backoff*10&&!s_stop&&!s_manual_disconnect&&!s_drop_requested;++i)usleep(100000);
            int max_b = g_mac.cfg.net_reconnect_max_sec >= 1 ? g_mac.cfg.net_reconnect_max_sec : 10;
            backoff = s_drop_requested ? 1 : (backoff < max_b ? backoff * 2 : max_b);
            continue;
        }
        pthread_mutex_lock(&s_lock);
        s_sock = s;
        ++s_generation;
        pthread_mutex_unlock(&s_lock);
        backoff = 1;
        hop_log(HOP_LOG_INFO,"net: control TCP connected; awaiting handshake");

        net_set_keepalive(s);
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        unsigned snd_ms = g_mac.cfg.net_send_timeout_ms >= 500
            ? (unsigned)g_mac.cfg.net_send_timeout_ms : (unsigned)HOP_CTRL_SEND_TIMEOUT_MS;
        struct timeval rcv = {0, 500000}, snd = {(time_t)(snd_ms / 1000u), (suseconds_t)((snd_ms % 1000u) * 1000u)};
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
        while (!s_stop && !s_rejected && !s_drop_requested && !s_manual_disconnect) {
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
            if (r <= 0) break;
            // Input injection can autorelease Cocoa objects on this long-lived pthread.
            @autoreleasepool {
                if (!hop_decoder_feed(s_dec, (uint8_t *)buf, (uint32_t)r)) break;
            }
        }

        pthread_mutex_lock(&s_lock);
        s_sock = -1;
        ++s_generation;
        close(s);
        pthread_mutex_unlock(&s_lock);
        pthread_mutex_lock(&s_qlock);s_online=0;queue_clear_locked();pthread_mutex_unlock(&s_qlock);
        set_connected(0);
        xfer_kick();
        if (!s_manual_disconnect) {
            s_drop_requested = 0;
            for(int i=0;i<backoff*10&&!s_stop&&!s_manual_disconnect&&!s_drop_requested;++i)usleep(100000);
            int max_b = g_mac.cfg.net_reconnect_max_sec >= 1 ? g_mac.cfg.net_reconnect_max_sec : 10;
            backoff = s_drop_requested ? 1 : (backoff < max_b ? backoff * 2 : max_b);
        }
    }
    if(s_listener>=0){close(s_listener);s_listener=-1;}
    return NULL;
}

void net_loop_run(void) {
    s_stop = 0;
    atomic_store_explicit(&s_local_input_mode, g_mac.cfg.input_mode & HOP_INPUT_MODES, memory_order_release);
    s_dec = hop_decoder_new_with_limit(NULL, on_message, HOP_CTRL_MAX_PAYLOAD);
    if (!s_dec) { hop_log(HOP_LOG_ERROR, "net: decoder allocation failed"); return; }
    if (pthread_create(&s_control_sender_thread, NULL, control_sender, NULL)) {
        hop_log(HOP_LOG_ERROR, "control sender creation failed");
        hop_decoder_free(s_dec); s_dec = NULL;
        return;
    }
    s_control_sender_started = 1;
    if (pthread_create(&s_net_thread, NULL, net_thread, NULL)) {
        hop_log(HOP_LOG_ERROR, "net: thread creation failed");
        net_request_stop();
        pthread_join(s_control_sender_thread, NULL);
        s_control_sender_started = 0;
        hop_decoder_free(s_dec); s_dec = NULL;
        return;
    }
    s_net_thread_started = 1;
}

/* The control reader can call xfer_kick while handling disconnect/reconnect.
 * Join both control workers before xfer_shutdown destroys the transfer locks. */
void net_loop_shutdown(void) {
    disp_switch_shutdown();
    if (!atomic_load_explicit(&s_stop, memory_order_acquire))
        net_request_stop();
    if (s_net_thread_started) {
        pthread_join(s_net_thread, NULL);
        s_net_thread_started = 0;
    }
    if (s_control_sender_started) {
        pthread_join(s_control_sender_thread, NULL);
        s_control_sender_started = 0;
    }
    if (s_dec) {
        hop_decoder_free(s_dec);
        s_dec = NULL;
    }
}
