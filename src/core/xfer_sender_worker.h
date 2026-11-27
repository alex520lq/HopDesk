#ifndef HOP_XFER_SENDER_WORKER_H
#define HOP_XFER_SENDER_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include "hop.h"
#include "xfer_status.h"
#include "xfer_task.h"
#include "xfer_sender_context.h"
#include "xfer_sender_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Intrusive Sender Queue ---- */

typedef struct {
    void *head;
    void *tail;
    uint32_t count;
} hop_xfer_sender_queue;

typedef struct {
    void *user;
    void *(*next)(void *user, const void *node);
    void (*set_next)(void *user, void *node, void *next);
    int (*is_cancelled)(void *user, const void *node);
    uint32_t (*batch)(void *user, const void *node);
    uint64_t (*retry_at_ms)(void *user, const void *node);
    uint64_t (*give_up_at_ms)(void *user, const void *node);
} hop_xfer_sender_queue_ops;

typedef enum {
    HOP_XFER_SENDER_QUEUE_NONE = 0,
    HOP_XFER_SENDER_QUEUE_READY,
    HOP_XFER_SENDER_QUEUE_EXPIRED
} hop_xfer_sender_queue_take_result;

void hop_xfer_sender_queue_init(hop_xfer_sender_queue *queue);
int hop_xfer_sender_queue_push_back(hop_xfer_sender_queue *queue,
                                    const hop_xfer_sender_queue_ops *ops,
                                    void *node);
int hop_xfer_sender_queue_push_front(hop_xfer_sender_queue *queue,
                                     const hop_xfer_sender_queue_ops *ops,
                                     void *node);
int hop_xfer_sender_queue_contains_batch(const hop_xfer_sender_queue *queue,
                                         const hop_xfer_sender_queue_ops *ops,
                                         uint32_t batch);
uint32_t hop_xfer_sender_queue_count(const hop_xfer_sender_queue *queue);
int hop_xfer_sender_queue_empty(const hop_xfer_sender_queue *queue);
void *hop_xfer_sender_queue_detach_all(hop_xfer_sender_queue *queue);
void *hop_xfer_sender_queue_take_cancelled(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops);
void *hop_xfer_sender_queue_take_head(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops,
    uint64_t now_ms,
    uint32_t cancel_batch,
    int peer_ready,
    int allow_ready,
    hop_xfer_sender_queue_take_result *result);

/* ---- Sender Scheduler ---- */

typedef enum {
    HOP_XFER_SENDER_SCHEDULER_ERROR = -1,
    HOP_XFER_SENDER_SCHEDULER_STOPPED = 0,
    HOP_XFER_SENDER_SCHEDULER_READY = 1,
    HOP_XFER_SENDER_SCHEDULER_EXPIRED = 2
} hop_xfer_sender_scheduler_result;

typedef struct {
    void *user;
    int (*begin_pass)(void *user, int *peer_ready, uint64_t *now_ms,
                      uint32_t *cancel_batch);
    int (*is_stopping)(void *user);
    int (*wait_for_work_locked)(void *user, uint32_t timeout_ms);
    void (*claim_job_locked)(void *user, void *job, int expired);
    void (*end_pass)(void *user);
} hop_xfer_sender_scheduler_ops;

hop_xfer_sender_scheduler_result hop_xfer_sender_scheduler_wait_next(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *queue_ops,
    const hop_xfer_sender_scheduler_ops *scheduler_ops,
    uint32_t poll_interval_ms,
    void **job_out);

/* ---- Active Job & Peer-ABORT State ---- */

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

/* ---- Retry & Terminal Lifecycle Policies ---- */

typedef enum {
    HOP_XFER_SENDER_RETRY_INVALID = -1,
    HOP_XFER_SENDER_RETRY_EXHAUSTED = 0,
    HOP_XFER_SENDER_RETRY_SCHEDULED = 1
} hop_xfer_sender_retry_result;

