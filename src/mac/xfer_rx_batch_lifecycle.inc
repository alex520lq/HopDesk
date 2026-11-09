static void rx_abort_send(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    send_abort(batch,reason);
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
    if (!hop_xfer_rx_batch_state_begin(&s_rx.batch_state, batch)) {
        hop_log(HOP_LOG_ERROR, "xfer: receiver batch state rejected begin for %u", batch);
        hop_xfer_task_finish(&s_rx.task, HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&s_rx.task);
        hop_xfer_rx_lifecycle_mark_dead(&s_rx_lifecycle, batch, HOP_ABORT_DISK);
        send_abort(batch, HOP_ABORT_DISK);
        return;
    }
    hop_log(HOP_LOG_INFO,"xfer: receive batch %u task=%llu",
            batch,(unsigned long long)s_rx.task.id);
    if (s_rx.services.status_begin) s_rx.services.status_begin(s_rx.services.context,batch,0,0,"Incoming files",HOP_XFER_RECEIVING);

    char session_hex[33]; hop_session_hex(&s_x.session, session_hex);
    snprintf(s_rx.dir, sizeof(s_rx.dir), "%s/HopDesk/%s/%u", temp_root(), session_hex, batch);
    if (hop_xfer_rx_fs_mkdir_p(s_rx.dir) != 0) {
        rx_terminate(&s_rx,batch,HOP_ABORT_DISK,1);
        return;
    }
}

static int rx_finish_batch(uint32_t batch, uint32_t ok, uint32_t total) {
    return hop_xfer_rx_finish_batch(&s_rx, batch, ok, total);
}
