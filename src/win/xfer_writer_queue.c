#include "xfer_writer_queue.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../core/xfer_rx_queue.h"
#include "../core/xfer_buffer_pool.h"
#include "xfer_writer_io.h"
#include "xfer_writer_worker.h"

#define RX_Q_SLOTS HOP_XFER_DEFAULT_RX_QUEUE_SLOTS
#define RX_Q_MAX_BYTES HOP_XFER_DEFAULT_RX_QUEUE_BYTES

enum { RX_OPEN = 1, RX_WRITE, RX_CLOSE };

typedef struct {
    int kind;
    wchar_t path[MAX_PATH];
    uint64_t offset;
    uint32_t length;
    uint8_t *data;
    int pool_slot;
    void *task_owner;
} writer_command;

struct hop_writer_queue {
    hop_xfer_buffer_pool pool;
    writer_command ring[RX_Q_SLOTS];
    int head, tail;
    hop_xfer_rx_queue accounting;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv_work, cv_space;
    hop_writer_worker worker;
    hop_writer_io io;
    volatile LONG error;
    int stop;
    int stopped;
    void *callback_context;
    hop_writer_queue_callbacks callbacks;
};

static int watchdog_begin(hop_writer_queue *queue, const char *label, uint32_t deadline) {
    return queue->callbacks.watchdog_begin
        ? queue->callbacks.watchdog_begin(queue->callback_context, label, deadline) : 0;
}
static void watchdog_end(hop_writer_queue *queue, int guard) {
    if (queue->callbacks.watchdog_end)
        queue->callbacks.watchdog_end(queue->callback_context, guard);
}

static void run_command(hop_writer_queue *queue, writer_command *command) {
    unsigned long error = 0;
    switch (command->kind) {
        case RX_OPEN:
            if (!hop_writer_io_open(&queue->io, command->path, &error)) {
                InterlockedExchange(&queue->error, 1);
                if (queue->callbacks.open_error)
                    queue->callbacks.open_error(queue->callback_context, command->path, error);
            }
            break;
        case RX_WRITE:
            if (!hop_writer_io_write_at(&queue->io, command->offset,
                                        command->data, command->length, &error)) {
                InterlockedExchange(&queue->error, 1);
                if (queue->callbacks.write_error)
                    queue->callbacks.write_error(queue->callback_context, command->offset, error);
            }
            break;
        case RX_CLOSE:
            hop_writer_io_close(&queue->io);
            break;
    }
}

static int worker_take(void *context, void *task_out) {
    hop_writer_queue *queue = (hop_writer_queue *)context;
    EnterCriticalSection(&queue->lock);
    /* A producer may still be returning a reservation outside this lock. */
    while (queue->accounting.queued_slots == 0 &&
           (!queue->stop || !hop_xfer_rx_queue_drained(&queue->accounting)))
        SleepConditionVariableCS(&queue->cv_work, &queue->lock, INFINITE);
    if (queue->accounting.queued_slots == 0) {
        LeaveCriticalSection(&queue->lock);
        return 0;
    }
    writer_command *command = (writer_command *)task_out;
    *command = queue->ring[queue->head];
    queue->head = (queue->head + 1) % RX_Q_SLOTS;
    (void)hop_xfer_rx_queue_dequeue(&queue->accounting);
    LeaveCriticalSection(&queue->lock);
    return 1;
}

static void worker_run(void *context, const void *task_data) {
    hop_writer_queue *queue = (hop_writer_queue *)context;
    writer_command *command = (writer_command *)task_data;
    int guard = watchdog_begin(queue, "disk writer task", queue->callbacks.io_deadline_ms);
    uint64_t started = queue->callbacks.now_ns
        ? queue->callbacks.now_ns(queue->callback_context) : 0;
    int cancelled = command->kind == RX_WRITE && command->task_owner &&
        queue->callbacks.task_cancelled &&
        queue->callbacks.task_cancelled(queue->callback_context, command->task_owner);
    if (!cancelled) run_command(queue, command);
    if (command->kind == RX_WRITE && command->task_owner &&
        queue->callbacks.write_time && queue->callbacks.now_ns) {
        uint64_t ended = queue->callbacks.now_ns(queue->callback_context);
        queue->callbacks.write_time(queue->callback_context, command->task_owner,
                                    ended >= started ? ended - started : 0);
    }
    watchdog_end(queue, guard);
}

