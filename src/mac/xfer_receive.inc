/* Included by xfer.c in the same translation unit; not compiled separately. */
/* ---------------- 写盘队列 ----------------
 *
 * 网络线程只做"收包 + 校验 + 入队"，写盘交给独立线程。
 *
 * 队列有界（RX_Q_SLOTS）。满了网络线程就阻塞在 wq_push，
 * recv 随之停止 → TCP 接收窗口收缩 → 发送端自动减速。
 * 这就是显式背压：内存上界 = RX_Q_SLOTS × chunk，不随传输时长增长。
 * 文件描述符由写线程独占，避免"网络线程关文件、写线程还在写"的竞态。
 */
enum { RX_OPEN = 1, RX_CLOSE };

typedef struct {
    int kind;
    char path[4096];
} rx_task;

static hop_writer_queue *s_wq;

static int wq_reserve_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    return hop_xfer_task_reserve_rx_buffer((hop_xfer_task *)owner, bytes);
}
static void wq_release_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    (void)hop_xfer_task_release_rx_buffer((hop_xfer_task *)owner, bytes);
}
static int wq_watchdog_begin(void *context, const char *label, uint32_t deadline) {
    (void)context;
    return hop_watchdog_begin(label, deadline);
}
static void wq_watchdog_end(void *context, int guard) {
    (void)context;
    hop_watchdog_end(guard);
}
static uint64_t wq_now_ns(void *context) {
    (void)context;
    return rate_now_ns();
}
static void wq_write_time(void *context, void *owner, uint64_t elapsed_ns) {
    (void)context;
    hop_xfer_task_snapshot snapshot;
    if (owner && hop_xfer_task_get_snapshot(((hop_xfer_task *)owner)->id, &snapshot))
        status_stage_time(HOP_XFER_DIR_RECEIVE, snapshot.transfer_id,
                          HOP_XFER_TIME_WRITE, elapsed_ns);
}
static void wq_open_error(void *context, const char *path, int error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: cannot create %s (%s)", path, strerror(error));
}
static void wq_write_error(void *context, uint64_t offset, int error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: write failed at %llu (%s)",
            (unsigned long long)offset, strerror(error));
}

static int wq_start(void) {
    if (s_wq) return 0;
    const hop_writer_queue_callbacks callbacks = {
        HOP_IO_DEADLINE_MS,
        wq_reserve_task_rx, wq_release_task_rx,
        wq_watchdog_begin, wq_watchdog_end, wq_now_ns,
        wq_write_time, wq_open_error, wq_write_error
    };
    s_wq = hop_writer_queue_create(NULL, &callbacks);
    return s_wq != NULL;
}

static int wq_push(const rx_task *task) {
    if (!s_wq || !task) return 0;
    if (task->kind == RX_OPEN)
        return hop_writer_queue_enqueue_open(s_wq, task->path);
    if (task->kind == RX_CLOSE)
        return hop_writer_queue_enqueue_close(s_wq);
    return 0;
}

static int wq_push_data(hop_xfer_task *owner, uint64_t offset,
                        const uint8_t *data, uint32_t length) {
    return s_wq && hop_writer_queue_enqueue_write(s_wq, owner, offset, data, length);
}

static void wq_drain(void) {
    hop_writer_queue_drain(s_wq);
}
static int wq_error_get(void) {
    return hop_writer_queue_error(s_wq);
}

static void wq_stop(void) {
    if (!s_wq) return;
    if (!hop_writer_queue_stop(s_wq)) {
        hop_log(HOP_LOG_WARN, "xfer: writer worker join failed; retain queue state");
        return;
    }
    hop_writer_queue_destroy(s_wq);
    s_wq = NULL;
}

/* ---------------- 接收 ---------------- */

