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

static struct {
    uint32_t batch;
    int      active;
    hop_xfer_task task;
    uint8_t reserved_entries[HOP_XFER_FILE_LIMIT];
    uint64_t reserved_entry_sizes[HOP_XFER_FILE_LIMIT];
    char     dir[4096];
    char    *cur_path;
    uint32_t cur_id;
    int      cur_is_image;
    int      cur_is_dir;    /* 目录在 BEGIN 时已登记，FILE_END 不能再登记一次 */
    int      cur_fail;      /* 本条目已判死：-1 = 正常，否则是 FILE_END 要回的 ACK 状态。
                             * 坏路径和坏数据都在这里落定，FILE_END 只负责回执。 */
    uint64_t cur_size;      /* 声明的大小：超出就拒收，防止被灌成无限大 */
    uint64_t cur_received; /* 已按连续偏移成功入队的文件字节数 */
    /* 当前文件的校验累加：每块 CRC 折进来，FILE_END 时与发送端比对 */
    uint32_t fcrc;
    uint8_t *img;
    size_t   img_cap, img_len, img_expected;
    int      img_w, img_h, img_stride;
    char   **tops;
    int      ntop;
} s_rx;

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
    free(s_rx.img);
    s_rx.img = NULL;
    s_rx.img_cap = s_rx.img_len = s_rx.img_expected = 0;
    for (int i = 0; i < s_rx.ntop; ++i) free(s_rx.tops[i]);
    free(s_rx.tops);
    s_rx.tops = NULL;
    s_rx.ntop = 0;
    s_rx.active = 0;
    s_rx.batch = 0;
    s_rx.fcrc = 0;
    s_rx.cur_id = 0;
    s_rx.cur_size = 0;
    s_rx.cur_received = 0;
    s_rx.cur_is_image = s_rx.cur_is_dir = 0;
    s_rx.img_w = s_rx.img_h = s_rx.img_stride = 0;
    s_rx.cur_fail = -1;
    memset(s_rx.reserved_entries,0,sizeof(s_rx.reserved_entries));
    memset(s_rx.reserved_entry_sizes,0,sizeof(s_rx.reserved_entry_sizes));
    hop_xfer_task_dispose(&s_rx.task);
    s_rx.dir[0] = '\0';
}

