#include "xfer_sender_active_job.h"

int hop_xfer_sender_active_job_is_active(
    const hop_xfer_sender_active_job *active) {
    return active && active->batch != 0;
}

int hop_xfer_sender_active_job_matches(
    const hop_xfer_sender_active_job *active, uint32_t batch) {
    return active && batch && active->batch == batch;
}

uint32_t hop_xfer_sender_active_job_cancel_target(
    const hop_xfer_sender_active_job *active, uint32_t queued_head_batch) {
    return active && active->batch ? active->batch : queued_head_batch;
}

int hop_xfer_sender_active_job_begin(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id) {
    if (!active || !batch || !task_id) return 0;
    active->batch = batch;
    active->task_id = task_id;
    active->abort_batch = 0;
    active->abort_reason = 0;
    return 1;
}

void hop_xfer_sender_active_job_reset_peer_abort(
    hop_xfer_sender_active_job *active) {
    if (!active) return;
    active->abort_batch = 0;
    active->abort_reason = 0;
}

int hop_xfer_sender_active_job_record_peer_abort(
    hop_xfer_sender_active_job *active, uint32_t batch, uint8_t reason) {
    if (!active || !batch || active->batch != batch) return 0;
    active->abort_batch = batch;
    active->abort_reason = reason;
    return 1;
}

int hop_xfer_sender_active_job_clear_if_matches(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id) {
    if (!active || !batch || !task_id || active->batch != batch ||
        active->task_id != task_id)
        return 0;
    active->batch = 0;
    active->task_id = 0;
    return 1;
}

int hop_xfer_sender_active_job_take_cancelled_and_clear(
    hop_xfer_sender_active_job *active, uint32_t batch, uint64_t task_id,
    uint8_t cancel_reason) {
    if (!active || !batch || !task_id || active->batch != batch ||
        active->task_id != task_id)
        return 0;
    int cancelled = active->abort_batch == batch &&
                    active->abort_reason == cancel_reason;
    active->batch = 0;
    active->task_id = 0;
    return cancelled;
}
