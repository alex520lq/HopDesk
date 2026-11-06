#ifndef HOP_XFER_TASK_H
#define HOP_XFER_TASK_H

#include <stdint.h>
#include "xfer_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t hop_xfer_task_id;
typedef enum {
    HOP_XFER_TASK_EMPTY = 0, HOP_XFER_TASK_CREATED, HOP_XFER_TASK_SCANNING,
    HOP_XFER_TASK_QUEUED, HOP_XFER_TASK_ACTIVE, HOP_XFER_TASK_COMPLETED,
    HOP_XFER_TASK_PARTIAL, HOP_XFER_TASK_FAILED, HOP_XFER_TASK_CANCELLED,
    HOP_XFER_TASK_INTERRUPTED
} hop_xfer_task_state;

/* A movable, non-copyable handle. The manager owns the task and reservation;
 * callers must initialize this to zero and must not copy it or mutate its fields. */
typedef struct {
    hop_xfer_task_id id;
    int initialized;
} hop_xfer_task;

/* Immutable snapshot. IDs and resource values are diagnostic, never ownership. */
typedef struct {
    hop_xfer_task_id id;
    uint64_t offer_id, reservation_handle_id;
    uint32_t request_id, transfer_id;
    hop_xfer_task_state state;
    uint32_t jobs, entries, rx_queue_slots;
    uint64_t pending_bytes, image_bytes, rx_queue_bytes, image_temp_bytes;
} hop_xfer_task_snapshot;

/* A task stays in the manager until dispose, even after it reaches a terminal
 * state. Snapshots contain only tasks that have not yet been disposed; durable
 * history and UI/progress integration are separate, not implemented here. */
int hop_xfer_task_get_snapshot(hop_xfer_task_id id, hop_xfer_task_snapshot *out);
int hop_xfer_task_list(hop_xfer_task_snapshot *out, int capacity, int *total);
uint64_t hop_xfer_task_image_temp_bytes(const hop_xfer_task *task);

/* Zero-initialized handle and nonzero transfer_id required. */
int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer_id, uint32_t request_id, uint32_t transfer_id);
/* Move updates the manager's owner; copied or stale handles are rejected. */
int hop_xfer_task_move(hop_xfer_task *dst, hop_xfer_task *src);
int hop_xfer_task_set_state(hop_xfer_task *task, hop_xfer_task_state state);
int hop_xfer_task_reserve_job(hop_xfer_task *task);
int hop_xfer_task_reserve_entry(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_image(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_rx_buffer(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_release_rx_buffer(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_image_temp(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_release_image_temp(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_transfer_image_temp(hop_xfer_task *from, hop_xfer_task *to, uint64_t bytes);
int hop_xfer_task_finish(hop_xfer_task *task, hop_xfer_task_state terminal_state);
void hop_xfer_task_dispose(hop_xfer_task *task);
int hop_xfer_task_is_terminal(const hop_xfer_task *task);
#ifdef __cplusplus
}
#endif
#endif
