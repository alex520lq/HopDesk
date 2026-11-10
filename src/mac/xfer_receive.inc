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
static xfer_rx_context s_rx;
static int rx_discard_dir(xfer_rx_context *rx);

static int wq_reserve_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    return hop_xfer_task_reserve_rx_buffer((hop_xfer_task *)owner, bytes);
}
static void wq_release_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    (void)hop_xfer_task_release_rx_buffer((hop_xfer_task *)owner, bytes);
}
static int wq_task_cancelled(void *context, void *owner) {
    (void)context;
    return owner && hop_xfer_task_cancel_requested(((hop_xfer_task *)owner)->id);
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
    xfer_rx_context *rx = (xfer_rx_context *)context;
    hop_xfer_task_snapshot snapshot;
    if (rx && owner && hop_xfer_task_get_snapshot(((hop_xfer_task *)owner)->id, &snapshot) &&
        rx->services.status_stage_time)
        rx->services.status_stage_time(rx->services.context, snapshot.transfer_id,
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
        wq_reserve_task_rx, wq_release_task_rx, wq_task_cancelled,
        wq_watchdog_begin, wq_watchdog_end, wq_now_ns,
        wq_write_time, wq_open_error, wq_write_error
    };
    s_wq = hop_writer_queue_create(&s_rx, &callbacks);
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
    if (s_wq) hop_writer_queue_drain(s_wq);
}
static int wq_error_get(void) {
    return s_wq ? hop_writer_queue_error(s_wq) : 0;
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


static void rx_context_shutdown_data(void *context) {
    (void)context;
    pthread_mutex_lock(&s_x.slock);
    int active = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (active >= 0) shutdown(active, SHUT_RDWR);
}

static int rx_context_discard_staging_directory(void *context) {
    return rx_discard_dir((xfer_rx_context *)context);
}

static void rx_context_clear_cancel_request(void *context) {
    (void)context;
    __sync_lock_test_and_set(&s_x.cancel_rx, 0);
}

static int rx_context_create_staging_directory(void *context, const char *session_hex, uint32_t batch) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !session_hex) return 0;
    /* Never overwrite the only record of a staging tree that failed cleanup. */
    if (rx->dir[0] && !rx_discard_dir(rx)) return 0;
    int length = snprintf(rx->dir, sizeof(rx->dir), "%s/HopDesk/%s/%u",
                          temp_root(), session_hex, batch);
    if (length <= 0 || (size_t)length >= sizeof(rx->dir)) {
        rx->dir[0] = '\0';
        return 0;
    }
    if (hop_xfer_rx_fs_mkdir_p(rx->dir) != 0) {
        rx->staging_cleanup_pending = 1;
        (void)rx_discard_dir(rx);
        return 0;
    }
    return 1;
}

static void rx_context_send_abort(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    send_abort(batch, reason);
}

static void rx_context_mark_sender_aborted(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    pthread_mutex_lock(&s_x.qlock);
    if (s_x.cur_batch == batch) {
        s_x.abort_batch = batch;
        s_x.abort_reason = reason;
    }
    pthread_mutex_unlock(&s_x.qlock);
    sender_context_wake_ack();
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



/* 关掉当前文件（排空写队列后）。断线时也要调，但不能清批次状态 ——
 * 发送端会带着同一个 batch 号回来重发剩下的条目。 */
static void rx_close_file(xfer_rx_context *rx) {
    if (rx && rx->services.close_file)
        rx->services.close_file(rx->services.context);
}

static void rx_reset(xfer_rx_context *rx) {
    if (!rx) return;
    if (rx->batch_state.active)
        hop_xfer_rx_terminate_batch(rx, rx->batch_state.batch, HOP_ABORT_DISK, 0);
    else {
        rx_close_file(rx);
        if (rx->staging_cleanup_pending) (void)rx_discard_dir(rx);
        hop_xfer_rx_reset_batch_state(rx);
    }
    /* One final retry during orderly shutdown; retain the path if it still fails. */
    if (rx->staging_cleanup_pending) (void)rx_discard_dir(rx);
}



/* Receiver-owned lifecycle boundary. Only a data reconnect in the SAME control
 * session may retain ACKs/tombstones; a new control session starts cleanly. */
static hop_xfer_rx_lifecycle s_rx_lifecycle;
/* One FIFO sender batch can be awaiting DONE_ACK per session. This is only
 * wire idempotency metadata, not another task or reservation owner. */
static hop_xfer_rx_done_receipt s_done_receipt;

static void *rx_context_build_path(void *context, const char *relative, uint32_t length) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !relative || !length || length > HOP_PATH_LEN_MAX) return NULL;
    char *terminated = (char *)malloc((size_t)length + 1u);
    if (!terminated) return NULL;
    memcpy(terminated, relative, length);
    terminated[length] = '\0';
    char *path = hop_xfer_rx_fs_path_for(rx->dir, terminated);
    free(terminated);
    return path;
}

