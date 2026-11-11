#include "xfer_rx_batch_rules.h"

hop_xfer_rx_batch_begin_action hop_xfer_rx_batch_begin_decide(
    uint32_t batch, int committed, uint8_t dead_reason,
    int active, uint32_t active_batch, uint8_t *reason) {
    if (reason) *reason = 0;
    if (!batch) return HOP_RX_BATCH_BEGIN_INVALID;
    if (committed) return HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED;
    if (dead_reason) {
        if (reason) *reason = dead_reason;
        return HOP_RX_BATCH_BEGIN_REJECT_DEAD;
    }
    if (active && active_batch == batch) return HOP_RX_BATCH_BEGIN_RESUME;
    return HOP_RX_BATCH_BEGIN_START;
}

hop_xfer_rx_batch_abort_action hop_xfer_rx_batch_abort_decide(
    uint32_t batch, int active, uint32_t active_batch, int notify_peer) {
    if (!batch) return HOP_RX_BATCH_ABORT_IGNORE;
    if (hop_xfer_rx_batch_is_active(batch, active, active_batch))
        return HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE;
    return notify_peer ? HOP_RX_BATCH_ABORT_REPLY_ONLY : HOP_RX_BATCH_ABORT_IGNORE;
}

int hop_xfer_rx_batch_is_active(uint32_t batch, int active, uint32_t active_batch) {
    return batch != 0 && active && active_batch == batch;
}

hop_xfer_rx_batch_finish_action hop_xfer_rx_batch_finish_decide(
    uint32_t batch, int active, uint32_t active_batch,
    int completion_claimed, uint32_t ok, uint32_t total,
    int is_image, int image_valid) {
    if (!hop_xfer_rx_batch_is_active(batch, active, active_batch))
        return HOP_RX_BATCH_FINISH_STALE;
    if (!completion_claimed) return HOP_RX_BATCH_FINISH_CANCELLED;
    if (ok == total) {
        if (!is_image || image_valid) return HOP_RX_BATCH_FINISH_COMPLETE;
        return HOP_RX_BATCH_FINISH_IMAGE_FAILED;
    }
    return is_image ? HOP_RX_BATCH_FINISH_IMAGE_FAILED : HOP_RX_BATCH_FINISH_PARTIAL;
}
