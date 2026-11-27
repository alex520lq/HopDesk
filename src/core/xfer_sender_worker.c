#include "xfer_sender_worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Intrusive Sender Queue ---- */

static int queue_ops_valid(const hop_xfer_sender_queue_ops *ops) {
    return ops && ops->next && ops->set_next && ops->is_cancelled &&
           ops->batch && ops->retry_at_ms && ops->give_up_at_ms;
}

static void queue_remove(hop_xfer_sender_queue *queue,
                         const hop_xfer_sender_queue_ops *ops,
                         void *previous, void *node, void *next) {
    if (previous) ops->set_next(ops->user, previous, next);
    else queue->head = next;

    if (queue->tail == node) queue->tail = previous;
    ops->set_next(ops->user, node, NULL);
}

void hop_xfer_sender_queue_init(hop_xfer_sender_queue *queue) {
    if (queue) {
        queue->head = NULL;
        queue->tail = NULL;
        queue->count = 0;
    }
}

int hop_xfer_sender_queue_push_back(hop_xfer_sender_queue *queue,
                                    const hop_xfer_sender_queue_ops *ops,
                                    void *node) {
    if (!queue || !node || !queue_ops_valid(ops) || queue->count == UINT32_MAX)
        return 0;
    ops->set_next(ops->user, node, NULL);
    if (queue->tail) ops->set_next(ops->user, queue->tail, node);
    else queue->head = node;
    queue->tail = node;
    queue->count++;
    return 1;
}

int hop_xfer_sender_queue_push_front(hop_xfer_sender_queue *queue,
                                     const hop_xfer_sender_queue_ops *ops,
                                     void *node) {
    if (!queue || !node || !queue_ops_valid(ops) || queue->count == UINT32_MAX)
        return 0;
    ops->set_next(ops->user, node, queue->head);
    queue->head = node;
    if (!queue->tail) queue->tail = node;
    queue->count++;
    return 1;
}

int hop_xfer_sender_queue_contains_batch(const hop_xfer_sender_queue *queue,
                                         const hop_xfer_sender_queue_ops *ops,
                                         uint32_t batch) {
    if (!queue || !batch || !queue_ops_valid(ops)) return 0;
    for (void *node = queue->head; node; node = ops->next(ops->user, node))
        if (ops->batch(ops->user, node) == batch) return 1;
    return 0;
}

uint32_t hop_xfer_sender_queue_count(const hop_xfer_sender_queue *queue) {
    return queue ? queue->count : 0;
}

int hop_xfer_sender_queue_empty(const hop_xfer_sender_queue *queue) {
    return !queue || queue->head == NULL;
}

void *hop_xfer_sender_queue_detach_all(hop_xfer_sender_queue *queue) {
    if (!queue) return NULL;
    void *head = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    return head;
}

void *hop_xfer_sender_queue_take_cancelled(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops) {
    if (!queue || !queue_ops_valid(ops)) return NULL;

    void *previous = NULL;
    void *node = queue->head;
    while (node) {
        void *next = ops->next(ops->user, node);
        if (ops->is_cancelled(ops->user, node)) {
            queue_remove(queue, ops, previous, node, next);
            if (queue->count) queue->count--;
            return node;
        }
        previous = node;
        node = next;
    }
    return NULL;
}

void *hop_xfer_sender_queue_take_head(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops,
    uint64_t now_ms,
    uint32_t cancel_batch,
    int peer_ready,
    int allow_ready,
    hop_xfer_sender_queue_take_result *result) {
    if (result) *result = HOP_XFER_SENDER_QUEUE_NONE;
    if (!queue || !result || !queue_ops_valid(ops)) return NULL;

    void *node = queue->head;
    if (!node) return NULL;

    uint64_t give_up_at = ops->give_up_at_ms(ops->user, node);
    int expired = (cancel_batch != 0 &&
                   ops->batch(ops->user, node) == cancel_batch) ||
                  (give_up_at != 0 && now_ms >= give_up_at);
    if (expired) {
        void *next = ops->next(ops->user, node);
        queue_remove(queue, ops, NULL, node, next);
        if (queue->count) queue->count--;
        *result = HOP_XFER_SENDER_QUEUE_EXPIRED;
        return node;
    }

    if (allow_ready && peer_ready &&
        ops->retry_at_ms(ops->user, node) <= now_ms) {
        void *next = ops->next(ops->user, node);
        queue_remove(queue, ops, NULL, node, next);
        if (queue->count) queue->count--;
        *result = HOP_XFER_SENDER_QUEUE_READY;
        return node;
    }

    return NULL;
}

