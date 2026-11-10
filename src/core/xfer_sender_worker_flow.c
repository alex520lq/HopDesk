#include "xfer_sender_worker_flow.h"
#include "xfer_sender_lifecycle.h"
#include "xfer_sender_retry.h"

static int worker_ops_valid(const hop_xfer_sender_worker_ops *ops) {
    return ops && ops->max_attempts && ops->reconnect_budget_ms &&
           ops->now_ms && ops->retry_delay_ms && ops->is_cancelled &&
           ops->begin_completion && ops->set_queued &&
           ops->store_retry_state && ops->requeue_front &&
           ops->peer_cancelled_and_clear_current && ops->send_abort &&
           ops->send_done && ops->task_finish && ops->status_end &&
           ops->dispose_job;
}

static void worker_finish_failed(const hop_xfer_sender_worker_ops *ops,
                                 const hop_xfer_sender_worker_job *job,
                                 const char *detail) {
    ops->status_end(ops->user, job->batch, HOP_XFER_FAILED, detail);
    ops->task_finish(ops->user, HOP_XFER_TASK_FAILED);
    ops->dispose_job(ops->user);
}

static void worker_log(const hop_xfer_sender_worker_ops *ops,
                       hop_xfer_sender_worker_log_event event,
                       const hop_xfer_sender_worker_job *job,
                       unsigned retry_delay_ms) {
    if (ops->log_event)
        ops->log_event(ops->user, event, job, retry_delay_ms);
}

hop_xfer_sender_worker_action hop_xfer_sender_worker_finish_job(
    const hop_xfer_sender_worker_ops *ops,
    hop_xfer_sender_worker_job *job) {
    if (!worker_ops_valid(ops) || !job || !job->task_id ||
        job->entry_count < 0 || job->total_count < 0 || job->ok_count < 0)
        return HOP_XFER_SENDER_WORKER_INVALID;

    hop_xfer_sender_job_result result = job->result;
    if (result != HOP_XFER_SENDER_JOB_DONE &&
        ops->is_cancelled(ops->user, job->batch, job->task_id))
        result = HOP_XFER_SENDER_JOB_CANCELLED;

    if (result == HOP_XFER_SENDER_JOB_DONE &&
        !ops->begin_completion(ops->user, job->task_id))
        result = HOP_XFER_SENDER_JOB_CANCELLED;

    if (result == HOP_XFER_SENDER_JOB_NET_DOWN) {
        uint32_t next_attempt = job->attempt == UINT32_MAX
            ? UINT32_MAX : job->attempt + 1u;
        unsigned delay = next_attempt < ops->max_attempts
            ? ops->retry_delay_ms(ops->user, next_attempt) : 0u;
        hop_xfer_sender_retry_plan plan;
        hop_xfer_sender_retry_result retry = hop_xfer_sender_plan_retry(
            ops->now_ms(ops->user), job->give_up_at_ms, job->attempt,
            ops->max_attempts, ops->reconnect_budget_ms, delay, &plan);

        if (retry == HOP_XFER_SENDER_RETRY_INVALID) {
            worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_RETRY_INVALID, job, 0);
            worker_finish_failed(ops, job, "Invalid retry policy");
            return HOP_XFER_SENDER_WORKER_FINISHED;
        }

        job->attempt = plan.attempt;
        job->give_up_at_ms = plan.give_up_at_ms;
        if (retry == HOP_XFER_SENDER_RETRY_SCHEDULED) {
            job->retry_at_ms = plan.retry_at_ms;
            ops->store_retry_state(ops->user, job);
            worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_RETRY_SCHEDULED,
                       job, delay);
            if (!ops->set_queued(ops->user, job->task_id)) {
                worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_RETRY_STATE_FAILED,
                           job, delay);
                worker_finish_failed(ops, job, "Invalid task retry state");
                return HOP_XFER_SENDER_WORKER_FINISHED;
            }
            if (!ops->requeue_front(ops->user)) {
                worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_REQUEUE_FAILED,
                           job, delay);
                worker_finish_failed(ops, job, "Retry queue admission failed");
                return HOP_XFER_SENDER_WORKER_FINISHED;
            }
            return HOP_XFER_SENDER_WORKER_REQUEUED;
        }

        /* Retain the final attempt/deadline values even when retry is exhausted. */
        ops->store_retry_state(ops->user, job);
        worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_RETRY_EXHAUSTED, job, 0);
    } else if (result == HOP_XFER_SENDER_JOB_CANCELLED) {
        ops->send_abort(ops->user, job->batch);
    } else if (result == HOP_XFER_SENDER_JOB_DONE) {
        if (job->ok_count < job->entry_count)
            worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_PARTIAL_DONE, job, 0);
        result = ops->send_done(ops->user);
    }

    if (job->completion_path) {
        job->peer_cancelled = ops->peer_cancelled_and_clear_current(
            ops->user, job->batch, job->task_id);
    }

    hop_xfer_sender_terminal_decision terminal;
    if (hop_xfer_sender_get_terminal_decision(
            result, job->peer_cancelled,
            job->ok_count == job->total_count, &terminal)) {
        ops->task_finish(ops->user, terminal.task_state);
        ops->status_end(ops->user, job->batch,
                        terminal.status_phase, terminal.detail);
    } else {
        ops->task_finish(ops->user, HOP_XFER_TASK_FAILED);
        ops->status_end(ops->user, job->batch,
                        HOP_XFER_FAILED, "Unknown sender result");
    }
    ops->dispose_job(ops->user);
    return HOP_XFER_SENDER_WORKER_FINISHED;
}
