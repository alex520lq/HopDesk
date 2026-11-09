static void rx_abort_send(void *context, uint32_t batch, uint8_t reason) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.send_abort)
        rx->services.send_abort(rx->services.context, batch, reason);
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

static void rx_begin_batch(xfer_rx_context *rx, uint32_t batch) {
    hop_xfer_rx_begin_batch(rx, batch);
}

static int rx_finish_batch(xfer_rx_context *rx, uint32_t batch, uint32_t ok, uint32_t total) {
    return hop_xfer_rx_finish_batch(rx, batch, ok, total);
}
