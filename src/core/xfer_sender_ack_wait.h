#ifndef HOP_XFER_SENDER_ACK_WAIT_H
#define HOP_XFER_SENDER_ACK_WAIT_H

#include "xfer_sender_context.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOP_XFER_SENDER_ACK_WAIT_TIMEOUT = -1,
    HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD = -2,
    HOP_XFER_SENDER_ACK_WAIT_LOCAL_IO = -3,
    HOP_XFER_SENDER_ACK_WAIT_CANCELLED = -4,
    HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT = -5
};

/* Shared file-ACK and DONE-ACK wait policy.
 * poll_interval_ms == 0 waits up to the remaining deadline; a nonzero value
 * preserves a platform's fixed polling slice (macOS uses 100 ms). */
int hop_xfer_sender_ack_wait(const hop_xfer_sender_context *context,
                             intptr_t socket_handle, uint64_t epoch,
                             uint32_t batch, uint64_t task_id, uint32_t id,
                             uint64_t deadline_ns, uint32_t poll_interval_ms,
                             uint64_t *rtt_us);

#ifdef __cplusplus
}
#endif
#endif