typedef struct xfer_rx_context {
    hop_xfer_rx_batch_state batch_state;
    hop_xfer_task task;
    hop_xfer_rx_entry_reservations entry_reservations;
    char     dir[4096];
    char    *cur_path;
        hop_xfer_rx_file_state file_state;
    hop_xfer_rx_image_buffer image_buffer;
    hop_xfer_rx_image_state image_state;
    char   **tops;
    int      ntop;
    void *platform_context;
    hop_xfer_rx_lifecycle *lifecycle;
    hop_xfer_rx_done_receipt *done_receipt;
    void (*shutdown_data_channel)(void *context);
    void (*mark_sender_aborted)(void *context, uint32_t batch, uint8_t reason);
    void (*send_file_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*send_done_ack)(void *context, uint32_t batch, const uint8_t *payload, uint32_t length);
    void (*deliver_ack)(void *context, uint32_t batch, uint32_t id, int status);
} xfer_rx_context;

static xfer_rx_context s_rx;

static void rx_context_shutdown_data(void *context) {
    (void)context;
    pthread_mutex_lock(&s_x.slock);
    int active = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (active >= 0) shutdown(active, SHUT_RDWR);
}

static void rx_context_mark_sender_aborted(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    pthread_mutex_lock(&s_x.qlock);
    if (s_x.cur_batch == batch) {
        s_x.abort_batch = batch;
        s_x.abort_reason = reason;
    }
    pthread_mutex_unlock(&s_x.qlock);
    pthread_mutex_lock(&s_ack.lock);
    pthread_cond_broadcast(&s_ack.cond);
    pthread_mutex_unlock(&s_ack.lock);
}

static void rx_context_send_file_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    pthread_mutex_lock(&s_x.slock);
    int active = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (active >= 0) send_ack(active, batch, id, (uint8_t)status);
}

static void rx_context_send_done_ack(void *context, uint32_t batch,
                                    const uint8_t *payload, uint32_t length) {
    (void)context;
    pthread_mutex_lock(&s_x.slock);
    int active = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (active >= 0)
        (void)send_small(active, HOP_MSG_XFER_DONE_ACK, payload, length, batch);
}

static void rx_context_deliver_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    ack_deliver(batch, id, status);
}

static void rx_context_configure(void) {
    s_rx.platform_context = NULL;
    s_rx.lifecycle = &s_rx_lifecycle;
    s_rx.done_receipt = &s_done_receipt;
    s_rx.shutdown_data_channel = rx_context_shutdown_data;
    s_rx.mark_sender_aborted = rx_context_mark_sender_aborted;
    s_rx.send_file_ack = rx_context_send_file_ack;
    s_rx.send_done_ack = rx_context_send_done_ack;
    s_rx.deliver_ack = rx_context_deliver_ack;
}

static int rx_reserve_entry_budget(void *context, uint64_t bytes) {
    return hop_xfer_task_reserve_entry((hop_xfer_task *)context, bytes);
}


/* 关掉当前文件（排空写队列后）。断线时也要调，但不能清批次状态 ——
 * 发送端会带着同一个 batch 号回来重发剩下的条目。 */
static void rx_close_file(void) {
    wq_drain();
    rx_task close_t;
    memset(&close_t, 0, sizeof(close_t));
    close_t.kind = RX_CLOSE;
    wq_push(&close_t);
    wq_drain();
}

static void rx_clear_state(void) {
    free(s_rx.cur_path);
    s_rx.cur_path = NULL;
    hop_xfer_rx_image_buffer_dispose(&s_rx.image_buffer);
    hop_xfer_rx_image_state_reset(&s_rx.image_state);
    for (int i = 0; i < s_rx.ntop; ++i) free(s_rx.tops[i]);
    free(s_rx.tops);
    s_rx.tops = NULL;
    s_rx.ntop = 0;
    hop_xfer_rx_batch_state_reset(&s_rx.batch_state);
    hop_xfer_rx_file_state_reset(&s_rx.file_state);
    hop_xfer_rx_file_state_set_kind(&s_rx.file_state, 0, 0);
    
    hop_xfer_rx_entry_reservations_reset(&s_rx.entry_reservations);
    hop_xfer_task_dispose(&s_rx.task);
    s_rx.dir[0] = '\0';
}

