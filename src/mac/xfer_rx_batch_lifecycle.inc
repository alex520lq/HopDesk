static void rx_terminate(uint32_t batch, uint8_t reason, int notify_peer) {
    if (!batch) return;
    if (batch & HOP_XFER_PASTE_BATCH_FLAG)
        clipboard_complete_remote_paste(batch,NULL,0,0);
    if (notify_peer) { send_abort(batch,reason); }
    if (!hop_xfer_rx_batch_is_active(batch, s_rx.batch_state.active, s_rx.batch_state.batch)) return;
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
        batch, committed, dead_reason, s_rx.batch_state.active, s_rx.batch_state.batch, &dead_reason);
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
    status_begin(batch,HOP_XFER_DIR_RECEIVE,0,0,"Incoming files",HOP_XFER_RECEIVING);

    char session_hex[33]; hop_session_hex(&s_x.session, session_hex);
    snprintf(s_rx.dir, sizeof(s_rx.dir), "%s/HopDesk/%s/%u", temp_root(), session_hex, batch);
    if (mkdir_p(s_rx.dir) != 0) {
        rx_terminate(batch,HOP_ABORT_DISK,1);
        return;
    }
}

static int rx_finish_batch(uint32_t batch, uint32_t ok, uint32_t total) {
    if (!hop_xfer_rx_batch_is_active(batch, s_rx.batch_state.active, s_rx.batch_state.batch)) {
        hop_log(HOP_LOG_WARN, "xfer: stale DONE for batch %u ignored (active=%u)",
                batch, s_rx.batch_state.active ? s_rx.batch_state.batch : 0);
        return -1;
    }
    /* Completion and per-task cancellation serialize before clipboard effects. */
    int completion_claimed = hop_xfer_task_begin_completion(&s_rx.task);
    if (!completion_claimed) {
        hop_xfer_rx_batch_finish_action cancelled = hop_xfer_rx_batch_finish_decide(
            batch, s_rx.batch_state.active, s_rx.batch_state.batch, 0, ok, total, s_rx.file_state.is_image, 0);
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
    } else if (s_rx.file_state.is_image) {
        if (s_rx.image_buffer.data && s_rx.image_state.received == s_rx.image_state.expected) {
            uint64_t clipboard_started_ns=rate_now_ns();
            clipboard_apply_image(s_rx.image_buffer.data, s_rx.image_state.width, s_rx.image_state.height, s_rx.image_state.stride);
            status_stage_time(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                              rate_now_ns()-clipboard_started_ns);
        } else
            hop_log(HOP_LOG_ERROR, "xfer: incomplete image rejected (%llu/%llu bytes)",
                    (unsigned long long)s_rx.image_state.received, (unsigned long long)s_rx.image_state.expected);
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
    if(s_rx.file_state.is_image && ok==total && s_rx.image_state.received==s_rx.image_state.expected)
        status_file_phase(HOP_XFER_DIR_RECEIVE,batch,0,HOP_XFER_FILE_COMPLETED,1);
    int image_valid = s_rx.image_buffer.data && s_rx.image_state.received == s_rx.image_state.expected;
    hop_xfer_rx_batch_finish_action finish_action = hop_xfer_rx_batch_finish_decide(
        batch, s_rx.batch_state.active, s_rx.batch_state.batch, 1, ok, total, s_rx.file_state.is_image, image_valid);
    int complete = finish_action == HOP_RX_BATCH_FINISH_COMPLETE;
    hop_xfer_task_finish(&s_rx.task,complete?HOP_XFER_TASK_COMPLETED:(s_rx.file_state.is_image?HOP_XFER_TASK_FAILED:HOP_XFER_TASK_PARTIAL));
    status_end(HOP_XFER_DIR_RECEIVE,batch,complete?HOP_XFER_COMPLETED:
               (s_rx.file_state.is_image?HOP_XFER_FAILED:HOP_XFER_PARTIAL),
               complete?"Receive completed":"Receive incomplete; clipboard not updated");
    int result=finish_action==HOP_RX_BATCH_FINISH_COMPLETE?HOP_DONE_ACK_COMPLETE:
               (finish_action==HOP_RX_BATCH_FINISH_IMAGE_FAILED?HOP_DONE_ACK_FAILED:HOP_DONE_ACK_PARTIAL);
    rx_reset();
    return result;
}
