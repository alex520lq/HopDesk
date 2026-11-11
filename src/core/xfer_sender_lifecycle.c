#include "xfer_sender_lifecycle.h"

#include <stddef.h>

/* Keep Task terminal state and transfer status publication decisions identical
 * across platform workers; platform code only performs the returned effects. */

int hop_xfer_sender_get_terminal_decision(
    hop_xfer_sender_job_result result,
    int peer_cancelled,
    int complete,
    hop_xfer_sender_terminal_decision *out) {
    if (!out) return 0;

    switch (result) {
        case HOP_XFER_SENDER_JOB_DONE:
            out->task_state = complete ? HOP_XFER_TASK_COMPLETED : HOP_XFER_TASK_PARTIAL;
            out->status_phase = complete ? HOP_XFER_COMPLETED : HOP_XFER_PARTIAL;
            out->detail = complete ? "Transfer completed" : "Some files failed";
            return 1;

        case HOP_XFER_SENDER_JOB_CANCELLED:
            out->task_state = HOP_XFER_TASK_CANCELLED;
            out->status_phase = HOP_XFER_CANCELLED;
            out->detail = "Cancelled";
            return 1;

        case HOP_XFER_SENDER_JOB_PEER_ABORT:
            out->task_state = peer_cancelled ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED;
            out->status_phase = peer_cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED;
            out->detail = peer_cancelled ? "Cancelled by receiver" : "Peer aborted transfer";
            return 1;

        case HOP_XFER_SENDER_JOB_COMMIT_REJECTED:
            out->task_state = HOP_XFER_TASK_FAILED;
            out->status_phase = HOP_XFER_FAILED;
            out->detail = "Receiver rejected completion";
            return 1;

        case HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN:
            out->task_state = HOP_XFER_TASK_INTERRUPTED;
            out->status_phase = HOP_XFER_FAILED;
            out->detail = "Completion delivery uncertain; peer may have received it";
            return 1;

        case HOP_XFER_SENDER_JOB_NET_DOWN:
            out->task_state = HOP_XFER_TASK_FAILED;
            out->status_phase = HOP_XFER_FAILED;
            out->detail = "Reconnect attempts exhausted";
            return 1;

        case HOP_XFER_SENDER_JOB_SHUTDOWN:
            out->task_state = HOP_XFER_TASK_INTERRUPTED;
            out->status_phase = HOP_XFER_FAILED;
            out->detail = "Transfer interrupted during shutdown";
            return 1;

        default:
            return 0;
    }
}
