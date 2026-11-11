#ifndef HOP_XFER_SENDER_LIFECYCLE_H
#define HOP_XFER_SENDER_LIFECYCLE_H

#include "xfer_sender_engine.h"
#include "xfer_task.h"
#include "xfer_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hop_xfer_task_state task_state;
    int status_phase;
    const char *detail;
} hop_xfer_sender_terminal_decision;

/* Maps a completed sender job outcome to the one terminal Task/status result.
 * peer_cancelled only distinguishes a receiver-requested cancellation from a
 * peer failure; complete only distinguishes DONE from PARTIAL. */
int hop_xfer_sender_get_terminal_decision(
    hop_xfer_sender_job_result result,
    int peer_cancelled,
    int complete,
    hop_xfer_sender_terminal_decision *out);

#ifdef __cplusplus
}
#endif
#endif
