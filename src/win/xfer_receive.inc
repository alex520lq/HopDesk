/* Included by xfer.c in the same translation unit; not compiled separately. */
/* ---------------- 写盘队列 ----------------
 *
 * 网络线程只做"收包 + 校验 + 入队"，写盘交给独立线程。
 *
 * 队列有界（RX_Q_SLOTS）。满了网络线程就阻塞在 wq_push，
 * recv 随之停止 → TCP 接收窗口收缩 → 发送端自动减速。
 * 这就是显式背压：内存上界 = RX_Q_SLOTS × chunk，不随传输时长增长。
 * 文件句柄由写线程独占，避免"网络线程关文件、写线程还在写"的竞态。
 */
enum { RX_OPEN = 1, RX_CLOSE };

typedef struct {
    int kind;
    wchar_t path[MAX_PATH];
} rx_task;

static hop_writer_queue *s_wq;
static xfer_rx_context s_rx;

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
    xfer_rx_context *rx = (xfer_rx_context *)context;
    hop_xfer_task_snapshot snapshot;
    if (rx && owner && hop_xfer_task_get_snapshot(((hop_xfer_task *)owner)->id, &snapshot) &&
        rx->services.status_stage_time)
        rx->services.status_stage_time(rx->services.context, snapshot.transfer_id,
                                       HOP_XFER_TIME_WRITE, elapsed_ns);
}
static void wq_open_error(void *context, const wchar_t *path, unsigned long error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: cannot create %ls (err %lu)", path, error);
}
static void wq_write_error(void *context, uint64_t offset, unsigned long error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: write failed at %llu (err %lu)",
            (unsigned long long)offset, error);
}

static int wq_start(void) {
    if (s_wq) return 0;
    const hop_writer_queue_callbacks callbacks = {
        HOP_IO_DEADLINE_MS,
        wq_reserve_task_rx, wq_release_task_rx,
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
    hop_writer_queue_drain(s_wq);
}
static int wq_error_get(void) {
    return hop_writer_queue_error(s_wq);
}

static void wq_stop(void) {
    if (!s_wq) return;
    if (!hop_writer_queue_stop(s_wq)) {
        hop_log(HOP_LOG_WARN, "xfer: writer worker did not exit, retain queue state");
        return;
    }
    hop_writer_queue_destroy(s_wq);
    s_wq = NULL;
}

/* ---------------- 接收 ---------------- */


static void rx_context_shutdown_data(void *context) {
    (void)context;
    EnterCriticalSection(&s_x.lock);
    SOCKET active = s_x.client;
    LeaveCriticalSection(&s_x.lock);
    if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
}

static void rx_context_mark_sender_aborted(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    EnterCriticalSection(&s_x.lock);
    if (s_x.cur_batch == batch) {
        s_x.abort_batch = batch;
        s_x.abort_reason = reason;
    }
    LeaveCriticalSection(&s_x.lock);
    SetEvent(s_ack.ev);
}

static void rx_context_send_file_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    EnterCriticalSection(&s_x.lock);
    SOCKET active = s_x.client;
    LeaveCriticalSection(&s_x.lock);
    if (active != INVALID_SOCKET) (void)send_ack(active, batch, id, (uint8_t)status);
}

static void rx_context_send_done_ack(void *context, uint32_t batch,
                                    const uint8_t *payload, uint32_t length) {
    (void)context;
    EnterCriticalSection(&s_x.lock);
    SOCKET active = s_x.client;
    LeaveCriticalSection(&s_x.lock);
    if (active != INVALID_SOCKET)
        (void)send_small(active, HOP_MSG_XFER_DONE_ACK, payload, length, batch);
}

static void rx_context_deliver_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    ack_deliver(batch, id, status);
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
    s_rx.dir[0] = L'\0';
}

