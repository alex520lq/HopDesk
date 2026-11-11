#ifndef HOP_XFER_SENDER_ACTIVE_JOB_H
#define HOP_XFER_SENDER_ACTIVE_JOB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lock-free Sender active-job/peer-ABORT state. The platform adapter must hold
 * the same native lock used by its queue whenever this state is shared with
 * the receiver, cancellation UI or worker thread.
 */
typedef struct {
    uint32_t batch;
    uint64_t task_id;
    uint32_t abort_batch;
    uint8_t abort_reason;
} hop_xfer_sender_active_job;

int hop_xfer_sender_active_job_is_active(
    const hop_xfer_sender_active_job *active);
int hop_xfer_sender_active_job_matches(
    const hop_xfer_sender_active_job *active, uint32_t batch);
uint32_t hop_xfer_sender_active_job_cancel_target(
    const hop_xfer_sender_active_job *active, uint32_t queued_head_batch);
int hop_xfer_sender_active_job_begin(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id);
void hop_xfer_sender_active_job_reset_peer_abort(
    hop_xfer_sender_active_job *active);
int hop_xfer_sender_active_job_record_peer_abort(
    hop_xfer_sender_active_job *active, uint32_t batch, uint8_t reason);
int hop_xfer_sender_active_job_take_cancelled_and_clear(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id,
    uint8_t cancel_reason);
int hop_xfer_sender_active_job_clear_if_matches(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id);

#ifdef __cplusplus
}
#endif
#endif