/* ---- Sender Scheduler ---- */

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

        if (!scheduler_ops->wait_for_work_locked(
                scheduler_ops->user, poll_interval_ms)) {
            scheduler_ops->end_pass(scheduler_ops->user);
            return HOP_XFER_SENDER_SCHEDULER_ERROR;
        }
        scheduler_ops->end_pass(scheduler_ops->user);
    }
}

/* ---- Active Job & Peer-ABORT State ---- */

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

/* ---- Retry & Terminal Lifecycle Policies ---- */

static uint64_t retry_add_saturating(uint64_t value, uint64_t amount) {
    return amount > UINT64_MAX - value ? UINT64_MAX : value + amount;
}

hop_xfer_sender_retry_result hop_xfer_sender_plan_retry(
    uint64_t now_ms,
    uint64_t current_give_up_at_ms,
    uint32_t current_attempt,
    uint32_t max_attempts,
    uint64_t reconnect_budget_ms,
    unsigned retry_delay_ms,
    hop_xfer_sender_retry_plan *out) {
    if (!out || !max_attempts) return HOP_XFER_SENDER_RETRY_INVALID;

    out->attempt = current_attempt == UINT32_MAX
        ? UINT32_MAX : current_attempt + 1u;
    out->give_up_at_ms = current_give_up_at_ms
        ? current_give_up_at_ms
        : retry_add_saturating(now_ms, reconnect_budget_ms);
    out->retry_at_ms = 0;

    if (out->attempt < max_attempts && now_ms < out->give_up_at_ms) {
        out->retry_at_ms = retry_add_saturating(now_ms, retry_delay_ms);
        return HOP_XFER_SENDER_RETRY_SCHEDULED;
    }
    return HOP_XFER_SENDER_RETRY_EXHAUSTED;
}

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

/* ---- Job Ownership & Release Lifecycle ---- */

static int job_release_valid(const hop_xfer_sender_job_release_view *v,
                             const hop_xfer_sender_job_release_ops *o) {
    return v && v->job && v->context && v->task && v->pixels &&
           v->image_payload_released && v->pending_finish_state &&
           v->pending_finish_valid && v->pending_status_end &&
           v->pending_status_phase && v->pending_status_detail && o &&
           o->claim && o->defer_to_shutdown && o->mark_released &&
           o->free_storage;
}

static void job_release_log(const hop_xfer_sender_job_release_view *v,
                            const char *msg) {
    hop_xfer_sender_context_logf(v->context, HOP_LOG_ERROR,
        "xfer: %s (batch=%u task=%llu)", msg, v->batch,
        (unsigned long long)v->task->id);
}

static hop_xfer_sender_job_release_result job_release_defer(
    const hop_xfer_sender_job_release_view *v,
    const hop_xfer_sender_job_release_ops *o,
    hop_xfer_sender_job_owner owner, const char *msg) {
    o->defer_to_shutdown(o->user, v->job, owner);
    job_release_log(v, msg);
    return HOP_XFER_SENDER_JOB_RELEASE_DEFERRED;
}

