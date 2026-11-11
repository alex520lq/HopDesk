#ifndef HOP_XFER_SENDER_WORKER_FLOW_H
#define HOP_XFER_SENDER_WORKER_FLOW_H
#include "xfer_sender_engine.h"
#include "xfer_task.h"
#ifdef __cplusplus
extern "C" {
#endif

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
    /* True once platform shutdown has closed admission and interrupted IO. */
    int (*is_stopping)(void *user);
    int (*begin_completion)(void *user, uint64_t task_id);
    int (*set_queued)(void *user, uint64_t task_id);
    void (*store_retry_state)(void *user,
                              const hop_xfer_sender_worker_job *job);
    int (*requeue_front)(void *user);
    /* For raw DONE paths, atomically reads peer cancellation and clears the
     * active-job marker after DONE/ABORT handling. */
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

/* For non-DONE paths the platform worker snapshots peer_cancelled and clears
 * its active marker atomically under the native lock before calling this. */
hop_xfer_sender_worker_action hop_xfer_sender_worker_finish_job(
    const hop_xfer_sender_worker_ops *ops,
    hop_xfer_sender_worker_job *job);

#ifdef __cplusplus
}
#endif
#endif