static void worker_complete(void *context, const void *task_data) {
    hop_writer_queue *queue = (hop_writer_queue *)context;
    const writer_command *command = (const writer_command *)task_data;
    if (command->kind == RX_WRITE && command->task_owner &&
        queue->callbacks.release_task_rx)
        queue->callbacks.release_task_rx(queue->callback_context,
                                         command->task_owner, command->length);
    EnterCriticalSection(&queue->lock);
    if (command->kind == RX_WRITE)
        hop_xfer_buffer_pool_release_locked(&queue->pool, command->pool_slot);
    hop_xfer_rx_queue_complete(&queue->accounting,
                               command->kind == RX_WRITE ? command->length : 0);
    WakeConditionVariable(&queue->cv_space);
    LeaveCriticalSection(&queue->lock);
}

static void worker_exit(void *context) {
    hop_writer_queue *queue = (hop_writer_queue *)context;
    hop_writer_io_close(&queue->io);
}

static int enqueue_control(hop_writer_queue *queue, int kind, const wchar_t *path) {
    if (!queue || (kind == RX_OPEN && !path)) return 0;
    writer_command command;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.pool_slot = -1;
    if (path) {
        size_t length = wcslen(path);
        if (length >= MAX_PATH) return 0;
        memcpy(command.path, path, (length + 1) * sizeof(wchar_t));
    }
    int guard = watchdog_begin(queue, "disk queue wait", queue->callbacks.io_deadline_ms + 5000);
    EnterCriticalSection(&queue->lock);
    while (!hop_xfer_rx_queue_can_push(&queue->accounting, 0) && !queue->stop)
        SleepConditionVariableCS(&queue->cv_space, &queue->lock, INFINITE);
    if (queue->stop || !hop_xfer_rx_queue_push(&queue->accounting, 0)) {
        LeaveCriticalSection(&queue->lock);
        watchdog_end(queue, guard);
        return 0;
    }
    queue->ring[queue->tail] = command;
    queue->tail = (queue->tail + 1) % RX_Q_SLOTS;
    WakeConditionVariable(&queue->cv_work);
    LeaveCriticalSection(&queue->lock);
    watchdog_end(queue, guard);
    return 1;
}

hop_writer_queue *hop_writer_queue_create(
    void *callback_context, const hop_writer_queue_callbacks *callbacks) {
    if (!callbacks || !callbacks->reserve_task_rx || !callbacks->release_task_rx ||
        !callbacks->task_cancelled) return NULL;
    hop_writer_queue *queue = (hop_writer_queue *)calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->callback_context = callback_context;
    queue->callbacks = *callbacks;
    hop_xfer_buffer_pool_init(&queue->pool);
    hop_xfer_rx_queue_init(&queue->accounting, RX_Q_SLOTS, RX_Q_MAX_BYTES);
    hop_writer_io_init(&queue->io);
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->cv_work);
    InitializeConditionVariable(&queue->cv_space);
    const hop_writer_worker_callbacks worker_callbacks = {
        worker_take, worker_run, worker_complete, worker_exit
    };
    if (!hop_writer_worker_start(&queue->worker, queue, sizeof(writer_command),
                                 &worker_callbacks)) {
        DeleteCriticalSection(&queue->lock);
        hop_xfer_buffer_pool_dispose(&queue->pool);
        free(queue);
        return NULL;
    }
    return queue;
}

int hop_writer_queue_enqueue_open(hop_writer_queue *queue, const wchar_t *path) {
    return enqueue_control(queue, RX_OPEN, path);
}
int hop_writer_queue_enqueue_close(hop_writer_queue *queue) {
    return enqueue_control(queue, RX_CLOSE, NULL);
}

