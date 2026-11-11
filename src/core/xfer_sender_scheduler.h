#ifndef HOP_XFER_SENDER_SCHEDULER_H
#define HOP_XFER_SENDER_SCHEDULER_H

#include <stdint.h>
#include "xfer_sender_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOP_XFER_SENDER_SCHEDULER_ERROR = -1,
    HOP_XFER_SENDER_SCHEDULER_STOPPED = 0,
    HOP_XFER_SENDER_SCHEDULER_READY = 1,
    HOP_XFER_SENDER_SCHEDULER_EXPIRED = 2
} hop_xfer_sender_scheduler_result;

/*
 * Native queue synchronization and waiting stay in the platform adapter.
 *
 * begin_pass must acquire the native queue lock, snapshot peer readiness,
 * monotonic milliseconds and the current batch-cancel marker, then return 1
 * with the queue lock still held. Returning 0 means no lock was acquired.
 *
 * wait_for_work_locked is called with that same lock held. It must atomically
 * release/wait/reacquire it (e.g. pthread_cond_timedwait), or release it around
 * a persistent native event wait (e.g. WaitForMultipleObjects). It must return
 * with the queue lock held. This contract prevents lost queue wakeups.
 *
 * claim_job_locked runs under the queue lock after a node has been detached.
 * It transfers the Job owner to WORKER and may publish the active-job marker.
 */
typedef struct {
    void *user;
    int (*begin_pass)(void *user, int *peer_ready, uint64_t *now_ms,
                      uint32_t *cancel_batch);
    int (*is_stopping)(void *user);
    int (*wait_for_work_locked)(void *user, uint32_t timeout_ms);
    void (*claim_job_locked)(void *user, void *job, int expired);
    void (*end_pass)(void *user);
} hop_xfer_sender_scheduler_ops;

/*
 * Blocks until one Job can be processed or the worker is stopping.
 *
 * Cancelled Jobs anywhere in the queue and expired FIFO heads are returned as
 * EXPIRED even while the peer is offline. Only the FIFO head can be returned
 * as READY, and only when the peer is ready and its retry deadline has passed.
 * The adapter's claim callback executes before the queue lock is released.
 */
hop_xfer_sender_scheduler_result hop_xfer_sender_scheduler_wait_next(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *queue_ops,
    const hop_xfer_sender_scheduler_ops *scheduler_ops,
    uint32_t poll_interval_ms,
    void **job_out);

#ifdef __cplusplus
}
#endif
#endif
