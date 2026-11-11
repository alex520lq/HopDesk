#include "xfer_sender_ack_wait.h"
#include "xfer_sender_context_adapter.h"

#include <stdint.h>

int hop_xfer_sender_ack_wait(const hop_xfer_sender_context *context,
                             intptr_t socket_handle, uint64_t epoch,
                             uint32_t batch, uint64_t task_id, uint32_t id,
                             uint64_t deadline_ns, uint32_t poll_interval_ms,
                             uint64_t *rtt_us) {
    if (!context || !context->ack_user || !context->ack_wait_take ||
        !context->ack_unregister || !context->now_ns)
        return HOP_XFER_SENDER_ACK_WAIT_LOCAL_IO;
    if (rtt_us) *rtt_us = 0;

    for (;;) {
        uint64_t now = context->now_ns(context->user);
        if (now >= deadline_ns) {
            context->ack_unregister(context->ack_user, batch, id);
            return HOP_XFER_SENDER_ACK_WAIT_TIMEOUT;
        }

        uint32_t wait_ms = poll_interval_ms;
        if (!wait_ms) {
            uint64_t remaining = deadline_ns - now;
            uint64_t rounded = remaining / 1000000ull +
                               (remaining % 1000000ull != 0);
            if (!rounded) rounded = 1;
            if (rounded > (uint64_t)(UINT32_MAX - 1u))
                rounded = (uint64_t)(UINT32_MAX - 1u);
            wait_ms = (uint32_t)rounded;
        }

        int status = 0, deadline_expired = 0;
        uint64_t rtt_ns = 0;
        int hit = context->ack_wait_take(context->ack_user, batch, id,
                                         wait_ms, &status, &rtt_ns,
                                         &deadline_expired);
        if (hit) {
            if (deadline_expired)
                return HOP_XFER_SENDER_ACK_WAIT_TIMEOUT;
            if (rtt_us) *rtt_us = rtt_ns / 1000ull;
            return status;
        }

        if (context->peer_aborted &&
            context->peer_aborted(context->user, batch)) {
            context->ack_unregister(context->ack_user, batch, id);
            return HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT;
        }
        if (hop_xfer_sender_context_job_cancel_requested(context, batch, task_id)) {
            context->ack_unregister(context->ack_user, batch, id);
            return HOP_XFER_SENDER_ACK_WAIT_CANCELLED;
        }
        if (!context->is_stopping || context->is_stopping(context->user) ||
            socket_handle == (intptr_t)-1 || !context->session_current ||
            !context->session_current(context->user, socket_handle, epoch)) {
            context->ack_unregister(context->ack_user, batch, id);
            return HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD;
        }
    }
}
