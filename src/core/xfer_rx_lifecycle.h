#ifndef HOP_XFER_RX_LIFECYCLE_H
#define HOP_XFER_RX_LIFECYCLE_H

#include "hop.h"

/* Receiver-thread-owned session and cancelled/aborted batch tombstone.
 * A session change invalidates the previous tombstone. No internal locking. */
typedef struct {
    hop_session_id session;
    uint32_t dead_batch;
    uint8_t dead_reason;
} hop_xfer_rx_lifecycle;

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
