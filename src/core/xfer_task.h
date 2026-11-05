#ifndef HOP_XFER_TASK_H
#define HOP_XFER_TASK_H

#include <stdint.h>
#include "xfer_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t hop_xfer_task_id;

typedef enum {
    HOP_XFER_TASK_EMPTY = 0,
    HOP_XFER_TASK_CREATED,
    HOP_XFER_TASK_SCANNING,
    HOP_XFER_TASK_QUEUED,
    HOP_XFER_TASK_ACTIVE,
    HOP_XFER_TASK_COMPLETED,
    HOP_XFER_TASK_PARTIAL,
    HOP_XFER_TASK_FAILED,
    HOP_XFER_TASK_CANCELLED,
    HOP_XFER_TASK_INTERRUPTED
} hop_xfer_task_state;

/*
 * Minimal internal task core. It deliberately owns no UI, history, retry
 * policy, or task collection. The task is the single lifetime owner of its
 * reservation handle; the budget module only accounts resources.
 */
typedef struct {
    hop_xfer_task_id id; /* process-local monotonic Task ID */
    uint64_t offer_id;   /* clipboard Offer generation; 0 for non-paste tasks */
    uint32_t request_id; /* control-plane Request ID; 0 if not request-driven */
    uint32_t transfer_id;/* data-plane batch ID; session scopes this value */
    hop_xfer_task_state state;
    hop_xfer_reservation_handle reservation;
    int initialized;
} hop_xfer_task;

/* task must be zero-initialized; transfer_id must be nonzero. */
int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer_id, uint32_t request_id, uint32_t transfer_id);
/* dst must be zero-initialized. Move transfers sole ownership of the reservation. */
int hop_xfer_task_move(hop_xfer_task *dst, hop_xfer_task *src);
int hop_xfer_task_set_state(hop_xfer_task *task, hop_xfer_task_state state);
int hop_xfer_task_reserve_job(hop_xfer_task *task);
int hop_xfer_task_reserve_entry(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_image(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_temp_image(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_release_temp_image(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_rx_buffer(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_release_rx_buffer(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_reserve_image_temp(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_release_image_temp(hop_xfer_task *task, uint64_t bytes);
int hop_xfer_task_finish(hop_xfer_task *task, hop_xfer_task_state terminal_state);
void hop_xfer_task_dispose(hop_xfer_task *task);
int hop_xfer_task_is_terminal(const hop_xfer_task *task);

#ifdef __cplusplus
}
#endif
#endif
