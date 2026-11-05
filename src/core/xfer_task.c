#include "xfer_task.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_task_id_lock = SRWLOCK_INIT;
#define TASK_LOCK() AcquireSRWLockExclusive(&s_task_id_lock)
#define TASK_UNLOCK() ReleaseSRWLockExclusive(&s_task_id_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_task_id_lock = PTHREAD_MUTEX_INITIALIZER;
#define TASK_LOCK() pthread_mutex_lock(&s_task_id_lock)
#define TASK_UNLOCK() pthread_mutex_unlock(&s_task_id_lock)
#endif

static uint64_t s_next_task_id;

static uint64_t next_task_id(void) {
    uint64_t id;
    TASK_LOCK();
    id = ++s_next_task_id;
    if (!id) id = ++s_next_task_id;
    TASK_UNLOCK();
    return id;
}

static int task_owns_reservation(const hop_xfer_task *task) {
    return task && task->initialized && task->id != 0 &&
           task->reservation.task_id == task->id &&
           task->reservation.handle_id != 0;
}

int hop_xfer_task_is_terminal(const hop_xfer_task *task) {
    if (!task || !task->initialized) return 1;
    return task->state == HOP_XFER_TASK_COMPLETED ||
           task->state == HOP_XFER_TASK_PARTIAL ||
           task->state == HOP_XFER_TASK_FAILED ||
           task->state == HOP_XFER_TASK_CANCELLED ||
           task->state == HOP_XFER_TASK_INTERRUPTED;
}

int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer_id, uint32_t request_id, uint32_t transfer_id) {
    if (!task || !transfer_id) return 0;
    memset(task, 0, sizeof(*task));
    task->id = next_task_id();
    task->offer_id = offer_id;
    task->request_id = request_id;
    task->transfer_id = transfer_id;
    if (!task->id || !hop_xfer_budget_reservation_init(&task->reservation, task->id)) {
        memset(task, 0, sizeof(*task));
        return 0;
    }
    task->state = HOP_XFER_TASK_CREATED;
    task->initialized = 1;
    return 1;
}

int hop_xfer_task_move(hop_xfer_task *dst, hop_xfer_task *src) {
    if (!dst || !src || dst == src || !task_owns_reservation(src) ||
        dst->initialized) return 0;
    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    memset(src, 0, sizeof(*src));
    return 1;
}

int hop_xfer_task_set_state(hop_xfer_task *task, hop_xfer_task_state state) {
    if (!task_owns_reservation(task) || hop_xfer_task_is_terminal(task)) return 0;
    if (state != HOP_XFER_TASK_SCANNING &&
        state != HOP_XFER_TASK_QUEUED &&
        state != HOP_XFER_TASK_ACTIVE) return 0;

    switch (task->state) {
        case HOP_XFER_TASK_CREATED:
            if (state != HOP_XFER_TASK_SCANNING &&
                state != HOP_XFER_TASK_QUEUED &&
                state != HOP_XFER_TASK_ACTIVE) return 0;
            break;
        case HOP_XFER_TASK_SCANNING:
            if (state != HOP_XFER_TASK_QUEUED) return 0;
            break;
        case HOP_XFER_TASK_QUEUED:
            if (state != HOP_XFER_TASK_ACTIVE) return 0;
            break;
        case HOP_XFER_TASK_ACTIVE:
            if (state != HOP_XFER_TASK_QUEUED) return 0; /* reconnect/retry */
            break;
        default:
            return 0;
    }
    task->state = state;
    return 1;
}

int hop_xfer_task_reserve_job(hop_xfer_task *task) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_job(&task->reservation);
}

int hop_xfer_task_reserve_entry(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_entry(&task->reservation, bytes);
}

int hop_xfer_task_reserve_image(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_image(&task->reservation, bytes);
}

int hop_xfer_task_reserve_temp_image(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_temp_image(&task->reservation, bytes);
}

int hop_xfer_task_release_temp_image(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) &&
           hop_xfer_budget_release_temp_image(&task->reservation, bytes);
}

int hop_xfer_task_reserve_rx_buffer(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_rx_buffer(&task->reservation, bytes);
}

int hop_xfer_task_release_rx_buffer(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) &&
           hop_xfer_budget_release_rx_buffer(&task->reservation, bytes);
}

int hop_xfer_task_reserve_image_temp(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) && !hop_xfer_task_is_terminal(task) &&
           hop_xfer_budget_reserve_image_temp(&task->reservation, bytes);
}

int hop_xfer_task_release_image_temp(hop_xfer_task *task, uint64_t bytes) {
    return task_owns_reservation(task) &&
           hop_xfer_budget_release_image_temp(&task->reservation, bytes);
}

int hop_xfer_task_finish(hop_xfer_task *task, hop_xfer_task_state terminal_state) {
    if (!task_owns_reservation(task)) return 0;
    /* A receive task cannot become terminal while its writer still owns buffers. */
    if (task->reservation.rx_queue_slots || task->reservation.rx_queue_bytes ||
        task->reservation.image_temp_bytes) return 0;
    if (terminal_state != HOP_XFER_TASK_COMPLETED &&
        terminal_state != HOP_XFER_TASK_PARTIAL &&
        terminal_state != HOP_XFER_TASK_FAILED &&
        terminal_state != HOP_XFER_TASK_CANCELLED &&
        terminal_state != HOP_XFER_TASK_INTERRUPTED) return 0;
    if (hop_xfer_task_is_terminal(task)) {
        hop_xfer_budget_release(&task->reservation);
        return 0;
    }
    task->state = terminal_state;
    hop_xfer_budget_release(&task->reservation);
    return 1;
}

void hop_xfer_task_dispose(hop_xfer_task *task) {
    if (!task || !task->initialized) return;
    /* Do not invalidate the owner while the disk writer still references it. */
    if (task->reservation.rx_queue_slots || task->reservation.rx_queue_bytes ||
        task->reservation.image_temp_bytes) return;
    if (!hop_xfer_task_is_terminal(task)) {
        if (!hop_xfer_task_finish(task, HOP_XFER_TASK_INTERRUPTED)) return;
    } else {
        hop_xfer_budget_release(&task->reservation);
    }
    memset(task, 0, sizeof(*task));
}