static void rx_reset(void) {
    if(s_rx.batch_state.active)status_end(HOP_XFER_DIR_RECEIVE,s_rx.batch_state.batch,HOP_XFER_FAILED,"Receive interrupted");
    /* 先把队列排空并关掉文件，再清状态 —— 否则写线程可能还在用旧路径 */
    rx_close_file();
    rx_clear_state();
}



/* Receiver-owned lifecycle boundary. Only a data reconnect in the SAME control
 * session may retain ACKs/tombstones; a new control session starts cleanly. */
static hop_xfer_rx_lifecycle s_rx_lifecycle;
/* One FIFO sender batch can be awaiting DONE_ACK per session. This is only
 * wire idempotency metadata, not another task or reservation owner. */
static hop_xfer_rx_done_receipt s_done_receipt;
static void rx_discard_dir(void);
static void rx_terminate(uint32_t batch, uint8_t reason, int notify_peer);
static void rx_bind_session(const hop_session_id *session) {
    if (!hop_session_id_equal(hop_xfer_rx_lifecycle_session(&s_rx_lifecycle), session)) {
        hop_xfer_rx_done_receipt_clear(&s_done_receipt);
        /* A new control session cannot resume the old batch. Its staging files
         * were never published; close the writer before removing them. Completed
         * or deliberately retained partial batches are not active here. */
        if (s_rx.batch_state.active && !hop_xfer_task_begin_completion(&s_rx.task) &&
            hop_xfer_task_cancel_requested(s_rx.task.id))
            /* The current socket may already belong to the new session. Never
             * send an old-session ABORT through it, even if batch IDs collide. */
            rx_terminate(s_rx.batch_state.batch,HOP_ABORT_CANCEL,0);
        if (s_rx.batch_state.active) {
            rx_close_file();
            rx_discard_dir();
            if (s_rx.batch_state.batch & HOP_XFER_PASTE_BATCH_FLAG)
                clipboard_complete_remote_paste(s_rx.batch_state.batch,NULL,0,0);
        }
        rx_reset();
        pthread_mutex_lock(&s_status_lock);
        memset(&s_receive_status,0,sizeof(s_receive_status));
        pthread_mutex_unlock(&s_status_lock);
    } else rx_close_file();
    (void)hop_xfer_rx_lifecycle_bind_session(&s_rx_lifecycle, session);
}

/* The control session may vanish permanently before a new data connection is
 * established. Poll only on the receiver thread; keep a same-session data
 * reconnect resumable, but promptly retire an invalidated control session. */
static void rx_expire_stale_session(void) {
    if (hop_session_id_valid(hop_xfer_rx_lifecycle_session(&s_rx_lifecycle)) && !hop_session_matches(hop_xfer_rx_lifecycle_session(&s_rx_lifecycle))) {
        hop_session_id empty = {{0}};
        rx_bind_session(&empty);
    }
}

/* 递归删掉本批临时目录。
 * 只在接收批次被 ABORT 时调用；普通部分完成仍保留已送达的文件，
 * 成功批次的文件就是产物本身，都不能删。 */
static void rx_discard_dir(void) {
    if (!s_rx.dir[0]) return;
    /* 路径是我们自己拼的，但删除是不可逆操作 —— 仍然验一次根目录 */
    int result = hop_xfer_rx_fs_remove_tree(s_rx.dir, "/HopDesk/");
    if (result == -2)
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %s", s_rx.dir);
    else if (result != 0)
        hop_log(HOP_LOG_WARN, "xfer: failed to remove %s (%s)", s_rx.dir, strerror(errno));
    s_rx.dir[0] = '\0';
}

