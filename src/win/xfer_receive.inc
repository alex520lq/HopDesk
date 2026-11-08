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
        hop_log(HOP_LOG_WARN, "xfer: writer worker did not exit, retain queue state");
        return;
    }
    hop_writer_queue_destroy(s_wq);
    s_wq = NULL;
}

/* ---------------- 接收 ---------------- */

typedef struct {
    uint32_t  batch;
    int       active;
    hop_xfer_task task;
    uint8_t   reserved_entries[HOP_XFER_FILE_LIMIT];
    uint64_t  reserved_entry_sizes[HOP_XFER_FILE_LIMIT];
    wchar_t   dir[MAX_PATH];
    uint32_t  cur_id;
    wchar_t  *cur_path;
    int       cur_is_image;
    int       cur_is_dir;    /* 目录在 BEGIN 时已登记，FILE_END 不能再登记一次 */
    int       cur_fail;      /* 本条目已判死：-1 = 正常，否则是 FILE_END 要回的 ACK 状态。
                              * 坏路径和坏数据都在这里落定，FILE_END 只负责回执。 */
    uint64_t  cur_size;      /* 声明的大小：超出就拒收，防止被灌成无限大 */
    uint64_t  cur_received; /* 已按连续偏移成功入队的文件字节数 */
    /* 当前文件的校验累加：每块 CRC 折进来，FILE_END 时与发送端比对 */
    uint32_t  fcrc;
    /* 图片累积 */
    uint8_t  *img;
    size_t    img_cap, img_len, img_expected;
    int       img_w, img_h, img_stride;
    /* 本批顶层条目（用于最后设置剪贴板） */
    wchar_t **tops;
    int       ntop;
} xfer_rx;

static xfer_rx s_rx;

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
    s_rx.dir[0] = L'\0';
}

static void rx_reset(void) {
    if(s_rx.active)status_end(HOP_XFER_DIR_RECEIVE,s_rx.batch,HOP_XFER_FAILED,"Receive interrupted");
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
        if (s_rx.active && !hop_xfer_task_begin_completion(&s_rx.task) &&
            hop_xfer_task_cancel_requested(s_rx.task.id))
            /* The current socket may already belong to the new session. Never
             * send an old-session ABORT through it, even if batch IDs collide. */
            rx_terminate(s_rx.batch,HOP_ABORT_CANCEL,0);
        if (s_rx.active) {
            rx_close_file();
            rx_discard_dir();
            if (s_rx.batch & HOP_XFER_PASTE_BATCH_FLAG)
                clip_complete_remote_paste(s_rx.batch,NULL,0,0);
        }
        rx_reset();
        AcquireSRWLockExclusive(&s_status_lock);
        memset(&s_receive_status,0,sizeof(s_receive_status));
        ReleaseSRWLockExclusive(&s_status_lock);
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
    if (!wcsstr(s_rx.dir, XFER_RX_ROOT)) {
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %ls", s_rx.dir);
        return;
    }
    wchar_t buf[MAX_PATH + 2];
    size_t n = wcslen(s_rx.dir);
    if (n + 2 > MAX_PATH + 2) return;
    wcscpy(buf, s_rx.dir);
    buf[n + 1] = L'\0';                 /* SHFileOperationW 要双 null 结尾 */

    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = buf;
    op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0)
        hop_log(HOP_LOG_WARN, "xfer: failed to remove %ls", s_rx.dir);
    s_rx.dir[0] = L'\0';
}

/* Create a directory, accepting ERROR_ALREADY_EXISTS only when the path
 * is actually a directory (a regular file at a parent path is an error). */
