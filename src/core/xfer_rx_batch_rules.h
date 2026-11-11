#ifndef HOP_XFER_RX_BATCH_RULES_H
#define HOP_XFER_RX_BATCH_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOP_RX_BATCH_BEGIN_INVALID = 0,
    HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED,
    HOP_RX_BATCH_BEGIN_REJECT_DEAD,
    HOP_RX_BATCH_BEGIN_RESUME,
    HOP_RX_BATCH_BEGIN_START
} hop_xfer_rx_batch_begin_action;

typedef enum {
    HOP_RX_BATCH_ABORT_IGNORE = 0,
    HOP_RX_BATCH_ABORT_REPLY_ONLY,
    HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE
} hop_xfer_rx_batch_abort_action;

typedef enum {
    HOP_RX_BATCH_FINISH_STALE = 0,
    HOP_RX_BATCH_FINISH_CANCELLED,
    HOP_RX_BATCH_FINISH_COMPLETE,
    HOP_RX_BATCH_FINISH_PARTIAL,
    HOP_RX_BATCH_FINISH_IMAGE_FAILED
} hop_xfer_rx_batch_finish_action;

/* Pure shared policy: precedence is committed receipt, tombstone, active
 * duplicate, then a new batch. dead_reason is zero when no tombstone matches. */
hop_xfer_rx_batch_begin_action hop_xfer_rx_batch_begin_decide(
    uint32_t batch, int committed, uint8_t dead_reason,
    int active, uint32_t active_batch, uint8_t *reason);

/* Stale ABORTs may be re-acknowledged to the peer, but must never touch
 * clipboard, writer, staging paths or task state belonging to another batch. */
hop_xfer_rx_batch_abort_action hop_xfer_rx_batch_abort_decide(
    uint32_t batch, int active, uint32_t active_batch, int notify_peer);

/* True only when a terminal event belongs to the currently active batch. */
int hop_xfer_rx_batch_is_active(uint32_t batch, int active, uint32_t active_batch);

/* Pure shared policy after the caller has atomically claimed completion against
 * cancellation. Image batches require a complete decoded image to succeed. */
hop_xfer_rx_batch_finish_action hop_xfer_rx_batch_finish_decide(
    uint32_t batch, int active, uint32_t active_batch,
    int completion_claimed, uint32_t ok, uint32_t total,
    int is_image, int image_valid);

#ifdef __cplusplus
}
#endif
#endif