static int rx_add_top(const char *p) {
    /* 重发会让同一个顶层条目再来一次（比如 ACK 超时但文件其实写成功了），
     * 不去重的话剪贴板里会出现两份同样的路径。ntop 只是顶层条目数，
     * 线性扫一遍的代价可以忽略。 */
    for (int i = 0; i < s_rx.ntop; ++i)
        if (strcmp(s_rx.tops[i], p) == 0) return 1;

    char **nv = (char **)realloc(s_rx.tops, (size_t)(s_rx.ntop + 1) * sizeof(char *));
    if (!nv) return 0;
    s_rx.tops = nv;
    s_rx.tops[s_rx.ntop] = strdup(p);
    if (!s_rx.tops[s_rx.ntop]) return 0;
    s_rx.ntop++;
    return 1;
}
 
/* Remove a previously successful top-level file before retrying it.
 * A retry opens with truncate; if it later fails, stale clipboard paths must
 * not continue advertising the now-incomplete file. */
static void rx_remove_top(const char *p) {
    if (!p) return;
    for (int i = 0; i < s_rx.ntop; ++i) {
        if (strcmp(s_rx.tops[i], p) != 0) continue;
        free(s_rx.tops[i]);
        if (i + 1 < s_rx.ntop)
            memmove(&s_rx.tops[i], &s_rx.tops[i + 1],
                    (size_t)(s_rx.ntop - i - 1) * sizeof(*s_rx.tops));
        s_rx.ntop--;
        return;
    }
}


/* Receiver-thread only. Wire notification has no status side effects. Close the
 * writer before freeing paths/buffers or deleting staging files. Terminal state
 * is idempotent; repeated/stale ABORTs cannot end an unrelated active batch. */
#include "xfer_rx_batch_lifecycle.inc"

#include "xfer_rx_protocol_handlers.inc"

static void on_data_msg(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;

    if (type == HOP_MSG_XFER_HELLO) {
        size_t want = strlen(g_mac.cfg.token);
        int valid = hop_xfer_hello_bound(p, len, g_mac.cfg.token, &s_x.session) && hop_session_matches(&s_x.session);
        if (!valid) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: peer handshake rejected (protocol/token mismatch)");
            pthread_mutex_lock(&s_x.slock);
            if (s_x.sock >= 0) shutdown(s_x.sock, SHUT_RDWR);
            s_x.peer_ready = 0;
            pthread_mutex_unlock(&s_x.slock);
            return;
        }
        pthread_mutex_lock(&s_x.slock);
        s_x.peer_ready = 1;
        pthread_mutex_unlock(&s_x.slock);
        hop_log(HOP_LOG_INFO,
                want ? "xfer: authenticated peer data channel (v%u)" :
                       "xfer: peer data channel ready (v%u)",
                (unsigned)HOP_XFER_PROTO_VER);
        return;
    }

    pthread_mutex_lock(&s_x.slock);
    int ready = s_x.peer_ready;
    int active_sock = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (!ready) {
        hop_log(HOP_LOG_WARN, "xfer: frame received before handshake; closing peer");
        if (active_sock >= 0) shutdown(active_sock, SHUT_RDWR);
        return;
    }

    static hop_xfer_rx_protocol_adapter adapter;
    rx_context_configure();
    hop_xfer_rx_protocol_adapter_init(&adapter, &s_rx_dispatch_handlers, &s_rx);
    (void)hop_xfer_rx_protocol_adapter_dispatch(&adapter, type, p, len);
}

/* ---------------- 连接 ---------------- */

static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host;
    if(!host[0])return -1;
    uint16_t port = g_mac.cfg.data_port ? g_mac.cfg.data_port : HOP_DATA_PORT;

    return net_connect_timeout(host, port, HOP_CONNECT_TIMEOUT_MS);
}

/* Transport gate: typed handlers only see frames bound to this control session.
 * Old batch CHUNK/END are ignored, never applied to the next active file. */
