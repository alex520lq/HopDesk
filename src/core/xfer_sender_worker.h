#ifndef HOP_XFER_SENDER_WORKER_H
#define HOP_XFER_SENDER_WORKER_H
#include <stddef.h>
#include <stdint.h>
#include "hop.h"
#include "xfer_sender_context.h"
#include "xfer_sender_scheduler.h"
#include "xfer_sender_worker_flow.h"
#ifdef __cplusplus
extern "C" {
#endif
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
} hop_xfer_sender_worker_ops;
typedef enum { HOP_XFER_SENDER_WORKER_RUN_ERROR=0, HOP_XFER_SENDER_WORKER_RUN_STOPPED=1 }
    hop_xfer_sender_worker_run_result;
hop_xfer_sender_worker_run_result hop_xfer_sender_worker_run(const hop_xfer_sender_worker_ops *ops);
#ifdef __cplusplus
}
#endif
#endif
