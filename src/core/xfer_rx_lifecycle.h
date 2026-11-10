#ifndef HOP_XFER_RX_LIFECYCLE_H
#define HOP_XFER_RX_LIFECYCLE_H

#include "hop.h"
#include "xfer_rx_batch_rules.h"

/* Receiver-thread-owned session and cancelled/aborted batch tombstone.
 * A session change invalidates the previous tombstone. No internal locking. */
typedef struct {
    hop_session_id session;
    uint32_t dead_batch;
    uint8_t dead_reason;
} hop_xfer_rx_lifecycle;

typedef struct {
    void *context;
    void (*send_abort)(void *context, uint32_t batch, uint8_t reason);
    void (*cancel_clipboard)(void *context, uint32_t batch);
    void (*release_resources)(void *context, int discard_staging);
    void (*mark_dead)(void *context, uint32_t batch, uint8_t reason);
    void (*finish_task)(void *context, int cancelled);
    void (*status_end)(void *context, uint32_t batch, int cancelled);
} hop_xfer_rx_abort_ops;

typedef enum {
    HOP_RX_ABORT_IGNORED = 0,
    HOP_RX_ABORT_REPLIED,
    HOP_RX_ABORT_TERMINATED
} hop_xfer_rx_abort_result;

/* Shared ordered ABORT side effects. Stale/reply-only actions cannot touch
 * clipboard, writer, staging paths or task state. */
hop_xfer_rx_abort_result hop_xfer_rx_abort_execute(
    hop_xfer_rx_batch_abort_action action, uint32_t batch, uint8_t reason,
    int notify_peer, int is_paste, const hop_xfer_rx_abort_ops *ops);

void hop_xfer_rx_lifecycle_init(hop_xfer_rx_lifecycle *lifecycle);
const hop_session_id *hop_xfer_rx_lifecycle_session(
    const hop_xfer_rx_lifecycle *lifecycle);
/* Returns nonzero when the bound session changes; a change clears its tombstone. */
int hop_xfer_rx_lifecycle_bind_session(
    hop_xfer_rx_lifecycle *lifecycle, const hop_session_id *session);
void hop_xfer_rx_lifecycle_mark_dead(
    hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t reason);
int hop_xfer_rx_lifecycle_dead_reason(
    const hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t *reason);

#endif