static void on_data_frame_checked(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    if (type == HOP_MSG_XFER_HELLO) { on_data_msg(user, type, p, len); return; }
    pthread_mutex_lock(&s_x.slock);
    hop_session_id session = s_x.session;
    pthread_mutex_unlock(&s_x.slock);
    uint32_t batch, body_len; const uint8_t *body;
    if (!hop_session_matches(&session) ||
        !hop_data_unwrap(type, p, len, &session, &batch, &body, &body_len)) {
        pthread_mutex_lock(&s_x.slock);
        if (s_x.sock != -1) shutdown(s_x.sock, SHUT_RDWR);
        pthread_mutex_unlock(&s_x.slock);
        return;
    }
    if ((type == HOP_MSG_FILE_CHUNK || type == HOP_MSG_FILE_END) &&
        (!s_rx.batch_state.active || batch != s_rx.batch_state.batch)) return;
    on_data_msg(user, type, body, body_len);
}

static void on_data_frame(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    if (!hop_update_receive_enter()) {
        xfer_kick();
        return;
    }
    int guard = hop_watchdog_begin("data receive frame", 60000);
    on_data_frame_checked(user,type,p,len);
    hop_watchdog_end(guard);
    hop_update_work_leave();
}

/* 用户在本端取消接收：通知对端、删掉半成品、立墓碑。
 * 由 conn 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.batch_state.active) return;
    uint32_t batch = s_rx.batch_state.batch;
    rx_terminate(batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
static void rx_poll_cancel(void) {
    rx_expire_stale_session();
    if (__sync_lock_test_and_set(&s_x.cancel_rx, 0) || (s_rx.batch_state.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    pthread_mutex_lock(&s_x.slock); int connected=s_x.peer_ready; pthread_mutex_unlock(&s_x.slock);
    if (s_rx.batch_state.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch_state.batch);
        rx_terminate(s_rx.batch_state.batch,HOP_ABORT_TIMEOUT,0);
    }
}

static int s_data_listener=-1;
static int accept_data(const hop_session_id *expected){
    if(s_data_listener<0){
        s_data_listener=socket(AF_INET,SOCK_STREAM,0);if(s_data_listener<0)return -1;
        int yes=1;setsockopt(s_data_listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(g_mac.cfg.data_port);a.sin_addr.s_addr=htonl(INADDR_ANY);
        if(bind(s_data_listener,(struct sockaddr*)&a,sizeof(a))||listen(s_data_listener,8)){
            hop_log(HOP_LOG_WARN,"xfer: data port %u unavailable (errno=%d)",(unsigned)g_mac.cfg.data_port,errno);
            close(s_data_listener);s_data_listener=-1;return -1;
        }
        hop_log(HOP_LOG_INFO,"xfer: listening on data port %u",(unsigned)g_mac.cfg.data_port);
    }
    while(!s_x.stop && hop_session_matches(expected)){
        rx_poll_cancel();
        fd_set rd;FD_ZERO(&rd);FD_SET(s_data_listener,&rd);struct timeval tv={0,200000};
        int n=select(s_data_listener+1,&rd,NULL,NULL,&tv);
        if(n>0){
            int peer=accept(s_data_listener,NULL,NULL);
            if(peer>=0){int yes=1;if(setsockopt(peer,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))){close(peer);return -1;}}
            return peer;
        }
        if(n<0&&errno!=EINTR){close(s_data_listener);s_data_listener=-1;return -1;}
    }
    return -1;
}
static void *conn_thread(void *arg) {
    (void)arg;
    hop_decoder *dec = hop_decoder_new(NULL, on_data_frame);
    if (!dec) return NULL;

    int backoff = 1;
    while (!s_x.stop) {
        rx_poll_cancel();
        hop_session_id session;
        if (!hop_session_current(&session)) { usleep(200000); continue; }
        int s = g_mac.cfg.role==HOP_ROLE_SERVER?accept_data(&session):do_connect();
        if (s < 0) {
            if (!hop_session_matches(&session)) continue;
            if (g_mac.cfg.role==HOP_ROLE_CLIENT && g_mac.cfg.peer_host[0])
                hop_log(HOP_LOG_WARN,"xfer: TCP data connection failed: host=%s port=%u; check data port and firewall",
                        g_mac.cfg.peer_host,(unsigned)g_mac.cfg.data_port);
            /* 退避要能被 stop 打断 —— 否则退出时要等满 10 秒 */
            for (int i = 0; i < backoff * 10 && !s_x.stop; ++i) {
                rx_poll_cancel();
                usleep(100 * 1000);
            }
            backoff = backoff < 10 ? backoff * 2 : 10;
            continue;
        }
        if (!hop_session_matches(&session)) { close(s); continue; }
        backoff = 1;

        int bufsz = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

        /* 数据通道空闲时间最长：不开 keepalive，链路中间设备丢掉空闲
         * 连接表项后这条 socket 就变黑洞 —— 对端发来的字节永远到不了，
         * 而本端 read 一直 EAGAIN，连"断开"都感知不到。 */
        net_set_keepalive(s);

        /* 发送超时：ABORT 是从接收线程发的，如果发送线程正卡在 write 里
         * （对端窗口满），接收线程拿不到发送锁就会跟着卡死 —— 那是死锁。
         * 给个上限，卡住就当作传输失败，别把整个通道拖死。
         * 256 KB 块在 100 Mbps 链路上只要 20 ms，5 秒是极宽的余量。 */
        struct timeval sndto;
        sndto.tv_sec = 5;
        sndto.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

        /* 接收超时不是错误处理，是给取消请求开一条唤醒通道：
         * 对端暂停发送时 read 会一直阻塞，而 s_rx 只归本线程所有，
         * 取消就只能干等 —— 用户点了"取消"，界面 30 秒没反应。
         * 200 ms 醒一次，代价是空闲时每秒 5 次空转，可以忽略。 */
        struct timeval rxto;
        rxto.tv_sec = 0;
        rxto.tv_usec = 200 * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = s;
        s_x.session = session;
        ++s_x.epoch;
        s_x.peer_ready = 0;
        pthread_mutex_unlock(&s_x.slock);

        pthread_mutex_lock(&s_x.qlock);
        s_x.abort_batch = 0;          /* 新会话，上一轮的 ABORT 不再算数 */
        pthread_mutex_unlock(&s_x.qlock);

        hop_decoder_reset(dec);
        rx_bind_session(&session);

        uint8_t hello[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
        uint32_t hello_len = hop_xfer_hello_encode(hello, g_mac.cfg.token, &session);
        if (!hello_len || !send_small(s, HOP_MSG_XFER_HELLO, hello, hello_len, 0)) {
            shutdown(s, SHUT_RDWR);
        }
        hop_log(HOP_LOG_INFO, "xfer: data channel connected; waiting for peer authentication");

        uint64_t started = hop_monotonic_ms();
        char buf[65536];
        while (!s_x.stop && hop_session_matches(&session)) {
            pthread_mutex_lock(&s_x.slock);
            int authenticated = s_x.peer_ready;
            pthread_mutex_unlock(&s_x.slock);
            if (!authenticated && hop_handshake_expired(hop_monotonic_ms(), started)) break;
            ssize_t r = read(s, buf, sizeof(buf));

            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 不是断线，只是这 200 ms 没数据：借这次醒来处理取消请求 */
                    rx_poll_cancel();
                    continue;
                }
                break;
            }
            if (r == 0) break;                 /* 对端正常关闭 */

            if (!hop_decoder_feed(dec, (const uint8_t *)buf, (uint32_t)r)) break;
            rx_poll_cancel();
        }

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = -1;
        ++s_x.epoch;
        s_x.peer_ready = 0;
        pthread_mutex_unlock(&s_x.slock);
        shutdown(s, SHUT_RDWR);
        pthread_mutex_lock(&s_txlock);
        close(s);
        pthread_mutex_unlock(&s_txlock);
        /* 只关文件，不清批次：发送端会带着同一个 batch 号回来，
         * 从断点条目继续发。这里清掉就变成"从头重传整批"了。 */
        rx_close_file();
        if (!s_x.stop) hop_log(HOP_LOG_WARN, "xfer: data channel closed");
    }

    if(s_data_listener>=0){close(s_data_listener);s_data_listener=-1;}
    hop_decoder_free(dec);
    return NULL;
}

