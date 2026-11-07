#ifndef HOP_XFER_TASK_H
#define HOP_XFER_TASK_H

#include <stdint.h>
#include "xfer_budget.h"
#include "xfer_status.h"

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

/* Task classification is independent of the budget reservation. Internal image
 * conversion jobs are not user-visible transfer transactions. */
typedef enum {
    HOP_TASK_KIND_UNSPECIFIED=0, HOP_TASK_KIND_FILES,
    HOP_TASK_KIND_IMAGE, HOP_TASK_KIND_IMAGE_WORK, HOP_TASK_KIND_MIXED
} hop_xfer_task_kind;
#define HOP_TASK_SESSION_BYTES 16u
/* Maximum number of simultaneously live manager records. */
#define HOP_TASK_MANAGER_MAX_LIVE 4096u

/* Immutable snapshot. IDs and resource values are diagnostic, never ownership. */
typedef struct {
    hop_xfer_task_id id;
    uint64_t offer_id, reservation_handle_id;
    uint32_t request_id, transfer_id;
    int direction;
    hop_xfer_task_kind kind;
    uint8_t session[HOP_TASK_SESSION_BYTES]; /* all zero means not yet bound */
    hop_xfer_task_state state;
    int cancel_requested, completing;
    uint32_t jobs, entries, rx_queue_slots;
    uint64_t pending_bytes, image_bytes, rx_queue_bytes, image_temp_bytes;
} hop_xfer_task_snapshot;

/* A task stays in the manager until dispose, even after it reaches a terminal
 * state. Snapshots contain only tasks that have not yet been disposed; durable
 * history and UI/progress integration are separate, not implemented here. */
/* Request is asynchronous. Returns 0 for missing, terminal, internal, or a
 * task already committing its result; never changes another task. */
int hop_xfer_task_request_cancel(hop_xfer_task_id id);
int hop_xfer_task_cancel_requested(hop_xfer_task_id id);
/* Linearization point: completion wins only if cancellation was not requested.
 * After this succeeds cancellation requests are rejected, before side effects. */
int hop_xfer_task_begin_completion(const hop_xfer_task *task);
/* Receiver-owned monotonic disconnect timer. Returns 1 only when an active
 * receive task has continuously lacked an authenticated data channel for at
 * least budget_ms. Reconnection and clock rollback reset its timer. */
int hop_xfer_task_rx_disconnect_expired(const hop_xfer_task *task, int connected,
                                        uint64_t now_ms, uint64_t budget_ms);
/* Diagnostic only: sums manager-owned reservations under the manager lock,
 * then reads the ledger under its lock. RX pool capacity is shared infrastructure
 * and is reported in ledger but intentionally excluded from task ownership. */
#define HOP_TASK_AUDIT_OWNER   1u
#define HOP_TASK_AUDIT_USAGE   2u
#define HOP_TASK_AUDIT_OVERFLOW 4u
#define HOP_TASK_AUDIT_TERMINAL 8u
typedef struct {
    uint32_t mismatches, task_count;
    uint64_t jobs, entries, rx_queue_slots;
    uint64_t pending_bytes, image_bytes, rx_queue_bytes, image_temp_bytes;
    hop_xfer_budget_snapshot ledger;
} hop_xfer_task_audit;
int hop_xfer_task_reconcile(hop_xfer_task_audit *out);

int hop_xfer_task_get_snapshot(hop_xfer_task_id id, hop_xfer_task_snapshot *out);
int hop_xfer_task_list(hop_xfer_task_snapshot *out, int capacity, int *total);
uint64_t hop_xfer_task_image_temp_bytes(const hop_xfer_task *task);

/* Zero-initialized handle and nonzero transfer_id required. */
int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer_id, uint32_t request_id, uint32_t transfer_id);
int hop_xfer_task_init_classified(hop_xfer_task *task, uint64_t offer_id, uint32_t request_id,
                                 uint32_t transfer_id, int direction, hop_xfer_task_kind kind);
/* The caller supplies the validated current transport session. On reconnect,
 * replacing it records the new transport incarnation, not a new Task ID. */
int hop_xfer_task_bind_session(const hop_xfer_task *task, const uint8_t session[HOP_TASK_SESSION_BYTES]);
/* Receivers may observe both files and images within a batch; record MIXED. */
int hop_xfer_task_set_kind(const hop_xfer_task *task, hop_xfer_task_kind kind);
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