typedef struct {
    uint32_t attempt;
    uint64_t give_up_at_ms;
    uint64_t retry_at_ms;
} hop_xfer_sender_retry_plan;

hop_xfer_sender_retry_result hop_xfer_sender_plan_retry(
    uint64_t now_ms,
    uint64_t current_give_up_at_ms,
    uint32_t current_attempt,
    uint32_t max_attempts,
    uint64_t reconnect_budget_ms,
    unsigned retry_delay_ms,
    hop_xfer_sender_retry_plan *out);

typedef struct {
    hop_xfer_task_state task_state;
    int status_phase;
    const char *detail;
} hop_xfer_sender_terminal_decision;

int hop_xfer_sender_get_terminal_decision(
    hop_xfer_sender_job_result result,
    int peer_cancelled,
    int complete,
    hop_xfer_sender_terminal_decision *out);

/* ---- Job Ownership & Release Lifecycle ---- */

typedef enum {
    HOP_XFER_SENDER_JOB_OWNER_SUBMITTER = 0,
    HOP_XFER_SENDER_JOB_OWNER_QUEUE,
    HOP_XFER_SENDER_JOB_OWNER_WORKER,
    HOP_XFER_SENDER_JOB_OWNER_SHUTDOWN,
    HOP_XFER_SENDER_JOB_OWNER_RELEASING,
    HOP_XFER_SENDER_JOB_OWNER_RELEASED
} hop_xfer_sender_job_owner;

typedef struct {
    void *job;
    const hop_xfer_sender_context *context;
    hop_xfer_task *task;
    uint32_t batch;
    int is_image;
    uint8_t **pixels;
    int *image_payload_released;
    hop_xfer_task_state *pending_finish_state;
    int *pending_finish_valid;
    int *pending_status_end;
    int *pending_status_phase;
    char *pending_status_detail;
} hop_xfer_sender_job_release_view;

typedef struct {
    void *user;
    int (*claim)(void *, void *, hop_xfer_sender_job_owner, hop_xfer_sender_job_owner *);
    void (*defer_to_shutdown)(void *, void *, hop_xfer_sender_job_owner);
    int (*mark_released)(void *, void *);
    void (*free_storage)(void *, void *);
} hop_xfer_sender_job_release_ops;

typedef enum {
    HOP_XFER_SENDER_JOB_RELEASE_DEFERRED = 0,
    HOP_XFER_SENDER_JOB_RELEASED = 1,
    HOP_XFER_SENDER_JOB_RELEASE_NOT_OWNER = -1,
    HOP_XFER_SENDER_JOB_RELEASE_INVALID = -2
} hop_xfer_sender_job_release_result;

hop_xfer_sender_job_release_result hop_xfer_sender_job_release(
    const hop_xfer_sender_job_release_view *view,
    const hop_xfer_sender_job_release_ops *ops,
    hop_xfer_sender_job_owner expected);

/* ---- Worker Flow & Runtime Loop ---- */

typedef struct {
    uint32_t batch;
    uint64_t task_id;
    uint32_t attempt;
    uint64_t retry_at_ms;
    uint64_t give_up_at_ms;
    int next_index;
    int ok_count;
    int entry_count;
    int total_count;
    hop_xfer_sender_job_result result;
    int completion_path;
    int peer_cancelled;
} hop_xfer_sender_worker_job;

typedef enum {
    HOP_XFER_SENDER_WORKER_INVALID = -1,
    HOP_XFER_SENDER_WORKER_FINISHED = 0,
    HOP_XFER_SENDER_WORKER_REQUEUED = 1
} hop_xfer_sender_worker_action;