int hop_writer_queue_enqueue_write(hop_writer_queue *queue, void *task_owner,
                                   uint64_t offset, const uint8_t *data,
                                   uint32_t length) {
    if (!queue || !task_owner || !data || !length || length > RX_Q_MAX_BYTES)
        return 0;
    int guard = watchdog_begin(queue, "disk queue wait", 35000);
    EnterCriticalSection(&queue->lock);
    while (!hop_xfer_rx_queue_can_push(&queue->accounting, length) && !queue->stop)
        SleepConditionVariableCS(&queue->cv_space, &queue->lock, INFINITE);
    if (queue->stop || !hop_xfer_rx_queue_reserve(&queue->accounting, length)) {
        LeaveCriticalSection(&queue->lock);
        watchdog_end(queue, guard);
        return 0;
    }
    LeaveCriticalSection(&queue->lock);

    writer_command command;
    memset(&command, 0, sizeof(command));
    command.kind = RX_WRITE;
    command.offset = offset;
    command.length = length;
    command.task_owner = task_owner;
    command.pool_slot = -1;

    int task_reserved = queue->callbacks.reserve_task_rx(queue->callback_context,
                                                         task_owner, length);
    int pool_result = 0;
    EnterCriticalSection(&queue->lock);
    if (!task_reserved) goto rollback;

    while (!queue->stop &&
           (pool_result = hop_xfer_buffer_pool_acquire_locked(
                &queue->pool, length, &command.pool_slot, &command.data)) == 0)
        SleepConditionVariableCS(&queue->cv_space, &queue->lock, INFINITE);
    if (queue->stop || pool_result != 1) goto release_task;
    LeaveCriticalSection(&queue->lock);

    memcpy(command.data, data, length);

    EnterCriticalSection(&queue->lock);
    if (queue->stop || !hop_xfer_rx_queue_commit_reserved(&queue->accounting))
        goto release_task;
    queue->ring[queue->tail] = command;
    queue->tail = (queue->tail + 1) % RX_Q_SLOTS;
    WakeConditionVariable(&queue->cv_work);
    LeaveCriticalSection(&queue->lock);
    watchdog_end(queue, guard);
    return 1;

release_task:
    LeaveCriticalSection(&queue->lock);
    queue->callbacks.release_task_rx(queue->callback_context, task_owner, length);
    EnterCriticalSection(&queue->lock);
rollback:
    /* Keep the reservation charged until its Task budget is returned. */
    hop_xfer_buffer_pool_release_locked(&queue->pool, command.pool_slot);
    hop_xfer_rx_queue_rollback(&queue->accounting, length);
    WakeAllConditionVariable(&queue->cv_space);
    if (queue->stop) WakeAllConditionVariable(&queue->cv_work);
    LeaveCriticalSection(&queue->lock);
    watchdog_end(queue, guard);
    return 0;
}

void hop_writer_queue_drain(hop_writer_queue *queue) {
    if (!queue) return;
    int guard = watchdog_begin(queue, "disk queue wait", 35000);
    EnterCriticalSection(&queue->lock);
    while (!hop_xfer_rx_queue_drained(&queue->accounting))
        SleepConditionVariableCS(&queue->cv_space, &queue->lock, INFINITE);
    LeaveCriticalSection(&queue->lock);
    watchdog_end(queue, guard);
}

void hop_writer_queue_error_reset(hop_writer_queue *queue) {
    if (queue) InterlockedExchange(&queue->error, 0);
}
int hop_writer_queue_error(const hop_writer_queue *queue) {
    return queue ? InterlockedCompareExchange((volatile LONG *)&queue->error, 0, 0) : 1;
}

int hop_writer_queue_stop(hop_writer_queue *queue) {
    if (!queue) return 0;
    if (queue->stopped) return 1;
    EnterCriticalSection(&queue->lock);
    queue->stop = 1;
    WakeAllConditionVariable(&queue->cv_work);
    WakeAllConditionVariable(&queue->cv_space);
    LeaveCriticalSection(&queue->lock);
    if (!hop_writer_worker_join(&queue->worker)) return 0;
    hop_xfer_buffer_pool_dispose(&queue->pool);
    queue->stopped = 1;
    return 1;
}

void hop_writer_queue_destroy(hop_writer_queue *queue) {
    if (!queue || !queue->stopped) return;
    DeleteCriticalSection(&queue->lock);
    free(queue);
}
