static void rx_abort_send(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    send_abort_msg(s_x.client,batch,reason,HOP_ABORT_TO_SENDER);
}

static void rx_abort_cancel_clipboard(void *context, uint32_t batch) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.cancel_clipboard)
        rx->services.cancel_clipboard(rx->services.context, batch);
}

static void rx_abort_close_file(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.close_file)
        rx->services.close_file(rx->services.context);
}

static void rx_abort_discard_staging(void *context) {
    rx_discard_dir((xfer_rx_context *)context);
}

static void rx_abort_mark_dead(void *context, uint32_t batch, uint8_t reason) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->lifecycle)
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, reason);
}

static void rx_abort_finish_task(void *context, int cancelled) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx) hop_xfer_task_finish(&rx->task,
        cancelled ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED);
}

static void rx_abort_clear_state(void *context) {
    rx_clear_state((xfer_rx_context *)context);
}

static void rx_abort_status_end(void *context, uint32_t batch, int cancelled) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.status_end)
        rx->services.status_end(rx->services.context, batch,
            cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED,
            cancelled ? "Receive cancelled" : "Receive aborted");
}

static void rx_terminate(xfer_rx_context *rx, uint32_t batch, uint8_t reason, int notify_peer) {
    hop_xfer_rx_batch_abort_action action = hop_xfer_rx_batch_abort_decide(
        batch, rx->batch_state.active, rx->batch_state.batch, notify_peer);
    const hop_xfer_rx_abort_ops ops = {
        rx, rx_abort_send, rx_abort_cancel_clipboard, rx_abort_close_file,
        rx_abort_discard_staging, rx_abort_mark_dead, rx_abort_finish_task,
        rx_abort_clear_state, rx_abort_status_end
    };
    (void)hop_xfer_rx_abort_execute(action, batch, reason, notify_peer,
        (batch & HOP_XFER_PASTE_BATCH_FLAG) != 0, &ops);
}

static void rx_begin_batch(uint32_t batch) {
    uint8_t dead_reason = 0;
    int committed = hop_xfer_rx_done_receipt_find(
        &s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle), batch) != NULL;
    (void)hop_xfer_rx_lifecycle_dead_reason(&s_rx_lifecycle, batch, &dead_reason);
    hop_xfer_rx_batch_begin_action begin_action = hop_xfer_rx_batch_begin_decide(
        batch, committed, dead_reason, s_rx.batch_state.active, s_rx.batch_state.batch, &dead_reason);
    switch (begin_action) {
        case HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED:
            return; /* already committed in this session; never recreate its task */
        case HOP_RX_BATCH_BEGIN_REJECT_DEAD:
            /* A cancelled/aborted batch must never recreate its staging directory. */
            rx_terminate(&s_rx, batch, dead_reason, 1);
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
    if (!hop_xfer_rx_batch_state_begin(&s_rx.batch_state, batch)) {
        hop_log(HOP_LOG_ERROR, "xfer: receiver batch state rejected begin for %u", batch);
        hop_xfer_task_finish(&s_rx.task, HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle, batch, HOP_ABORT_DISK);
        send_abort_msg(s_x.client, batch, HOP_ABORT_DISK, HOP_ABORT_TO_SENDER);
        return;
    }
    hop_log(HOP_LOG_INFO,"xfer: receive batch %u task=%llu",
            batch,(unsigned long long)s_rx.task.id);
    if (s_rx.services.status_begin) s_rx.services.status_begin(s_rx.services.context,batch,0,0,"Incoming files",HOP_XFER_RECEIVING);

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        rx_terminate(&s_rx,batch,HOP_ABORT_DISK,1);
        return;
    }

    /* 手工拼接并显式检查长度 —— wsprintfW 没有边界检查，临时目录路径
     * 一旦接近 MAX_PATH 就会直接越界写。
     * 注意：swprintf 的 %s 要的是【窄】字符串（%ls 才是宽的），
     * 用 %s 传 L"HopDesk" 只会读到第一个字符 'H'。这里不碰格式化，直接拼。 */
    wchar_t sub[80];
    char session_hex[33]; hop_session_hex(&s_x.session, session_hex);
    wchar_t num[16];
    if (swprintf(num, 16, L"%u", batch) <= 0) { rx_terminate(&s_rx,batch,HOP_ABORT_DISK,1); return; }
    wcscpy(sub, XFER_RX_ROOT);
    wcscat(sub, L"\\");
    for (int i=0;i<32;i++) { size_t at=wcslen(sub); sub[at]=(wchar_t)session_hex[i];sub[at+1]=0; }
    wcscat(sub, L"\\");
    wcscat(sub, num);

    size_t sub_len = wcslen(sub);
    if ((size_t)n + sub_len + 1 > MAX_PATH) {
        hop_log(HOP_LOG_WARN, "xfer: temp path too long, batch %u dropped", batch);
        rx_terminate(&s_rx,batch,HOP_ABORT_DISK,1);
        return;
    }

    wcscpy(s_rx.dir, tmp);
    wcscat(s_rx.dir, sub);


    if (!hop_xfer_rx_fs_ensure_dirs(s_rx.dir) || !hop_xfer_rx_fs_create_directory_if_missing(s_rx.dir)) {
        rx_terminate(&s_rx,batch,HOP_ABORT_DISK,1);
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
    return hop_xfer_rx_finish_batch(&s_rx, batch, ok, total);
}