static int create_directory_if_missing(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return 1;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* Create parent components without attempting to create a drive/UNC root. */
static int ensure_dirs(wchar_t *path) {
    wchar_t *p = path;
    if (((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z')) && p[1] == L':') {
        p += 2;
        if (*p == L'\\' || *p == L'/') ++p;
    } else if (p[0] == L'\\' && p[1] == L'\\') {
        p += 2;
        while (*p && *p != L'\\' && *p != L'/') ++p; /* server */
        if (*p) ++p;
        while (*p && *p != L'\\' && *p != L'/') ++p; /* share */
        if (*p) ++p;
    }
    for (; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t save = *p;
            *p = L'\0';
            int ok = create_directory_if_missing(path);
            *p = save;
            if (!ok) return 0;
        }
    }
    return 1;
}

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
static wchar_t *rx_path_for(const char *rel) {
    wchar_t *w = u2w(rel, (int)strlen(rel));
    if (!w) return NULL;
    for (wchar_t *p = w; *p; ++p) if (*p == L'/') *p = L'\\';

    size_t dl = wcslen(s_rx.dir), wl = wcslen(w);
    wchar_t *o = (wchar_t *)malloc((dl + wl + 2) * sizeof(wchar_t));
    if (!o) { free(w); return NULL; }
    wcscpy(o, s_rx.dir);
    if (dl && o[dl - 1] != L'\\') wcscat(o, L"\\");
    wcscat(o, w);
    free(w);
    return o;
}

/* Receiver-thread only. Wire notification has no status side effects. Close the
 * writer before freeing paths/buffers or deleting staging files. Terminal state
 * is idempotent; repeated/stale ABORTs cannot end an unrelated active batch. */
static void rx_terminate(uint32_t batch, uint8_t reason, int notify_peer) {
    if (!batch) return;
    if (batch & HOP_XFER_PASTE_BATCH_FLAG)
        clip_complete_remote_paste(batch,NULL,0,0);
    if (notify_peer) { send_abort_msg(s_x.client,batch,reason,HOP_ABORT_TO_SENDER); }
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
     * 开始发文件，在第一个数据帧之后被 InterlockedExchange 吃掉，
     * 把刚建立的新批次误判成"用户要取消"。批次一换，旧请求就该作废。 */
    InterlockedExchange(&s_x.cancel_rx, 0);

    rx_reset();
    if (!hop_xfer_task_init_classified(&s_rx.task,0,(batch & HOP_XFER_PASTE_BATCH_FLAG)?batch:0,batch,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_UNSPECIFIED)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_DISK);
        send_abort_msg(s_x.client,batch,HOP_ABORT_DISK,HOP_ABORT_TO_SENDER);
        return;
    }
    (void)hop_xfer_task_bind_session(&s_rx.task,hop_xfer_rx_lifecycle_session(&s_rx_lifecycle)->bytes);
    if (!hop_xfer_task_reserve_job(&s_rx.task)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_TOO_LARGE);
        hop_xfer_task_finish(&s_rx.task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        send_abort_msg(s_x.client,batch,HOP_ABORT_TOO_LARGE,HOP_ABORT_TO_SENDER);
        return;
    }
    if (!hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE)) {
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle,batch,HOP_ABORT_DISK);
        hop_xfer_task_finish(&s_rx.task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        send_abort_msg(s_x.client,batch,HOP_ABORT_DISK,HOP_ABORT_TO_SENDER);
        return;
    }
    /* Keep the last tombstone: a late old BEGIN must not replace a newer task. */
    s_rx.active = 1;
    s_rx.batch = batch;
    hop_log(HOP_LOG_INFO,"xfer: receive batch %u task=%llu",
            batch,(unsigned long long)s_rx.task.id);
    status_begin(batch,HOP_XFER_DIR_RECEIVE,0,0,"Incoming files",HOP_XFER_RECEIVING);

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        rx_terminate(batch,HOP_ABORT_DISK,1);
        return;
    }

    /* 手工拼接并显式检查长度 —— wsprintfW 没有边界检查，临时目录路径
     * 一旦接近 MAX_PATH 就会直接越界写。
     * 注意：swprintf 的 %s 要的是【窄】字符串（%ls 才是宽的），
     * 用 %s 传 L"HopDesk" 只会读到第一个字符 'H'。这里不碰格式化，直接拼。 */
    wchar_t sub[80];
    char session_hex[33]; hop_session_hex(&s_x.session, session_hex);
    wchar_t num[16];
    if (swprintf(num, 16, L"%u", batch) <= 0) { rx_terminate(batch,HOP_ABORT_DISK,1); return; }
    wcscpy(sub, XFER_RX_ROOT);
    wcscat(sub, L"\\");
    for (int i=0;i<32;i++) { size_t at=wcslen(sub); sub[at]=(wchar_t)session_hex[i];sub[at+1]=0; }
    wcscat(sub, L"\\");
    wcscat(sub, num);

    size_t sub_len = wcslen(sub);
    if ((size_t)n + sub_len + 1 > MAX_PATH) {
        hop_log(HOP_LOG_WARN, "xfer: temp path too long, batch %u dropped", batch);
        rx_terminate(batch,HOP_ABORT_DISK,1);
        return;
    }

    wcscpy(s_rx.dir, tmp);
    wcscat(s_rx.dir, sub);


    if (!ensure_dirs(s_rx.dir) || !create_directory_if_missing(s_rx.dir)) {
        rx_terminate(batch,HOP_ABORT_DISK,1);
        return;
    }

    /* GetTempPathW 可能返回 8.3 短路径（ADMINI~1），剪贴板里给用户看的
     * 路径要换成完整形式 */
    wchar_t full[MAX_PATH];
    DWORD fl = GetLongPathNameW(s_rx.dir, full, MAX_PATH);
    if (fl > 0 && fl < MAX_PATH) wcscpy(s_rx.dir, full);
}