static void rx_reset(void) {
    if(s_rx.batch_state.active && s_rx.services.status_end) s_rx.services.status_end(s_rx.services.context,s_rx.batch_state.batch,HOP_XFER_FAILED,"Receive interrupted");
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

static void rx_context_reset_file_error(void *context) {
    (void)context;
    hop_writer_queue_error_reset(s_wq);
}
static int rx_context_open_file(void *context, const void *native_path) {
    (void)context;
    rx_task task;
    memset(&task, 0, sizeof(task));
    task.kind = RX_OPEN;
    wcsncpy(task.path, (const wchar_t *)native_path, MAX_PATH - 1);
    return wq_push(&task);
}
static int rx_context_write_file_chunk(void *context, hop_xfer_task *task,
                                       uint64_t offset, const uint8_t *data, uint32_t length) {
    (void)context;
    return wq_push_data(task, offset, data, length);
}
static void rx_context_close_file(void *context) {
    (void)context;
    rx_close_file();
}
static int rx_context_file_error(void *context) {
    (void)context;
    return wq_error_get();
}

static int rx_add_top(const wchar_t *p);
static void rx_remove_top(const wchar_t *p);

static int rx_context_add_top_path(void *context, const void *native_path) {
    (void)context;
    return rx_add_top((const wchar_t *)native_path);
}
static void rx_context_remove_top_path(void *context, const void *native_path) {
    (void)context;
    rx_remove_top((const wchar_t *)native_path);
}
static void rx_context_cancel_clipboard(void *context, uint32_t batch) {
    (void)context;
    clip_complete_remote_paste(batch, NULL, 0, 0);
}
static void rx_context_finish_clipboard(void *context, uint32_t batch,
        const void *image_data, uint32_t width, uint32_t height, uint32_t stride,
        int image_valid, const void *const *top_paths, int top_count, int allow_publish) {
    (void)context;
    if (allow_publish && image_valid && image_data) {
        uint64_t started = rate_now_ns();
        clip_apply_remote_image(image_data, (int)width, (int)height, (int)stride);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (allow_publish && top_count > 0) {
        uint64_t started = rate_now_ns();
        if (batch & HOP_XFER_PASTE_BATCH_FLAG)
            clip_complete_remote_paste(batch, (const wchar_t *const *)top_paths, top_count, 1);
        else
            clip_apply_remote_paths((const wchar_t *const *)top_paths, top_count);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (batch & HOP_XFER_PASTE_BATCH_FLAG) {
        clip_complete_remote_paste(batch, NULL, 0, 0);
    }
}

static void rx_terminate(uint32_t batch, uint8_t reason, int notify_peer);
static void rx_begin_batch(uint32_t batch);
static int rx_finish_batch(uint32_t batch, uint32_t ok, uint32_t total);

static void rx_context_terminate_batch(void *context, uint32_t batch, uint8_t reason, int notify_peer) {
    (void)context;
    rx_terminate(batch, reason, notify_peer);
}
static void rx_context_begin_batch(void *context, uint32_t batch) {
    (void)context;
    rx_begin_batch(batch);
}
static int rx_context_finish_batch(void *context, uint32_t batch, uint32_t ok, uint32_t total) {
    (void)context;
    return rx_finish_batch(batch, ok, total);
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
    AcquireSRWLockExclusive(&s_status_lock);
    s_receive_status.status.files_total = total;
    ReleaseSRWLockExclusive(&s_status_lock);
}

static void rx_context_status_reset(void *context) {
    (void)context;
    AcquireSRWLockExclusive(&s_status_lock);
    memset(&s_receive_status, 0, sizeof(s_receive_status));
    ReleaseSRWLockExclusive(&s_status_lock);
}

static void rx_context_configure(void) {
    s_rx.writer = s_wq;
    s_rx.lifecycle = &s_rx_lifecycle;
    s_rx.done_receipt = &s_done_receipt;
    s_rx.services.context = NULL;
    s_rx.services.shutdown_data_channel = rx_context_shutdown_data;
    s_rx.services.mark_sender_aborted = rx_context_mark_sender_aborted;
    s_rx.services.send_file_ack = rx_context_send_file_ack;
    s_rx.services.send_done_ack = rx_context_send_done_ack;
    s_rx.services.deliver_ack = rx_context_deliver_ack;
    s_rx.services.terminate_batch = rx_context_terminate_batch;
    s_rx.services.begin_batch = rx_context_begin_batch;
    s_rx.services.finish_batch = rx_context_finish_batch;
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
static void rx_discard_dir(void);
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
                if (s_rx.services.cancel_clipboard) s_rx.services.cancel_clipboard(s_rx.services.context, s_rx.batch_state.batch);
        }
        rx_reset();
        if (s_rx.services.status_reset) s_rx.services.status_reset(s_rx.services.context);
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
    /* 路径是我们自己拼的，但删除是不可逆操作 —— 仍然验一次根目录，
     * 免得哪天拼错了把用户目录端掉。 */
    int result = hop_xfer_rx_fs_remove_tree(s_rx.dir, XFER_RX_ROOT);
    if (result == -2)
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %ls", s_rx.dir);
    else if (result != 0)
        hop_log(HOP_LOG_WARN, "xfer: failed to remove %ls", s_rx.dir);
    s_rx.dir[0] = L'\0';
}

/* Create a directory, accepting ERROR_ALREADY_EXISTS only when the path
 * is actually a directory (a regular file at a parent path is an error). */


/* Create parent components without attempting to create a drive/UNC root. */


static int rx_add_top(const wchar_t *p) {
    /* 重发会让同一个顶层条目再来一次（比如 ACK 超时但文件其实写成功了），
     * 不去重的话剪贴板里会出现两份同样的路径。ntop 只是顶层条目数，
     * 线性扫一遍的代价可以忽略。 */
    for (int i = 0; i < s_rx.ntop; ++i)
        if (wcscmp(s_rx.tops[i], p) == 0) return 1;

    wchar_t **nv = (wchar_t **)realloc(s_rx.tops, (size_t)(s_rx.ntop + 1) * sizeof(wchar_t *));
    if (!nv) return 0;
    s_rx.tops = nv;
    s_rx.tops[s_rx.ntop] = _wcsdup(p);
    if (!s_rx.tops[s_rx.ntop]) return 0;
    s_rx.ntop++;
    return 1;
}

/* Remove a previously successful top-level file before retrying it.
 * A retry opens with CREATE_ALWAYS; if it later fails, stale clipboard paths
 * must not continue advertising the now-incomplete file. */
static void rx_remove_top(const wchar_t *p) {
    if (!p) return;
    for (int i = 0; i < s_rx.ntop; ++i) {
        if (wcscmp(s_rx.tops[i], p) != 0) continue;
        free(s_rx.tops[i]);
        if (i + 1 < s_rx.ntop)
            memmove(&s_rx.tops[i], &s_rx.tops[i + 1],
                    (size_t)(s_rx.ntop - i - 1) * sizeof(*s_rx.tops));
        s_rx.ntop--;
        return;
    }
}


/* 把 UTF-8 相对路径拼到本批目录下，返回 malloc 的宽路径 */


/* Receiver-thread only. Wire notification has no status side effects. Close the
 * writer before freeing paths/buffers or deleting staging files. Terminal state
 * is idempotent; repeated/stale ABORTs cannot end an unrelated active batch. */
#include "xfer_rx_batch_lifecycle.inc"



static void on_data_msg(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;

    if (type == HOP_MSG_XFER_HELLO) {
        size_t want = strlen(s_x.cfg.token);
        int valid = hop_xfer_hello_bound(p, len, s_x.cfg.token, &s_x.session) && hop_session_matches(&s_x.session);
        if (!valid) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: peer handshake rejected (protocol/token mismatch)");
            EnterCriticalSection(&s_x.lock);
            SOCKET active = s_x.client;
            s_x.peer_ready = 0;
            LeaveCriticalSection(&s_x.lock);
            if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
            return;
        }
        EnterCriticalSection(&s_x.lock);
        s_x.peer_ready = 1;
        LeaveCriticalSection(&s_x.lock);
        hop_log(HOP_LOG_INFO,
                want ? "xfer: authenticated peer data channel (v%u)" :
                       "xfer: peer data channel ready (v%u)",
                (unsigned)HOP_XFER_PROTO_VER);
        return;
    }

    EnterCriticalSection(&s_x.lock);
    int ready = s_x.peer_ready;
    SOCKET active = s_x.client;
    LeaveCriticalSection(&s_x.lock);
    if (!ready) {
        hop_log(HOP_LOG_WARN, "xfer: frame received before handshake; closing peer");
        if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
        return;
    }

    static hop_xfer_rx_protocol_adapter adapter;
    rx_context_configure();
    hop_xfer_rx_protocol_adapter_init(&adapter, &hop_xfer_rx_platform_dispatch_handlers, &s_rx);
    (void)hop_xfer_rx_protocol_adapter_dispatch(&adapter, type, p, len);
}

/* Transport gate: typed handlers only see frames bound to this control session.
 * Old batch CHUNK/END are ignored, never applied to the next active file. */
static void on_data_frame_checked(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    if (type == HOP_MSG_XFER_HELLO) { on_data_msg(user, type, p, len); return; }
    EnterCriticalSection(&s_x.lock);
    hop_session_id session = s_x.session;
    LeaveCriticalSection(&s_x.lock);
    uint32_t batch, body_len; const uint8_t *body;
    if (!hop_session_matches(&session) ||
        !hop_data_unwrap(type, p, len, &session, &batch, &body, &body_len)) {
        EnterCriticalSection(&s_x.lock);
        if (s_x.client != INVALID_SOCKET) shutdown(s_x.client, SD_BOTH);
        LeaveCriticalSection(&s_x.lock);
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
 * 由 listen 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.batch_state.active) return;
    uint32_t batch = s_rx.batch_state.batch;
    rx_terminate(batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
static void rx_poll_cancel(void) {
    rx_expire_stale_session();
    if (InterlockedExchange(&s_x.cancel_rx, 0) || (s_rx.batch_state.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    EnterCriticalSection(&s_x.lock); int connected=s_x.peer_ready; LeaveCriticalSection(&s_x.lock);
    if (s_rx.batch_state.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch_state.batch);
        rx_terminate(s_rx.batch_state.batch,HOP_ABORT_TIMEOUT,0);
    }
}

/* A pending handshake is incremental, separate from the active stream. A slow
 * unauthenticated client cannot stop the current peer from receiving data. */
static SOCKET data_listener(void) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return sock;
    BOOL yes = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(s_x.cfg.data_port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 8)) {
        hop_log(HOP_LOG_WARN, "xfer: port %u unavailable (%d), retrying", s_x.cfg.data_port, WSAGetLastError());
        closesocket(sock); return INVALID_SOCKET;
    }
    return sock;
}
static void retire_data_socket(SOCKET sock) {
    if (sock == INVALID_SOCKET) return;
    EnterCriticalSection(&s_x.lock);
    if (s_x.client == sock) {
        s_x.client = INVALID_SOCKET; s_x.peer_ready = 0; ++s_x.epoch;
    }
    shutdown(sock, SD_BOTH);
    LeaveCriticalSection(&s_x.lock);
    EnterCriticalSection(&s_txlock);
    closesocket(sock);
    LeaveCriticalSection(&s_txlock);
}
static DWORD WINAPI listen_thread(LPVOID arg) {
    (void)arg;
    hop_decoder *dec = hop_decoder_new(NULL, on_data_frame);
    if (!dec) return 1;
    SOCKET active = INVALID_SOCKET, pending = INVALID_SOCKET;
    uint8_t hello[HOP_HEADER_SIZE + 2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    uint32_t have = 0, need = HOP_HEADER_SIZE;
    uint64_t started = 0, retry_at = 0;
    while (WaitForSingleObject(s_x.stop, 0) != WAIT_OBJECT_0) {
        uint64_t now = hop_monotonic_ms();
        if (active != INVALID_SOCKET && !hop_session_matches(&s_x.session)) {
            retire_data_socket(active); active = INVALID_SOCKET; rx_close_file();
        }
        if (s_x.listen_s == INVALID_SOCKET && now >= retry_at) {
            s_x.listen_s = data_listener(); retry_at = now + 2000;
            InterlockedExchange(&s_x.listening, s_x.listen_s != INVALID_SOCKET);
        }
        if (pending != INVALID_SOCKET && hop_handshake_expired(now, started)) {
            closesocket(pending); pending = INVALID_SOCKET;
        }
        rx_poll_cancel();
        fd_set rd; FD_ZERO(&rd);
        if (s_x.listen_s != INVALID_SOCKET) FD_SET(s_x.listen_s, &rd);
        if (active != INVALID_SOCKET) FD_SET(active, &rd);
        if (pending != INVALID_SOCKET) FD_SET(pending, &rd);
        if (!rd.fd_count) { WaitForSingleObject(s_x.stop, 200); continue; }
        struct timeval tv = {0, 200000};
        int ready = select(0, &rd, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) {
            retire_data_socket(active); active = INVALID_SOCKET;
            if (pending != INVALID_SOCKET) closesocket(pending);
            pending = INVALID_SOCKET;
            if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);
            s_x.listen_s = INVALID_SOCKET; InterlockedExchange(&s_x.listening, 0);
            continue;
        }
        if (!ready) continue;
        /* Process active input first, then reset its decoder on replacement. */
        if (active != INVALID_SOCKET && FD_ISSET(active, &rd)) {
            uint8_t buf[65536];
            int n = recv(active, (char *)buf, sizeof(buf), 0);
            if (n <= 0 || !hop_decoder_feed(dec, buf, (uint32_t)n)) {
                retire_data_socket(active); active = INVALID_SOCKET; rx_close_file();
                hop_log(HOP_LOG_WARN, "xfer: data channel closed");
            }
        }
        if (pending != INVALID_SOCKET && FD_ISSET(pending, &rd)) {
            int n = recv(pending, (char *)hello + have, (int)(need - have), 0);
            int bad = n <= 0;
            if (!bad) have += (uint32_t)n;
            if (!bad && have == HOP_HEADER_SIZE && need == HOP_HEADER_SIZE) {
                uint32_t len = hop_get_be32(hello + 1);
                if (hello[0] != HOP_MSG_XFER_HELLO || len < 2 + HOP_SESSION_BYTES || len > 2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES) bad = 1;
                else need += len;
            }
            if (!bad && have == need) {
                hop_session_id session;
                bad = !hop_session_current(&session) ||
                      !hop_xfer_hello_bound(hello + HOP_HEADER_SIZE, need - HOP_HEADER_SIZE, s_x.cfg.token, &session);
                if (bad) hop_log(HOP_LOG_WARN,"xfer: data handshake rejected: protocol, token or control session mismatch");
                if (!bad) {
                    uint8_t reply[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
                    uint32_t reply_len = hop_xfer_hello_encode(reply, s_x.cfg.token, &session);
                    uint8_t header[HOP_HEADER_SIZE];
                    hop_encode_header(header, HOP_MSG_XFER_HELLO, reply_len);
                    bad = !send_all(pending, (const char *)header, sizeof(header)) ||
                          !send_all(pending, (const char *)reply, (int)reply_len);
                }
                if (!bad) {
                    hop_log(HOP_LOG_INFO, active == INVALID_SOCKET ? "xfer: authenticated data channel" :
                            "xfer: replacing stale data connection");
                    retire_data_socket(active);
                    rx_bind_session(&session); hop_decoder_reset(dec);
                    active = pending; pending = INVALID_SOCKET;
                    EnterCriticalSection(&s_x.lock);
                    s_x.session = session;
                    s_x.client = active; s_x.peer_ready = 1; ++s_x.epoch; s_x.abort_batch = 0;
                    LeaveCriticalSection(&s_x.lock);
                }
            }
            if (bad) { closesocket(pending); pending = INVALID_SOCKET; }
        }
        if (s_x.listen_s != INVALID_SOCKET && FD_ISSET(s_x.listen_s, &rd)) {
            SOCKET sock = accept(s_x.listen_s, NULL, NULL);
            if (sock != INVALID_SOCKET) {
                /* Only authenticated active peers are protected from replacement. */
                if (pending != INVALID_SOCKET) closesocket(pending);
                {
                    pending = sock; have = 0; need = HOP_HEADER_SIZE; started = hop_monotonic_ms();
                    DWORD snd = 3000, rcv = 200;
                    int bufsz = 4 * 1024 * 1024;
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd, sizeof(snd));
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv, sizeof(rcv));
                    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof(bufsz));
                    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof(bufsz));
                    net_set_keepalive(sock);
                }
            }
        }
    }
    if (pending != INVALID_SOCKET) closesocket(pending);
    retire_data_socket(active);
    if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);
    s_x.listen_s = INVALID_SOCKET; InterlockedExchange(&s_x.listening, 0);
    hop_decoder_free(dec);
    return 0;
}