static void rx_reset(void) {
    if(s_rx.active)status_end(HOP_XFER_DIR_RECEIVE,s_rx.batch,HOP_XFER_FAILED,"Receive interrupted");
    /* 先把队列排空并关掉文件，再清状态 —— 否则写线程可能还在用旧路径 */
    rx_close_file();
    rx_clear_state();
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)ftw;
    if (flag == FTW_DP) rmdir(path);
    else                unlink(path);
    return 0;
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
        if (s_rx.active && !hop_xfer_task_begin_completion(&s_rx.task) &&
            hop_xfer_task_cancel_requested(s_rx.task.id))
            /* The current socket may already belong to the new session. Never
             * send an old-session ABORT through it, even if batch IDs collide. */
            rx_terminate(s_rx.batch,HOP_ABORT_CANCEL,0);
        if (s_rx.active) {
            rx_close_file();
            rx_discard_dir();
            if (s_rx.batch & HOP_XFER_PASTE_BATCH_FLAG)
                clipboard_complete_remote_paste(s_rx.batch,NULL,0,0);
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
    if (!strstr(s_rx.dir, "/HopDesk/")) {
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %s", s_rx.dir);
        return;
    }
    if (nftw(s_rx.dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
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
static void rx_terminate(uint32_t batch, uint8_t reason, int notify_peer) {
    if (!batch) return;
    if (batch & HOP_XFER_PASTE_BATCH_FLAG)
        clipboard_complete_remote_paste(batch,NULL,0,0);
    if (notify_peer) { send_abort(batch,reason); }
    if (!hop_xfer_rx_batch_is_active(batch, s_rx.active, s_rx.batch)) return;
    rx_close_file();
    rx_discard_dir();
    hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle, batch, reason);
    hop_xfer_task_finish(&s_rx.task,reason==HOP_ABORT_CANCEL?
                         HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
    rx_clear_state();
    status_end(HOP_XFER_DIR_RECEIVE,batch,
               reason==HOP_ABORT_CANCEL?HOP_XFER_CANCELLED:HOP_XFER_FAILED,
               reason==HOP_ABORT_CANCEL?"Receive cancelled":"Receive aborted");
}

static void rx_begin_batch(uint32_t batch) {
    uint8_t dead_reason = 0;
    int committed = hop_xfer_rx_done_receipt_find(
        &s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle), batch) != NULL;
    (void)hop_xfer_rx_lifecycle_dead_reason(&s_rx_lifecycle, batch, &dead_reason);
    hop_xfer_rx_batch_begin_action begin_action = hop_xfer_rx_batch_begin_decide(
        batch, committed, dead_reason, s_rx.active, s_rx.batch, &dead_reason);
    switch (begin_action) {
        case HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED:
            return; /* already committed in this session; never recreate its task */
        case HOP_RX_BATCH_BEGIN_REJECT_DEAD:
            /* A cancelled/aborted batch must never recreate its staging directory. */
            rx_terminate(batch, dead_reason, 1);
            return;
        case HOP_RX_BATCH_BEGIN_RESUME:
            return; /* duplicate BEGIN for the active batch */
        case HOP_RX_BATCH_BEGIN_START:
            break;
        case HOP_RX_BATCH_BEGIN_INVALID:
        default:
            return;
    }
    /* 清掉上一批遗留的"接收侧取消"请求。
     *
     * 用户可能在"没有接收在途"的时候点取消（比如只在发文件），
     * 那时 cancel_rx 置了 1 却没人消费 —— 它会一直躺到对端下一次
     * 开始发文件，在第一个数据帧之后被 test_and_set 吃掉，
     * 把刚建立的新批次误判成"用户要取消"。批次一换，旧请求就该作废。 */
    __sync_lock_test_and_set(&s_x.cancel_rx, 0);

    rx_reset();
    if (!hop_xfer_task_init_classified(&s_rx.task,0,(batch & HOP_XFER_PASTE_BATCH_FLAG)?batch:0,batch,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_UNSPECIFIED)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_DISK);
        send_abort(batch,HOP_ABORT_DISK);
        return;
    }
    (void)hop_xfer_task_bind_session(&s_rx.task,hop_xfer_rx_lifecycle_session(&s_rx_lifecycle)->bytes);
    if (!hop_xfer_task_reserve_job(&s_rx.task)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_TOO_LARGE);
        hop_xfer_task_finish(&s_rx.task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        send_abort(batch,HOP_ABORT_TOO_LARGE);
        return;
    }
    if (!hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_DISK);
        hop_xfer_task_finish(&s_rx.task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        send_abort(batch,HOP_ABORT_DISK);
        return;
    }
    /* Keep the last tombstone: a late old BEGIN must not replace a newer task. */
    s_rx.active = 1;
    s_rx.batch = batch;
    hop_log(HOP_LOG_INFO,"xfer: receive batch %u task=%llu",
            batch,(unsigned long long)s_rx.task.id);
    status_begin(batch,HOP_XFER_DIR_RECEIVE,0,0,"Incoming files",HOP_XFER_RECEIVING);

    char session_hex[33]; hop_session_hex(&s_x.session, session_hex);
    snprintf(s_rx.dir, sizeof(s_rx.dir), "%s/HopDesk/%s/%u", temp_root(), session_hex, batch);
    if (mkdir_p(s_rx.dir) != 0) {
        rx_terminate(batch,HOP_ABORT_DISK,1);
        return;
    }
}

static int rx_finish_batch(uint32_t batch, uint32_t ok, uint32_t total) {
    if (!hop_xfer_rx_batch_is_active(batch, s_rx.active, s_rx.batch)) {
        hop_log(HOP_LOG_WARN, "xfer: stale DONE for batch %u ignored (active=%u)",
                batch, s_rx.active ? s_rx.batch : 0);
        return -1;
    }
    /* Completion and per-task cancellation serialize before clipboard effects. */
    int completion_claimed = hop_xfer_task_begin_completion(&s_rx.task);
    if (!completion_claimed) {
        hop_xfer_rx_batch_finish_action cancelled = hop_xfer_rx_batch_finish_decide(
            batch, s_rx.active, s_rx.batch, 0, ok, total, s_rx.cur_is_image, 0);
        if (cancelled == HOP_RX_BATCH_FINISH_CANCELLED)
            rx_terminate(batch,HOP_ABORT_CANCEL,1);
        return -1;
    }

    rx_task t;
    memset(&t, 0, sizeof(t));
    t.kind = RX_CLOSE;
    wq_push(&t);
    wq_drain();

    if (ok != total) {
        /* Keep the staged data for diagnosis/recovery, but never expose a partial
         * folder as a successful clipboard item. */
        hop_log(HOP_LOG_ERROR,
                "xfer: batch %u incomplete (%u/%u entries); staged data kept at %s; clipboard not updated",
                batch, ok, total, s_rx.dir);
    } else if (s_rx.cur_is_image) {
        if (s_rx.img && s_rx.img_len == s_rx.img_expected) {
            uint64_t clipboard_started_ns=rate_now_ns();
            clipboard_apply_image(s_rx.img, s_rx.img_w, s_rx.img_h, s_rx.img_stride);
            status_stage_time(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                              rate_now_ns()-clipboard_started_ns);
        } else
            hop_log(HOP_LOG_ERROR, "xfer: incomplete image rejected (%llu/%llu bytes)",
                    (unsigned long long)s_rx.img_len, (unsigned long long)s_rx.img_expected);
    } else if (s_rx.ntop > 0) {
        uint64_t clipboard_started_ns=rate_now_ns();
        if (batch & HOP_XFER_PASTE_BATCH_FLAG) {
            /* A requested cross-device paste must pass the request ID and
             * original clipboard version gate; never use the generic path. */
            clipboard_complete_remote_paste(batch,(const char *const *)s_rx.tops,s_rx.ntop,1);
        } else {
            clipboard_apply_paths((const char *const *)s_rx.tops, s_rx.ntop);
        }
        status_stage_time(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns()-clipboard_started_ns);
    }
    if ((batch & HOP_XFER_PASTE_BATCH_FLAG) && ok != total)
        clipboard_complete_remote_paste(batch,NULL,0,0);

    hop_log(HOP_LOG_INFO, "xfer: batch %u done (%d items, %u/%u ACKed)",
            batch, s_rx.ntop, ok, total);
    pthread_mutex_lock(&s_status_lock);
    s_receive_status.status.files_total=total;
    pthread_mutex_unlock(&s_status_lock);
    if(s_rx.cur_is_image && ok==total && s_rx.img_len==s_rx.img_expected)
        status_file_phase(HOP_XFER_DIR_RECEIVE,batch,0,HOP_XFER_FILE_COMPLETED,1);
    int image_valid = s_rx.img && s_rx.img_len == s_rx.img_expected;
    hop_xfer_rx_batch_finish_action finish_action = hop_xfer_rx_batch_finish_decide(
        batch, s_rx.active, s_rx.batch, 1, ok, total, s_rx.cur_is_image, image_valid);
    int complete = finish_action == HOP_RX_BATCH_FINISH_COMPLETE;
    hop_xfer_task_finish(&s_rx.task,complete?HOP_XFER_TASK_COMPLETED:(s_rx.cur_is_image?HOP_XFER_TASK_FAILED:HOP_XFER_TASK_PARTIAL));
    status_end(HOP_XFER_DIR_RECEIVE,batch,complete?HOP_XFER_COMPLETED:
               (s_rx.cur_is_image?HOP_XFER_FAILED:HOP_XFER_PARTIAL),
               complete?"Receive completed":"Receive incomplete; clipboard not updated");
    int result=finish_action==HOP_RX_BATCH_FINISH_COMPLETE?HOP_DONE_ACK_COMPLETE:
               (finish_action==HOP_RX_BATCH_FINISH_IMAGE_FAILED?HOP_DONE_ACK_FAILED:HOP_DONE_ACK_PARTIAL);
    rx_reset();
    return result;
}

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

    switch (type) {

        case HOP_MSG_FILE_BEGIN: {
            hop_xfer_file_begin_msg msg;
            if (!hop_xfer_decode_file_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
                if (s_rx.active) rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                pthread_mutex_lock(&s_x.slock);
                int active = s_x.sock;
                pthread_mutex_unlock(&s_x.slock);
                if (active >= 0) shutdown(active, SHUT_RDWR);
                return;
            }
            uint32_t batch = msg.batch;
            uint32_t id = msg.id;
            uint64_t size = msg.size;
            uint32_t flags = msg.flags;
            uint32_t plen = msg.relpath_len;

            rx_begin_batch(batch);
            if (!s_rx.active || s_rx.batch != batch) return;
            (void)hop_xfer_task_set_kind(&s_rx.task,HOP_TASK_KIND_FILES);
            if (id == 0 || id > HOP_XFER_FILE_LIMIT) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            /* 先登记当前条目，确保坏路径也能在 FILE_END 时回 ACK。 */
            free(s_rx.cur_path);
            s_rx.cur_path     = NULL;
            s_rx.cur_id       = id;
            s_rx.cur_is_image = 0;
            s_rx.cur_is_dir   = (flags & HOP_FILE_FLAG_DIR) ? 1 : 0;
            s_rx.cur_fail     = -1;
            s_rx.cur_size     = 0;
            s_rx.cur_received = 0;
            s_rx.fcrc         = 0;
            hop_writer_queue_error_reset(s_wq);

            char rel[HOP_PATH_LEN_MAX + 1];
            if (!hop_xfer_relpath_bytes_safe(msg.relpath, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe relative path rejected");
                s_rx.cur_fail = HOP_FILE_ACK_BADPATH;
                break;
            }
            memcpy(rel, msg.relpath, plen);
            rel[plen] = '\0';


            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", s_rx.dir, rel);

            s_rx.cur_path = strdup(path);
            s_rx.cur_size = size;
            if (!s_rx.cur_path) { s_rx.cur_fail=HOP_FILE_ACK_DISK; break; }
            rx_remove_top(s_rx.cur_path);
            if (!s_rx.reserved_entries[id - 1]) {
                if (!hop_xfer_task_reserve_entry(&s_rx.task,size)) {
                    rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                s_rx.reserved_entries[id - 1] = 1;
                s_rx.reserved_entry_sizes[id - 1] = size;
            } else if (s_rx.reserved_entry_sizes[id - 1] != size) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            status_add_received_file(batch,id,rel,size,s_rx.cur_is_dir);

            if (s_rx.cur_is_dir) {
                if (mkdir_p(path) != 0) {
                    s_rx.cur_fail = HOP_FILE_ACK_DISK;
                    break;
                }
                /* 顶层目录才进剪贴板清单 */
                if (!strchr(rel, '/') && !rx_add_top(path))
                    s_rx.cur_fail = HOP_FILE_ACK_DISK;
            } else {
                /* 建父目录；失败时不能把后续文件错误地 ACK 成功。 */
                char *sep = strrchr(path, '/');
                if (sep) {
                    *sep = '\0';
                    int parent_ok = mkdir_p(path) == 0;
                    *sep = '/';
                    if (!parent_ok) {
                        s_rx.cur_fail = HOP_FILE_ACK_DISK;
                        break;
                    }
                }

                /* 打开交给写线程 —— 描述符归它独占 */
                rx_task t;
                memset(&t, 0, sizeof(t));
                t.kind = RX_OPEN;
                snprintf(t.path, sizeof(t.path), "%s", path);
                if (!wq_push(&t)) s_rx.cur_fail = HOP_FILE_ACK_DISK;

                /* 不在这里进剪贴板清单 —— 此刻文件还没写成功。
                 * 写盘失败 / 校验失败的文件绝不能出现在剪贴板里，
                 * 否则用户粘贴到的是一份截断的、看起来完全正常的文件。
                 * 由 FILE_END 的成功分支决定是否登记。 */
            }
            break;
        }

        case HOP_MSG_IMAGE_BEGIN: {
            hop_xfer_image_begin_msg msg;
            if (!hop_xfer_decode_image_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed IMAGE_BEGIN rejected");
                if (s_rx.active) rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint32_t batch = msg.batch;
            rx_begin_batch(batch);
            if (!s_rx.active || s_rx.batch != batch) return;
            (void)hop_xfer_task_set_kind(&s_rx.task,HOP_TASK_KIND_IMAGE);

            int w = (int)msg.width;
            int h = (int)msg.height;
            int stride = (int)msg.stride;

            /* 对端声称的尺寸必须自己验：不然 w=h=100000 就能把内存吃光。
             * 这正是"避免内存不断增长"的边界。 */
            if (!hop_xfer_image_valid(w, h, stride)) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            size_t expected = (size_t)stride * (size_t)h;
            if (!s_rx.cur_is_image) {
                if (!hop_xfer_task_reserve_image(&s_rx.task,(uint64_t)expected)) {
                    rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
            } else if (s_rx.img_expected != expected) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            s_rx.cur_is_image = 1;
            s_rx.cur_id = msg.id;
            s_rx.img_w = w;
            s_rx.img_h = h;
            s_rx.img_stride = stride;
            s_rx.img_expected = expected;
            status_image(batch,HOP_XFER_DIR_RECEIVE,s_rx.img_expected);
            s_rx.img_len = 0;
            break;
        }

        case HOP_MSG_FILE_CHUNK: {
            if (!s_rx.active) return; /* late data after termination */
            uint64_t receive_started_ns=rate_now_ns();
            if (len < 20) { rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1); return; }
            uint64_t off = hop_get_be64(p + 4);
            uint32_t n   = hop_get_be32(p + 12);
            uint32_t crc = hop_get_be32(p + 16);
            if (!hop_xfer_chunk_valid(p, len,
                    s_rx.cur_is_image ? s_rx.img_expected :
                        (s_rx.cur_fail >= 0 ? UINT64_MAX : s_rx.cur_size),
                    s_rx.cur_is_image)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed or out-of-range chunk; aborting receive");
                rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            if (hop_get_be32(p) != s_rx.cur_id) {
                rx_terminate(s_rx.batch,HOP_ABORT_CRC,1);
                return;
            }
            const uint8_t *data = p + 20;

            /* 逐块校验。TCP 的 16 位校验和 + 网卡 offload 有漏检可能，
             * 漏检一次就是静默写入损坏数据。这里宁可报错也不要写坏文件。
             *
             * 坏一块只作废这一个文件，不再 ABORT 整批：
             * 发送端收到 ACK_CRC 会单独重发它，其余文件照常落地。 */
            uint64_t crc_started_ns=rate_now_ns();
            int crc_ok=(hop_crc32(0, data, n) == crc);
            uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
            if (!crc_ok) {
                status_stage_time(HOP_XFER_DIR_RECEIVE,s_rx.batch,HOP_XFER_TIME_CRC,crc_elapsed_ns);
                status_stage_time(HOP_XFER_DIR_RECEIVE,s_rx.batch,HOP_XFER_TIME_RECEIVE,
                                  rate_now_ns()-receive_started_ns);
                hop_log(HOP_LOG_ERROR,
                        "xfer: CHUNK CRC MISMATCH off=%llu len=%u (expect %08x)",
                        (unsigned long long)off, n, crc);
                if (s_rx.cur_is_image) {
                    rx_terminate(s_rx.batch,HOP_ABORT_CRC,1);
                } else {
                    s_rx.cur_fail = HOP_FILE_ACK_CRC;
                }
                return;
            }

            if (s_rx.cur_is_image) {
                size_t need = (size_t)off + n;
                if (off != s_rx.img_len || need > s_rx.img_expected) {
                    hop_log(HOP_LOG_ERROR, "xfer: image chunk gap/overlap or length overflow");
                    rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                /* IMAGE_BEGIN already reserved img_expected bytes in the
                 * task ledger. Allocate that exact amount once: geometric growth
                 * would make the real capacity exceed the reservation and may
                 * transiently keep both old and new buffers alive during realloc. */
                if (!s_rx.img) {
                    s_rx.img = (uint8_t *)realloc(NULL, s_rx.img_expected);
                    if (!s_rx.img) { rx_terminate(s_rx.batch,HOP_ABORT_DISK,1); return; }
                    s_rx.img_cap = s_rx.img_expected;
                }
                if (need > s_rx.img_cap) {
                    hop_log(HOP_LOG_ERROR, "xfer: image buffer capacity exceeded");
                    rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                memcpy(s_rx.img + off, data, n);
                if (need > s_rx.img_len) s_rx.img_len = need;
                uint64_t receive_elapsed_ns=rate_now_ns()-receive_started_ns;
                status_chunk_timed(HOP_XFER_DIR_RECEIVE,s_rx.batch,s_rx.cur_id,off,n,
                                   0,crc_elapsed_ns,0,receive_elapsed_ns);
                break;
            }

            /* TCP is ordered: reject gaps, overlaps and chunks after a prior
             * file failure instead of creating sparse or truncated files. */
            if (s_rx.cur_fail >= 0) break;
            if (off != s_rx.cur_received || off > s_rx.cur_size ||
                (uint64_t)n > s_rx.cur_size - off) {
                hop_log(HOP_LOG_ERROR, "xfer: non-contiguous file chunk rejected (off=%llu received=%llu size=%llu)",
                        (unsigned long long)off, (unsigned long long)s_rx.cur_received,
                        (unsigned long long)s_rx.cur_size);
                s_rx.cur_fail = HOP_FILE_ACK_CRC;
                break;
            }
            /* 不许超出声明的大小：否则对端能一直灌，把磁盘写爆 */
            if (off + n > s_rx.cur_size) {
                hop_log(HOP_LOG_ERROR, "xfer: chunk beyond declared size (%llu+%u > %llu)",
                        (unsigned long long)off, n, (unsigned long long)s_rx.cur_size);
                return;
            }

            /* 校验通过才入队。拷贝一份交给写线程 —— 解码器的缓冲
             * 在回调返回后就被释放了，不能把指针递出去。 */
            if (!wq_push_data(&s_rx.task, off, data, n)) {
                s_rx.cur_fail = HOP_FILE_ACK_DISK;
                break;
            }
            s_rx.cur_received += n;
            /* 文件 CRC 折入本块 CRC：既覆盖数据（块 CRC 已验证），
             * 也覆盖顺序与偏移，且不需要把整份文件重算一遍 */
            s_rx.fcrc = hop_xfer_file_crc_step(s_rx.fcrc, off, crc);
            uint64_t receive_elapsed_ns=rate_now_ns()-receive_started_ns;
            status_chunk_timed(HOP_XFER_DIR_RECEIVE,s_rx.batch,s_rx.cur_id,off,n,
                               0,crc_elapsed_ns,0,receive_elapsed_ns);
            break;
        }

        case HOP_MSG_FILE_END: {
            hop_xfer_file_end_msg end_msg;
            if (!hop_xfer_decode_file_end(p, len, &end_msg)) return;
            uint32_t id = end_msg.id;
            if (!s_rx.active || id != s_rx.cur_id) return;
            uint32_t want = end_msg.file_crc;
            uint8_t status = end_msg.status;

            /* 图片整批走，没有逐文件回执：发送端不会等 */
            if (s_rx.cur_is_image) break;

            status_file_phase(HOP_XFER_DIR_RECEIVE,s_rx.batch,(int)id-1,HOP_XFER_FILE_VERIFYING,0);
            int ack = HOP_FILE_ACK_OK;

            /* Always drain and close, including a file already marked bad by
             * a chunk CRC or queue error. Failure cleanup must never race the
             * writer's still-open handle. */
            if (!s_rx.cur_is_dir) rx_close_file();

            if (s_rx.cur_fail >= 0) {
                ack = s_rx.cur_fail;
            } else {
                if (wq_error_get()) {
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %s incomplete",
                            s_rx.cur_path ? s_rx.cur_path : "?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %s",
                            (unsigned)status, s_rx.cur_path ? s_rx.cur_path : "?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (s_rx.cur_received != s_rx.cur_size) {
                    hop_log(HOP_LOG_ERROR, "xfer: incomplete file rejected (%llu/%llu bytes)",
                            (unsigned long long)s_rx.cur_received,
                            (unsigned long long)s_rx.cur_size);
                    ack = HOP_FILE_ACK_CRC;
                } else if (want != s_rx.fcrc) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %s (expect %08x got %08x)",
                            s_rx.cur_path ? s_rx.cur_path : "?", want, s_rx.fcrc);
                    ack = HOP_FILE_ACK_CRC;
                } else if (s_rx.cur_path && !s_rx.cur_is_dir) {
                    /* 落盘成功、校验通过，才登记为可粘贴条目。
                     * 目录条目不在这里登记 —— 它在 BEGIN 时就已建好并登记。 */
                    const char *tail = s_rx.cur_path + strlen(s_rx.dir) + 1;
                    if (!strchr(tail, '/') && !rx_add_top(s_rx.cur_path))
                        ack = HOP_FILE_ACK_DISK;
                }
            }


            if (ack != HOP_FILE_ACK_OK && !s_rx.cur_is_dir && s_rx.cur_path) {
                if (unlink(s_rx.cur_path) != 0 && errno != ENOENT)
                    hop_log(HOP_LOG_WARN, "xfer: failed to remove incomplete file %s (%s)",
                            s_rx.cur_path, strerror(errno));
            }

            status_file_phase(HOP_XFER_DIR_RECEIVE,s_rx.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            {
                pthread_mutex_lock(&s_x.slock);
                int fd = s_x.sock;
                pthread_mutex_unlock(&s_x.slock);
                if (fd >= 0) send_ack(fd, s_rx.batch, id, ack);
            }
            break;
        }

        case HOP_MSG_FILE_ACK: {
            hop_xfer_file_ack_msg ack;
            if (hop_xfer_decode_file_ack(p, len, &ack) && ack.id >= 1 && ack.id <= HOP_XFER_FILE_LIMIT)
                ack_deliver(ack.batch, ack.id, ack.status);
            break;
        }

        case HOP_MSG_XFER_ABORT: {
            hop_xfer_abort_msg abort_msg;
            if (!hop_xfer_decode_abort(p, len, &abort_msg)) return;
            uint32_t batch = abort_msg.batch;
            uint8_t reason = abort_msg.reason;

            if (abort_msg.target == HOP_ABORT_TO_SENDER) {
                pthread_mutex_lock(&s_x.qlock);
                if (s_x.cur_batch == batch) { s_x.abort_batch = batch; s_x.abort_reason = reason; }
                pthread_mutex_unlock(&s_x.qlock);
                pthread_mutex_lock(&s_ack.lock);
                pthread_cond_broadcast(&s_ack.cond);
                pthread_mutex_unlock(&s_ack.lock);
            }

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。 */
            if (abort_msg.target == HOP_ABORT_TO_RECEIVER) rx_terminate(batch,reason,0);
            break;
        }

        case HOP_MSG_XFER_DONE: {
            hop_xfer_done_msg done;
            if (!hop_xfer_decode_done(p, len, &done)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed XFER_DONE ignored");
                return;
            }
            if (!done.batch || !done.total || done.total > HOP_XFER_FILE_LIMIT || done.ok > done.total) return;
            int result;
            const hop_xfer_rx_done_receipt *receipt =
                hop_xfer_rx_done_receipt_find(&s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle), done.batch);
            if (receipt) {
                if (!hop_xfer_rx_done_receipt_matches_payload(receipt, &done)) return;
                result=receipt->status; /* repeat ACK without repeating clipboard effects */
            } else {
                result=rx_finish_batch(done.batch,done.ok,done.total);
                if (result<0) return;
                hop_xfer_rx_done_receipt_record(&s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle),
                                                &done, (uint8_t)result);
            }
            uint8_t reply[5]; hop_put_be32(reply,done.batch); reply[4]=(uint8_t)result;
            pthread_mutex_lock(&s_x.slock);
            int fd=s_x.sock;
            pthread_mutex_unlock(&s_x.slock);
            if (fd >= 0) (void)send_small(fd,HOP_MSG_XFER_DONE_ACK,reply,sizeof(reply),done.batch);
            break;
        }

        case HOP_MSG_XFER_DONE_ACK:
            if (len==5 && hop_get_be32(p)!=0 && p[4]<=HOP_DONE_ACK_FAILED)
                ack_deliver(hop_get_be32(p),HOP_DONE_ACK_SLOT,p[4]);
            break;

        default:
            break;
    }
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
        (!s_rx.active || batch != s_rx.batch)) return;
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
    if (!s_rx.active) return;
    uint32_t batch = s_rx.batch;
    rx_terminate(batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
static void rx_poll_cancel(void) {
    rx_expire_stale_session();
    if (__sync_lock_test_and_set(&s_x.cancel_rx, 0) || (s_rx.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    pthread_mutex_lock(&s_x.slock); int connected=s_x.peer_ready; pthread_mutex_unlock(&s_x.slock);
    if (s_rx.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch);
        rx_terminate(s_rx.batch,HOP_ABORT_TIMEOUT,0);
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