static int rx_context_prepare_file_path(void *context, const void *native_path, int is_directory) {
    (void)context;
    if (!native_path) return 0;
    char *copy = strdup((const char *)native_path);
    if (!copy) return 0;
    int result;
    if (is_directory) {
        result = hop_xfer_rx_fs_mkdir_p(copy) == 0;
    } else {
        char *separator = strrchr(copy, '/');
        if (!separator) { free(copy); return 0; }
        *separator = '\0';
        result = hop_xfer_rx_fs_mkdir_p(copy) == 0;
    }
    free(copy);
    return result;
}

static int rx_context_is_top_level_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    const char *path = (const char *)native_path;
    if (!rx || !path) return 0;
    size_t root_length = strlen(rx->dir);
    if (strncmp(path, rx->dir, root_length) != 0 || path[root_length] != '/') return 0;
    const char *tail = path + root_length + 1u;
    return *tail && strchr(tail, '/') == NULL;
}

static int rx_context_remove_file(void *context, const void *native_path) {
    (void)context;
    if (!native_path) return 0;
    if (hop_xfer_rx_fs_remove_file((const char *)native_path) == 0) return 1;
    return errno == ENOENT;
}

static void rx_context_reset_file_error(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer) return;
    hop_writer_queue_error_reset((hop_writer_queue *)rx->writer);
}
static int rx_context_open_file(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer || !native_path) return 0;
    return hop_writer_queue_enqueue_open((hop_writer_queue *)rx->writer,
                                         (const char *)native_path);
}
static int rx_context_write_file_chunk(void *context, hop_xfer_task *task,
                                       uint64_t offset, const uint8_t *data, uint32_t length) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer || task != &rx->task) return 0;
    return hop_writer_queue_enqueue_write((hop_writer_queue *)rx->writer,
                                          task, offset, data, length);
}
static void rx_context_close_file(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer) return;
    hop_writer_queue *writer = (hop_writer_queue *)rx->writer;
    hop_writer_queue_drain(writer);
    (void)hop_writer_queue_enqueue_close(writer);
    hop_writer_queue_drain(writer);
}
static int rx_context_file_error(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    return rx && rx->writer ? hop_writer_queue_error((const hop_writer_queue *)rx->writer) : 1;
}

static int rx_add_top(xfer_rx_context *rx, const char *p);
static void rx_remove_top(xfer_rx_context *rx, const char *p);