static int job_release_resources(const hop_xfer_sender_job_release_view *v) {
    if (*v->pixels) {
        free(*v->pixels);
        *v->pixels = NULL;
        *v->image_payload_released = 1;
    } else if (v->is_image && !*v->image_payload_released) {
        job_release_log(v, "image Job lost its pixel owner before Reservation release");
        return 0;
    }
    if (v->task->initialized) {
        uint64_t bytes = hop_xfer_task_image_temp_bytes(v->task);
        if (bytes && !*v->image_payload_released) {
            job_release_log(v, "image-temp Reservation has no owned pixel buffer");
            return 0;
        }
        if (bytes && !hop_xfer_task_release_image_temp(v->task, bytes)) {
            job_release_log(v, "Job image-temp Reservation release deferred");
            return 0;
        }
    } else if (v->task->id) {
        return 0;
    }
    return 1;
}

hop_xfer_sender_job_release_result hop_xfer_sender_job_release(
    const hop_xfer_sender_job_release_view *v,
    const hop_xfer_sender_job_release_ops *o,
    hop_xfer_sender_job_owner expected) {
    if (!v || !v->job) return HOP_XFER_SENDER_JOB_RELEASED;
    if (!job_release_valid(v, o)) return HOP_XFER_SENDER_JOB_RELEASE_INVALID;
    hop_xfer_sender_job_owner actual = expected;
    if (!o->claim(o->user, v->job, expected, &actual)) {
        hop_xfer_sender_context_logf(v->context, HOP_LOG_ERROR,
            "xfer: refusing Job release by non-owner (batch=%u owner=%d expected=%d)",
            v->batch, (int)actual, (int)expected);
        return HOP_XFER_SENDER_JOB_RELEASE_NOT_OWNER;
    }
    if ((v->task->initialized || v->task->id) &&
        expected == HOP_XFER_SENDER_JOB_OWNER_SUBMITTER &&
        !*v->pending_finish_valid) {
        *v->pending_finish_state = HOP_XFER_TASK_FAILED;
        *v->pending_finish_valid = 1;
    }
    if (!job_release_resources(v))
        return job_release_defer(v, o, expected, "Job resource release deferred");
    if (v->task->initialized || v->task->id) {
        if (!v->task->initialized)
            return job_release_defer(v, o, expected, "refusing to release Job with uninitialized live Task handle");
        if (!hop_xfer_task_is_terminal(v->task)) {
            hop_xfer_task_state state = *v->pending_finish_valid
                ? *v->pending_finish_state
                : (expected == HOP_XFER_SENDER_JOB_OWNER_SUBMITTER
                    ? HOP_XFER_TASK_FAILED : HOP_XFER_TASK_INTERRUPTED);
            if (!*v->pending_finish_valid &&
                hop_xfer_sender_context_job_cancel_requested(v->context, v->batch, v->task->id))
                state = HOP_XFER_TASK_CANCELLED;
            hop_xfer_sender_context_task_finish(v->context, v->task, state);
            if (!hop_xfer_task_is_terminal(v->task))
                return job_release_defer(v, o, expected, "Job Task terminalization deferred");
        }
        if (!hop_xfer_sender_context_task_dispose(v->context, v->task) ||
            v->task->initialized || v->task->id)
            return job_release_defer(v, o, expected, "Job Task disposal deferred");
    }
    if (*v->pending_status_end) {
        hop_xfer_sender_context_status_end(v->context, v->batch, *v->pending_status_phase,
            v->pending_status_detail[0] ? v->pending_status_detail : "Transfer ended");
        *v->pending_status_end = 0;
    }
    if (!o->mark_released(o->user, v->job))
        return job_release_defer(v, o, expected, "Job release ownership changed before free");
    o->free_storage(o->user, v->job);
    return HOP_XFER_SENDER_JOB_RELEASED;
}

/* ---- Worker Flow ---- */