/* 收完后把顶层条目放进剪贴板（CF_HDROP），Mac 那边就能直接粘贴 */
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

    /* 落盘全部完成后才能把路径挂到剪贴板。 */
    rx_task t;
    memset(&t, 0, sizeof(t));
    t.kind = RX_CLOSE;
    wq_push(&t);
    wq_drain();

    if (ok != total) {
        /* Keep the staged files for recovery, but never advertise a partial folder
         * through CF_HDROP as though it were a successful transfer. */
        hop_log(HOP_LOG_ERROR,
                "xfer: batch %u incomplete (%u/%u entries); staged data kept at %ls; clipboard not updated",
                batch, ok, total, s_rx.dir);
    } else if (s_rx.cur_is_image) {
        if (s_rx.img && s_rx.img_len == s_rx.img_expected) {
            uint64_t clipboard_started_ns=rate_now_ns();
            clip_apply_remote_image(s_rx.img, s_rx.img_w, s_rx.img_h, s_rx.img_stride);
            status_stage_time(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                              rate_now_ns()-clipboard_started_ns);
        } else
            hop_log(HOP_LOG_ERROR, "xfer: incomplete image rejected (%llu/%llu bytes)",
                    (unsigned long long)s_rx.img_len, (unsigned long long)s_rx.img_expected);
    } else if (s_rx.ntop > 0) {
        uint64_t clipboard_started_ns=rate_now_ns();
        if (batch & HOP_XFER_PASTE_BATCH_FLAG) {
            /* Requested cross-device paste must pass request/version validation. */
            clip_complete_remote_paste(batch,(const wchar_t *const *)s_rx.tops,s_rx.ntop,1);
        } else {
            clip_apply_remote_paths((const wchar_t *const *)s_rx.tops, s_rx.ntop);
        }
        status_stage_time(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns()-clipboard_started_ns);
    }
    if ((batch & HOP_XFER_PASTE_BATCH_FLAG) && ok != total)
        clip_complete_remote_paste(batch,NULL,0,0);
    hop_log(HOP_LOG_INFO, "xfer: batch %u done (%d items, %u/%u ACKed)",
            batch, s_rx.ntop, ok, total);
    AcquireSRWLockExclusive(&s_status_lock);
    s_receive_status.status.files_total=total;
    ReleaseSRWLockExclusive(&s_status_lock);
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

    switch (type) {

        case HOP_MSG_FILE_BEGIN: {
            hop_xfer_file_begin_msg msg;
            if (!hop_xfer_decode_file_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
                if (s_rx.active) rx_terminate(s_rx.batch,HOP_ABORT_TOO_LARGE,1);
                EnterCriticalSection(&s_x.lock);
                SOCKET active = s_x.client;
                LeaveCriticalSection(&s_x.lock);
                if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
                return;
            }
            uint32_t batch = msg.batch;
            uint32_t id = msg.id;
            uint64_t size = msg.size;
            uint32_t flags = msg.flags;
            uint32_t plen = msg.relpath_len;
            const uint8_t *rel = msg.relpath;

            rx_begin_batch(batch);
            if (!s_rx.active || s_rx.batch != batch) return;
            (void)hop_xfer_task_set_kind(&s_rx.task,HOP_TASK_KIND_FILES);
            if (id == 0 || id > HOP_XFER_FILE_LIMIT) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            /* 先把"当前条目"登记好，哪怕它是坏的：FILE_END 要靠 cur_id 回 ACK，
             * 漏回一次，发送端就要白等 10 秒超时再重试。 */
            free(s_rx.cur_path);
            s_rx.cur_path     = NULL;
            s_rx.cur_id       = id;
            s_rx.cur_is_image = 0;
            s_rx.cur_is_dir   = (flags & HOP_FILE_FLAG_DIR) ? 1 : 0;
            s_rx.cur_fail     = -1;
            s_rx.cur_size     = 0;   /* 0 = 任何分块都越界被拒，坏条目不落盘 */
            s_rx.cur_received = 0;
            s_rx.fcrc         = 0;
            hop_writer_queue_error_reset(s_wq);

            char *relz = (char *)malloc((size_t)plen + 1);
            if (!relz) { s_rx.cur_fail = HOP_FILE_ACK_DISK; break; }
            memcpy(relz, rel, plen);
            relz[plen] = '\0';

            if (!hop_xfer_relpath_bytes_safe(rel, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", relz);
                free(relz);
                s_rx.cur_fail = HOP_FILE_ACK_BADPATH;
                break;
            }
            status_add_received_file(batch,id,relz,size,s_rx.cur_is_dir);
            wchar_t *path = rx_path_for(relz);
            free(relz);
            if (!path) { s_rx.cur_fail = HOP_FILE_ACK_BADPATH; break; }

            s_rx.cur_path = path;
            rx_remove_top(s_rx.cur_path);
            s_rx.cur_size = size;
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

            if (s_rx.cur_is_dir) {
                if (!ensure_dirs(path) || !create_directory_if_missing(path)) {
                    s_rx.cur_fail = HOP_FILE_ACK_DISK;
                    break;
                }
                /* 顶层目录才进剪贴板清单 */
                if (!wcschr(path + wcslen(s_rx.dir) + 1, L'\\') &&
                    !rx_add_top(path))
                    s_rx.cur_fail = HOP_FILE_ACK_DISK;
            } else {
                /* 建父目录；失败时不能把后续文件错误地 ACK 成功。 */
                wchar_t *sep = wcsrchr(path, L'\\');
                if (sep) {
                    wchar_t save = *sep;
                    *sep = L'\0';
                    int parent_ok = ensure_dirs(path) && create_directory_if_missing(path);
                    *sep = save;
                    if (!parent_ok) {
                        s_rx.cur_fail = HOP_FILE_ACK_DISK;
                        break;
                    }
                }
                /* 打开交给写线程 —— 句柄归它独占 */
                rx_task t;
                memset(&t, 0, sizeof(t));
                t.kind = RX_OPEN;
                wcsncpy(t.path, path, MAX_PATH - 1);
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
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
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
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %ls incomplete",
                            s_rx.cur_path ? s_rx.cur_path : L"?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %ls",
                            (unsigned)status, s_rx.cur_path ? s_rx.cur_path : L"?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (s_rx.cur_received != s_rx.cur_size) {
                    hop_log(HOP_LOG_ERROR, "xfer: incomplete file rejected (%llu/%llu bytes)",
                            (unsigned long long)s_rx.cur_received,
                            (unsigned long long)s_rx.cur_size);
                    ack = HOP_FILE_ACK_CRC;
                } else if (want != s_rx.fcrc) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %ls (expect %08x got %08x)",
                            s_rx.cur_path ? s_rx.cur_path : L"?", want, s_rx.fcrc);
                    ack = HOP_FILE_ACK_CRC;
                } else if (s_rx.cur_path && !s_rx.cur_is_dir) {
                    /* 落盘成功、校验通过，才登记为可粘贴条目。
                     * 目录条目不在这里登记 —— 它在 BEGIN 时就已建好并登记。 */
                    const wchar_t *tail = s_rx.cur_path + wcslen(s_rx.dir) + 1;
                    if (!wcschr(tail, L'\\') && !rx_add_top(s_rx.cur_path))
                        ack = HOP_FILE_ACK_DISK;
                }
            }


            if (ack != HOP_FILE_ACK_OK && !s_rx.cur_is_dir && s_rx.cur_path) {
                if (!DeleteFileW(s_rx.cur_path)) {
                    DWORD error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                        hop_log(HOP_LOG_WARN, "xfer: failed to remove incomplete file %ls (err %lu)",
                                s_rx.cur_path, error);
                }
            }

            status_file_phase(HOP_XFER_DIR_RECEIVE,s_rx.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            send_ack(s_x.client, s_rx.batch, id, ack);
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
                EnterCriticalSection(&s_x.lock);
                if (s_x.cur_batch == batch) { s_x.abort_batch = batch; s_x.abort_reason = reason; }
                LeaveCriticalSection(&s_x.lock);
                SetEvent(s_ack.ev);
            }

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。
             * 墓碑必须立 —— 否则对端再发一帧 FILE_BEGIN 就把目录又建回来了。 */
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
            EnterCriticalSection(&s_x.lock);
            SOCKET fd=s_x.client;
            LeaveCriticalSection(&s_x.lock);
            if (fd != INVALID_SOCKET) (void)send_small(fd,HOP_MSG_XFER_DONE_ACK,reply,sizeof(reply),done.batch);
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
 * 由 listen 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.active) return;
    uint32_t batch = s_rx.batch;
    rx_terminate(batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
static void rx_poll_cancel(void) {
    rx_expire_stale_session();
    if (InterlockedExchange(&s_x.cancel_rx, 0) || (s_rx.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    EnterCriticalSection(&s_x.lock); int connected=s_x.peer_ready; LeaveCriticalSection(&s_x.lock);
    if (s_rx.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch);
        rx_terminate(s_rx.batch,HOP_ABORT_TIMEOUT,0);
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