static DWORD WINAPI data_client_thread(LPVOID unused){
    (void)unused;unsigned delay=500;
    hop_decoder *dec=hop_decoder_new(NULL,on_data_frame);if(!dec)return 1;
    while(WaitForSingleObject(s_x.stop,0)!=WAIT_OBJECT_0){
        rx_poll_cancel();
        hop_session_id session;
        if(!hop_session_current(&session)){WaitForSingleObject(s_x.stop,200);continue;}
        SOCKET sock=net_connect_timeout_win(s_x.cfg.peer_host,s_x.cfg.data_port,s_x.stop);
        if(sock==INVALID_SOCKET){
            if(s_x.cfg.peer_host[0])hop_log(HOP_LOG_WARN,"xfer: TCP data connection failed: host=%s port=%u; check data port and firewall",s_x.cfg.peer_host,(unsigned)s_x.cfg.data_port);
            for(unsigned waited=0;waited<delay;waited+=200){
                rx_poll_cancel();
                if(WaitForSingleObject(s_x.stop,200)==WAIT_OBJECT_0)break;
            }
            delay=delay<8000?delay*2:8000;continue;
        }
        DWORD snd=5000,rcv=200;net_set_keepalive(sock);
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(char*)&snd,sizeof(snd));setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(char*)&rcv,sizeof(rcv));
        rx_bind_session(&session);hop_decoder_reset(dec);
        EnterCriticalSection(&s_x.lock);s_x.session=session;s_x.client=sock;s_x.peer_ready=0;++s_x.epoch;s_x.abort_batch=0;LeaveCriticalSection(&s_x.lock);
        uint8_t hello[2+HOP_TOKEN_MAX+HOP_SESSION_BYTES],header[HOP_HEADER_SIZE];
        uint32_t len=hop_xfer_hello_encode(hello,s_x.cfg.token,&session);hop_encode_header(header,HOP_MSG_XFER_HELLO,len);
        if(!send_all(sock,(const char*)header,sizeof(header)) || !send_all(sock,(const char*)hello,(int)len))shutdown(sock,SD_BOTH);
        uint64_t started=hop_monotonic_ms();int authed=0;
        while(WaitForSingleObject(s_x.stop,0)!=WAIT_OBJECT_0 && hop_session_matches(&session)){
            EnterCriticalSection(&s_x.lock);authed=s_x.peer_ready;LeaveCriticalSection(&s_x.lock);
            if(!authed && hop_handshake_expired(hop_monotonic_ms(),started))break;
            rx_poll_cancel();
            uint8_t buf[65536];int n=recv(sock,(char*)buf,sizeof(buf),0);
            if(n==SOCKET_ERROR){int e=WSAGetLastError();if(e==WSAETIMEDOUT||e==WSAEWOULDBLOCK||e==WSAEINTR)continue;break;}
            if(n<=0||!hop_decoder_feed(dec,buf,(uint32_t)n))break;
        }
        retire_data_socket(sock);rx_close_file();
        delay=authed?500:(delay<8000?delay*2:8000);
        for(unsigned waited=0;waited<delay;waited+=200){
            rx_poll_cancel();
            if(WaitForSingleObject(s_x.stop,200)==WAIT_OBJECT_0)break;
        }
    }
    hop_decoder_free(dec);return 0;
}