static int worker_ops_valid(const hop_xfer_sender_worker_ops *ops) {
    return ops && ops->max_attempts && ops->reconnect_budget_ms &&
           ops->now_ms && ops->retry_delay_ms && ops->is_cancelled &&
           ops->is_stopping && ops->begin_completion && ops->set_queued &&
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
        if (ops->is_stopping(ops->user)) {
            result = HOP_XFER_SENDER_JOB_SHUTDOWN;
        } else {
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
                    if (ops->is_stopping(ops->user)) {
                        result = HOP_XFER_SENDER_JOB_SHUTDOWN;
                    } else {
                        worker_finish_failed(ops, job, "Retry queue admission failed");
                        return HOP_XFER_SENDER_WORKER_FINISHED;
                    }
                } else {
                    return HOP_XFER_SENDER_WORKER_REQUEUED;
                }
            } else {
                ops->store_retry_state(ops->user, job);
                worker_log(ops, HOP_XFER_SENDER_WORKER_LOG_RETRY_EXHAUSTED, job, 0);
            }
        }
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

/* ---- Worker Runtime Loop ---- */

#define SENDER_CHUNK_HEADER_BYTES 25u

static int runtime_ops_valid(const hop_xfer_sender_worker_runtime_ops *o) {
    return o && o->queue && o->queue_ops && o->poll_interval_ms && o->chunk_size &&
           o->view_job && o->is_stopping && o->snapshot_connection &&
           o->status_should_start && o->status_reset && o->clear_active &&
           o->send_abort && o->run_job && o->take_peer_cancelled_and_clear &&
           o->finish_job && o->release_job;
}

static int view_valid(const hop_xfer_sender_worker_job_view *v, const void *job) {
    return v && v->platform_job == job && v->context && v->task && v->task->id &&
           v->session && v->next_index && v->ok_count && v->pending_finish_state &&
           v->pending_finish_valid && v->pending_status_end &&
           v->pending_status_phase && v->pending_status_detail &&
           v->pending_status_detail_capacity;
}

static void pending_terminal(hop_xfer_sender_worker_job_view *v,
                             hop_xfer_task_state state, int phase,
                             const char *detail, int publish_status) {
    *v->pending_finish_state = state;
    *v->pending_finish_valid = 1;
    if (!publish_status) return;
    *v->pending_status_end = 1;
    *v->pending_status_phase = phase;
    (void)snprintf(v->pending_status_detail, v->pending_status_detail_capacity,
                   "%s", detail ? detail : "Transfer ended");
}

static int snapshot(const hop_xfer_sender_worker_runtime_ops *o, intptr_t *socket,
                    uint64_t *epoch, hop_session_id *session, int *connected) {
    *socket = (intptr_t)-1;
    *epoch = 0;
    memset(session, 0, sizeof(*session));
    *connected = 0;
    if (o->snapshot_connection(o->user, socket, epoch, session, connected))
        return 1;
    *socket = (intptr_t)-1;
    *epoch = 0;
    memset(session, 0, sizeof(*session));
    *connected = 0;
    return 0;
}

static void release_selected(const hop_xfer_sender_worker_runtime_ops *o,
                             hop_xfer_sender_worker_job_view *v, void *job) {
    if (v && v->task && v->task->id) o->clear_active(o->user, v->batch, v->task->id);
    o->release_job(o->user, job);
}

