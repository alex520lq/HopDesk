#include "xfer_sender_scheduler.h"

static int queue_ops_valid(const hop_xfer_sender_queue_ops *ops) {
    return ops && ops->next && ops->set_next && ops->is_cancelled &&
           ops->batch && ops->retry_at_ms && ops->give_up_at_ms;
}

static int scheduler_ops_valid(const hop_xfer_sender_scheduler_ops *ops) {
    return ops && ops->begin_pass && ops->is_stopping &&
           ops->wait_for_work_locked && ops->claim_job_locked &&
           ops->end_pass;
}

hop_xfer_sender_scheduler_result hop_xfer_sender_scheduler_wait_next(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *queue_ops,
    const hop_xfer_sender_scheduler_ops *scheduler_ops,
    uint32_t poll_interval_ms,
    void **job_out) {
    if (job_out) *job_out = 0;
    if (!queue || !job_out || !queue_ops_valid(queue_ops) ||
        !scheduler_ops_valid(scheduler_ops) || !poll_interval_ms)
        return HOP_XFER_SENDER_SCHEDULER_ERROR;

    for (;;) {
        int peer_ready = 0;
        uint64_t now_ms = 0;
        uint32_t cancel_batch = 0;
        if (!scheduler_ops->begin_pass(scheduler_ops->user, &peer_ready,
                                       &now_ms, &cancel_batch))
            return HOP_XFER_SENDER_SCHEDULER_ERROR;

        if (scheduler_ops->is_stopping(scheduler_ops->user)) {
            scheduler_ops->end_pass(scheduler_ops->user);
            return HOP_XFER_SENDER_SCHEDULER_STOPPED;
        }

        void *job = hop_xfer_sender_queue_take_cancelled(queue, queue_ops);
        hop_xfer_sender_scheduler_result result =
            job ? HOP_XFER_SENDER_SCHEDULER_EXPIRED
                : HOP_XFER_SENDER_SCHEDULER_ERROR;

        if (!job) {
            hop_xfer_sender_queue_take_result take_result =
                HOP_XFER_SENDER_QUEUE_NONE;
            job = hop_xfer_sender_queue_take_head(
                queue, queue_ops, now_ms, cancel_batch, peer_ready, 1,
                &take_result);
            if (take_result == HOP_XFER_SENDER_QUEUE_READY)
                result = HOP_XFER_SENDER_SCHEDULER_READY;
            else if (take_result == HOP_XFER_SENDER_QUEUE_EXPIRED)
                result = HOP_XFER_SENDER_SCHEDULER_EXPIRED;
        }

        if (job) {
            scheduler_ops->claim_job_locked(
                scheduler_ops->user, job,
                result == HOP_XFER_SENDER_SCHEDULER_EXPIRED);
            scheduler_ops->end_pass(scheduler_ops->user);
            *job_out = job;
            return result;
        }

        /* The adapter returns with the same queue lock held. The next pass
         * refreshes peer state after every wake/timeout before taking a Job. */
        if (!scheduler_ops->wait_for_work_locked(
                scheduler_ops->user, poll_interval_ms)) {
            scheduler_ops->end_pass(scheduler_ops->user);
            return HOP_XFER_SENDER_SCHEDULER_ERROR;
        }
        scheduler_ops->end_pass(scheduler_ops->user);
    }
}