static int rx_context_add_top_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    return rx && native_path ? rx_add_top(rx, (const char *)native_path) : 0;
}
static void rx_context_remove_top_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && native_path) rx_remove_top(rx, (const char *)native_path);
}
static void rx_context_cancel_clipboard(void *context, uint32_t batch) {
    (void)context;
    clipboard_complete_remote_paste(batch, NULL, 0, 0);
}
static void rx_context_finish_clipboard(void *context, uint32_t batch,
        const void *image_data, uint32_t width, uint32_t height, uint32_t stride,
        int image_valid, const void *const *top_paths, int top_count, int allow_publish) {
    (void)context;
    if (allow_publish && image_valid && image_data) {
        uint64_t started = rate_now_ns();
        clipboard_apply_image(image_data, (int)width, (int)height, (int)stride);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (allow_publish && top_count > 0) {
        uint64_t started = rate_now_ns();
        if (batch & HOP_XFER_PASTE_BATCH_FLAG)
            clipboard_complete_remote_paste(batch, (const char *const *)top_paths, top_count, 1);
        else
            clipboard_apply_paths((const char *const *)top_paths, top_count);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (batch & HOP_XFER_PASTE_BATCH_FLAG) {
        clipboard_complete_remote_paste(batch, NULL, 0, 0);
    }
}


static uint64_t rx_context_now_ns(void *context) {
    (void)context;
    return rate_now_ns();
}
static int rx_context_status_begin(void *context, uint32_t batch, uint32_t files,
        uint64_t bytes, const char *name, int phase) {
    (void)context;
    return status_begin(batch, HOP_XFER_DIR_RECEIVE, files, bytes, name, phase);
}
static void rx_context_status_end(void *context, uint32_t batch, int phase, const char *detail) {
    (void)context;
    status_end(HOP_XFER_DIR_RECEIVE, batch, phase, detail);
}
static void rx_context_status_add_file(void *context, uint32_t batch, uint32_t id,
        const char *path, uint64_t size, int directory) {
    (void)context;
    status_add_received_file(batch, id, path, size, directory);
}
static void rx_context_status_image(void *context, uint32_t batch, uint64_t size) {
    (void)context;
    status_image(batch, HOP_XFER_DIR_RECEIVE, size);
}
static void rx_context_status_stage(void *context, uint32_t batch, int stage, uint64_t elapsed_ns) {
    (void)context;
    status_stage_time(HOP_XFER_DIR_RECEIVE, batch, stage, elapsed_ns);
}
static void rx_context_status_chunk(void *context, uint32_t batch, uint32_t id,
        uint64_t offset, uint32_t length, uint64_t crc_ns, uint64_t receive_ns) {
    (void)context;
    status_chunk_timed(HOP_XFER_DIR_RECEIVE, batch, id, offset, length, 0, crc_ns, 0, receive_ns);
}
static void rx_context_status_file_phase(void *context, uint32_t batch, int index,
        int phase, uint32_t attempts) {
    (void)context;
    status_file_phase(HOP_XFER_DIR_RECEIVE, batch, index, phase, attempts);
}
static void rx_context_status_set_files_total(void *context, uint32_t total) {
    (void)context;
    pthread_mutex_lock(&s_status_lock);
    s_receive_status.status.files_total = total;
    pthread_mutex_unlock(&s_status_lock);
}

static void rx_context_status_reset(void *context) {
    (void)context;
    pthread_mutex_lock(&s_status_lock);
    memset(&s_receive_status, 0, sizeof(s_receive_status));
    pthread_mutex_unlock(&s_status_lock);
}

static void rx_context_configure(void) {
    s_rx.lifecycle = &s_rx_lifecycle;
    s_rx.done_receipt = &s_done_receipt;
    s_rx.writer = s_wq;
    s_rx.services.context = &s_rx;
    s_rx.services.shutdown_data_channel = rx_context_shutdown_data;
    s_rx.services.mark_sender_aborted = rx_context_mark_sender_aborted;
    s_rx.services.send_abort = rx_context_send_abort;
    s_rx.services.clear_cancel_request = rx_context_clear_cancel_request;
    s_rx.services.create_staging_directory = rx_context_create_staging_directory;
    s_rx.services.discard_staging_directory = rx_context_discard_staging_directory;
    s_rx.services.send_file_ack = rx_context_send_file_ack;
    s_rx.services.send_done_ack = rx_context_send_done_ack;
    s_rx.services.deliver_ack = rx_context_deliver_ack;
    s_rx.services.build_path = rx_context_build_path;
    s_rx.services.prepare_file_path = rx_context_prepare_file_path;
    s_rx.services.is_top_level_path = rx_context_is_top_level_path;
    s_rx.services.remove_file = rx_context_remove_file;
    s_rx.services.reset_file_error = rx_context_reset_file_error;
    s_rx.services.open_file = rx_context_open_file;
    s_rx.services.write_file_chunk = rx_context_write_file_chunk;
    s_rx.services.close_file = rx_context_close_file;
    s_rx.services.file_error = rx_context_file_error;
    s_rx.services.add_top_path = rx_context_add_top_path;
    s_rx.services.remove_top_path = rx_context_remove_top_path;
    s_rx.services.finish_clipboard = rx_context_finish_clipboard;
    s_rx.services.cancel_clipboard = rx_context_cancel_clipboard;
    s_rx.services.now_ns = rx_context_now_ns;
    s_rx.services.status_begin = rx_context_status_begin;
    s_rx.services.status_end = rx_context_status_end;
    s_rx.services.status_add_received_file = rx_context_status_add_file;
    s_rx.services.status_image = rx_context_status_image;
    s_rx.services.status_stage_time = rx_context_status_stage;
    s_rx.services.status_chunk_received = rx_context_status_chunk;
    s_rx.services.status_file_phase = rx_context_status_file_phase;
    s_rx.services.status_set_files_total = rx_context_status_set_files_total;
    s_rx.services.status_reset = rx_context_status_reset;
}
static int rx_discard_dir(xfer_rx_context *rx);
static void rx_bind_session(const hop_session_id *session) {
    hop_xfer_rx_bind_session(&s_rx, session);
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
static int rx_discard_dir(xfer_rx_context *rx) {
    if (!rx) return 0;
    if (!rx->dir[0]) { rx->staging_cleanup_pending = 0; return 1; }
    int result = hop_xfer_rx_fs_remove_tree(rx->dir, "/HopDesk/");
    if (result == 0) {
        rx->dir[0] = '\0';
        rx->staging_cleanup_pending = 0;
        return 1;
    }
    if (result == -2)
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete receiver staging directory");
    else
        hop_log(HOP_LOG_WARN, "xfer: failed to remove receiver staging directory (result %d); retaining path for retry", result);
    rx->staging_cleanup_pending = 1;
    return 0;
}

static int rx_add_top(xfer_rx_context *rx, const char *p) {
    if (!rx || !p) return 0;
    for (int i = 0; i < rx->ntop; ++i)
        if (strcmp(rx->tops[i], p) == 0) return 1;
    char **nv = (char **)realloc(rx->tops, (size_t)(rx->ntop + 1) * sizeof(char *));
    if (!nv) return 0;
    rx->tops = nv;
    rx->tops[rx->ntop] = strdup(p);
    if (!rx->tops[rx->ntop]) return 0;
    rx->ntop++;
    return 1;
}
 
/* Remove a previously successful top-level file before retrying it.
 * A retry opens with truncate; if it later fails, stale clipboard paths must
 * not continue advertising the now-incomplete file. */
static void rx_remove_top(xfer_rx_context *rx, const char *p) {
    if (!rx || !p) return;
    for (int i = 0; i < rx->ntop; ++i) {
        if (strcmp(rx->tops[i], p) != 0) continue;
        free(rx->tops[i]);
        if (i + 1 < rx->ntop)
            memmove(&rx->tops[i], &rx->tops[i + 1],
                    (size_t)(rx->ntop - i - 1) * sizeof(*rx->tops));
        rx->ntop--;
        return;
    }
}


/* Receiver-thread only. Wire notification has no status side effects. Close the
 * writer before freeing paths/buffers or deleting staging files. Terminal state
 * is idempotent; repeated/stale ABORTs cannot end an unrelated active batch. */



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
    hop_xfer_rx_protocol_adapter_init(&adapter, &hop_xfer_rx_platform_dispatch_handlers, &s_rx);
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
    hop_xfer_rx_terminate_batch(&s_rx,batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
static void rx_poll_cancel(void) {
    rx_expire_stale_session();
    if (__sync_lock_test_and_set(&s_x.cancel_rx, 0) || (s_rx.batch_state.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    pthread_mutex_lock(&s_x.slock); int connected=s_x.peer_ready; pthread_mutex_unlock(&s_x.slock);
    if (s_rx.batch_state.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch_state.batch);
        hop_xfer_rx_terminate_batch(&s_rx,s_rx.batch_state.batch,HOP_ABORT_TIMEOUT,0);
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
        rx_close_file(&s_rx);
        if (!s_x.stop) hop_log(HOP_LOG_WARN, "xfer: data channel closed");
    }

    if(s_data_listener>=0){close(s_data_listener);s_data_listener=-1;}
    hop_decoder_free(dec);
    return NULL;
}