hop_xfer_sender_worker_run_result hop_xfer_sender_worker_run(
    const hop_xfer_sender_worker_runtime_ops *o) {
    if (!runtime_ops_valid(o) ||
        (size_t)o->chunk_size > SIZE_MAX - SENDER_CHUNK_HEADER_BYTES - HOP_DATA_PREFIX) {
        hop_log(HOP_LOG_ERROR, "xfer: invalid shared Sender Worker adapter");
        return HOP_XFER_SENDER_WORKER_RUN_ERROR;
    }
    size_t bytes = SENDER_CHUNK_HEADER_BYTES + (size_t)HOP_DATA_PREFIX + (size_t)o->chunk_size;
    uint8_t *tx = (uint8_t *)malloc(bytes);
    if (!tx) {
        hop_log(HOP_LOG_ERROR, "xfer: sender tx buffer allocation failed");
        return HOP_XFER_SENDER_WORKER_RUN_ERROR;
    }
    hop_xfer_sender_worker_run_result result = HOP_XFER_SENDER_WORKER_RUN_STOPPED;
    for (;;) {
        void *job = NULL;
        hop_xfer_sender_scheduler_result selected = hop_xfer_sender_scheduler_wait_next(
            o->queue, o->queue_ops, &o->scheduler, o->poll_interval_ms, &job);
        if (selected == HOP_XFER_SENDER_SCHEDULER_STOPPED) break;
        if (selected == HOP_XFER_SENDER_SCHEDULER_ERROR || !job) {
            hop_log(HOP_LOG_ERROR, "xfer: shared Sender scheduler failed");
            result = HOP_XFER_SENDER_WORKER_RUN_ERROR;
            break;
        }
        hop_xfer_sender_worker_job_view v;
        memset(&v, 0, sizeof(v));
        if (!o->view_job(o->user, job, &v) || !view_valid(&v, job)) {
            hop_log(HOP_LOG_ERROR, "xfer: invalid Sender Job view; releasing worker-owned Job");
            if (v.task && v.task->id) o->clear_active(o->user, v.batch, v.task->id);
            o->release_job(o->user, job);
            continue;
        }
        if (selected == HOP_XFER_SENDER_SCHEDULER_EXPIRED) {
            intptr_t socket = (intptr_t)-1;
            uint64_t epoch = 0;
            hop_session_id current;
            int connected = 0;
            int snap = snapshot(o, &socket, &epoch, &current, &connected);
            int cancelled = hop_xfer_sender_context_job_cancel_requested(
                v.context, v.batch, v.task->id);
            if (snap && cancelled && connected && hop_session_id_valid(&current) &&
                hop_session_id_equal(v.session, &current))
                o->send_abort(o->user, v.context, socket, epoch, v.batch);
            int show = o->status_should_start(o->user, v.batch);
            if (show) hop_xfer_sender_context_status_start_job(v.context, job);
            pending_terminal(&v, cancelled ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED,
                cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED,
                "Transfer stopped while queued", show);
            o->clear_active(o->user, v.batch, v.task->id);
            o->release_job(o->user, job);
            continue;
        }
        if (selected != HOP_XFER_SENDER_SCHEDULER_READY) {
            hop_log(HOP_LOG_ERROR, "xfer: unexpected Sender scheduler result");
            result = HOP_XFER_SENDER_WORKER_RUN_ERROR;
            break;
        }
        if (o->is_stopping(o->user)) {
            release_selected(o, &v, job);
            continue;
        }
        intptr_t socket = (intptr_t)-1;
        uint64_t epoch = 0;
        hop_session_id current;
        int connected = 0;
        int snap = snapshot(o, &socket, &epoch, &current, &connected);
        if (snap && hop_session_id_valid(&current) && !hop_session_id_equal(v.session, &current)) {
            *v.session = current;
            *v.next_index = 0;
            *v.ok_count = 0;
            o->status_reset(o->user);
        }
        if (hop_session_id_valid(v.session))
            (void)hop_xfer_sender_context_task_bind_session(v.context, v.task, v.session->bytes);
        if (!hop_xfer_sender_context_task_set_state(v.context, v.task, HOP_XFER_TASK_ACTIVE)) {
            o->clear_active(o->user, v.batch, v.task->id);
            pending_terminal(&v, HOP_XFER_TASK_FAILED, HOP_XFER_FAILED,
                             "Task activation failed", 0);
            o->release_job(o->user, job);
            continue;
        }
        hop_xfer_sender_context_status_start_job(v.context, job);
        hop_xfer_sender_job_result jr = connected
            ? o->run_job(o->user, job, socket, epoch, tx)
            : HOP_XFER_SENDER_JOB_NET_DOWN;
        int completion = jr == HOP_XFER_SENDER_JOB_DONE;
        int peer = completion ? 0 : o->take_peer_cancelled_and_clear(o->user, v.batch, v.task->id);
        (void)o->finish_job(o->user, job, socket, epoch, jr, completion, peer);
    }
    free(tx);
    return result;
}
