#include "xfer_rx_lifecycle.h"
#include <string.h>

void hop_xfer_rx_lifecycle_init(hop_xfer_rx_lifecycle *lifecycle) {
    if (lifecycle) memset(lifecycle, 0, sizeof(*lifecycle));
}

const hop_session_id *hop_xfer_rx_lifecycle_session(
    const hop_xfer_rx_lifecycle *lifecycle) {
    return lifecycle ? &lifecycle->session : NULL;
}

int hop_xfer_rx_lifecycle_bind_session(
    hop_xfer_rx_lifecycle *lifecycle, const hop_session_id *session) {
    if (!lifecycle || !session) return 0;
    if (hop_session_id_equal(&lifecycle->session, session)) return 0;
    lifecycle->session = *session;
    lifecycle->dead_batch = 0;
    lifecycle->dead_reason = 0;
    return 1;
}

void hop_xfer_rx_lifecycle_mark_dead(
    hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t reason) {
    if (!lifecycle || !batch) return;
    lifecycle->dead_batch = batch;
    lifecycle->dead_reason = reason;
}

int hop_xfer_rx_lifecycle_dead_reason(
    const hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t *reason) {
    if (!lifecycle || !batch || lifecycle->dead_batch != batch) return 0;
    if (reason) *reason = lifecycle->dead_reason;
    return 1;
}

hop_xfer_rx_abort_result hop_xfer_rx_abort_execute(
    hop_xfer_rx_batch_abort_action action, uint32_t batch, uint8_t reason,
    int notify_peer, int is_paste, const hop_xfer_rx_abort_ops *ops) {
    if (!batch || !ops || action == HOP_RX_BATCH_ABORT_IGNORE)
        return HOP_RX_ABORT_IGNORED;
    if (action == HOP_RX_BATCH_ABORT_REPLY_ONLY) {
        if (ops->send_abort) ops->send_abort(ops->context, batch, reason);
        return HOP_RX_ABORT_REPLIED;
    }
    if (action != HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE)
        return HOP_RX_ABORT_IGNORED;

    if (is_paste && ops->cancel_clipboard)
        ops->cancel_clipboard(ops->context, batch);
    if (notify_peer && ops->send_abort)
        ops->send_abort(ops->context, batch, reason);
    if (ops->release_resources) ops->release_resources(ops->context, 1);
    if (ops->mark_dead) ops->mark_dead(ops->context, batch, reason);
    if (ops->finish_task)
        ops->finish_task(ops->context, reason == HOP_ABORT_CANCEL);
    if (ops->status_end)
        ops->status_end(ops->context, batch, reason == HOP_ABORT_CANCEL);
    return HOP_RX_ABORT_TERMINATED;
}