typedef enum {
    HOP_XFER_SENDER_WORKER_LOG_RETRY_INVALID = 1,
    HOP_XFER_SENDER_WORKER_LOG_RETRY_SCHEDULED,
    HOP_XFER_SENDER_WORKER_LOG_RETRY_STATE_FAILED,
    HOP_XFER_SENDER_WORKER_LOG_REQUEUE_FAILED,
    HOP_XFER_SENDER_WORKER_LOG_RETRY_EXHAUSTED,
    HOP_XFER_SENDER_WORKER_LOG_PARTIAL_DONE
} hop_xfer_sender_worker_log_event;

typedef struct {
    void *user;
    uint32_t max_attempts;
    uint64_t reconnect_budget_ms;
    uint64_t (*now_ms)(void *user);
    unsigned (*retry_delay_ms)(void *user, uint32_t next_attempt);
    int (*is_cancelled)(void *user, uint32_t batch, uint64_t task_id);
    int (*is_stopping)(void *user);
    int (*begin_completion)(void *user, uint64_t task_id);
    int (*set_queued)(void *user, uint64_t task_id);
    void (*store_retry_state)(void *user,
                              const hop_xfer_sender_worker_job *job);
    int (*requeue_front)(void *user);
    int (*peer_cancelled_and_clear_current)(void *user,
                                            uint32_t batch,
                                            uint64_t task_id);
    void (*send_abort)(void *user, uint32_t batch);
    hop_xfer_sender_job_result (*send_done)(void *user);
    void (*task_finish)(void *user, hop_xfer_task_state state);
    void (*status_end)(void *user, uint32_t batch, int phase,
                       const char *detail);
    void (*dispose_job)(void *user);
    void (*log_event)(void *user, hop_xfer_sender_worker_log_event event,
                      const hop_xfer_sender_worker_job *job,
                      unsigned retry_delay_ms);
} hop_xfer_sender_worker_ops;

hop_xfer_sender_worker_action hop_xfer_sender_worker_finish_job(
    const hop_xfer_sender_worker_ops *ops,
    hop_xfer_sender_worker_job *job);

typedef struct {
    void *platform_job;
    const hop_xfer_sender_context *context;
    hop_xfer_task *task;
    hop_session_id *session;
    int *next_index, *ok_count;
    hop_xfer_task_state *pending_finish_state;
    int *pending_finish_valid, *pending_status_end, *pending_status_phase;
    char *pending_status_detail;
    size_t pending_status_detail_capacity;
    uint32_t batch;
} hop_xfer_sender_worker_job_view;

typedef struct {
    void *user;
    hop_xfer_sender_queue *queue;
    const hop_xfer_sender_queue_ops *queue_ops;
    hop_xfer_sender_scheduler_ops scheduler;
    uint32_t poll_interval_ms, chunk_size;
    int (*view_job)(void *, void *, hop_xfer_sender_worker_job_view *);
    int (*is_stopping)(void *);
    int (*snapshot_connection)(void *, intptr_t *, uint64_t *, hop_session_id *, int *);
    int (*status_should_start)(void *, uint32_t);
    void (*status_reset)(void *);
    void (*clear_active)(void *, uint32_t, uint64_t);
    void (*send_abort)(void *, const hop_xfer_sender_context *, intptr_t, uint64_t, uint32_t);
    hop_xfer_sender_job_result (*run_job)(void *, void *, intptr_t, uint64_t, uint8_t *);
    int (*take_peer_cancelled_and_clear)(void *, uint32_t, uint64_t);
    hop_xfer_sender_worker_action (*finish_job)(void *, void *, intptr_t, uint64_t,
        hop_xfer_sender_job_result, int, int);
    void (*release_job)(void *, void *);
} hop_xfer_sender_worker_runtime_ops;

typedef enum {
    HOP_XFER_SENDER_WORKER_RUN_ERROR = 0,
    HOP_XFER_SENDER_WORKER_RUN_STOPPED = 1
} hop_xfer_sender_worker_run_result;

hop_xfer_sender_worker_run_result hop_xfer_sender_worker_run(
    const hop_xfer_sender_worker_runtime_ops *ops);

#ifdef __cplusplus
}
#endif
#endif
