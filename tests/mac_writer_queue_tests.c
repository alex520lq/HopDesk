#include "xfer_writer_queue.h"
#include "xfer_budget.h"

#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <pthread.h>
#include "writer_io_fault_override.h"
#undef open
#undef write

static atomic_int io_fault;
int hop_test_open(const char *path, int flags, mode_t mode) {
    int expected = 2;
    if (atomic_compare_exchange_strong(&io_fault, &expected, 0)) { errno = EACCES; return -1; }
    return open(path, flags, mode);
}
ssize_t hop_test_write(int fd, const void *data, size_t len) {
    int fault = atomic_load(&io_fault);
    if (fault == 1) {
        int expected = 1;
        if (atomic_compare_exchange_strong(&io_fault, &expected, 3))
            return write(fd, data, len > 1 ? len / 2 : len);
    } else if (fault == 3) {
        atomic_store(&io_fault, 0); errno = ENOSPC; return -1;
    }
    return write(fd, data, len);
}

static atomic_int fail_pool_malloc_once;
void *hop_test_pool_malloc(size_t size) {
    if (atomic_exchange(&fail_pool_malloc_once, 0)) return NULL;
    return malloc(size);
}

typedef struct {
    atomic_uint held_bytes;
    atomic_uint reserved_calls;
    atomic_uint released_calls;
    atomic_uint write_time_calls;
    atomic_uint open_errors;
    atomic_uint write_errors;
    atomic_int last_error;
    int reject_reservation;
    atomic_int cancel_writes;
    pthread_mutex_t gate_lock;
    pthread_cond_t gate_cv;
    int pause_reserve, reserve_entered, release_reserve;
    int pause_worker, worker_entered, release_worker;
    int enqueue_done;
    int stop_done, stop_result;
    hop_writer_queue *stop_queue;
} test_context;

static int reserve_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (!owner || ctx->reject_reservation) return 0;
    pthread_mutex_lock(&ctx->gate_lock);
    if (ctx->pause_reserve) {
        ctx->reserve_entered = 1;
        pthread_cond_broadcast(&ctx->gate_cv);
        while (!ctx->release_reserve) pthread_cond_wait(&ctx->gate_cv, &ctx->gate_lock);
    }
    pthread_mutex_unlock(&ctx->gate_lock);
    atomic_fetch_add(&ctx->held_bytes, bytes);
    atomic_fetch_add(&ctx->reserved_calls, 1);
    return 1;
}
static int task_cancelled(void *opaque, void *owner) {
    test_context *ctx = (test_context *)opaque;
    pthread_mutex_lock(&ctx->gate_lock);
    if (ctx->pause_worker) {
        ctx->worker_entered = 1;
        pthread_cond_broadcast(&ctx->gate_cv);
        while (!ctx->release_worker) pthread_cond_wait(&ctx->gate_cv, &ctx->gate_lock);
    }
    pthread_mutex_unlock(&ctx->gate_lock);
    return owner && atomic_load(&ctx->cancel_writes);
}
static void release_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (owner) {
        atomic_fetch_sub(&ctx->held_bytes, bytes);
        atomic_fetch_add(&ctx->released_calls, 1);
    }
}
static int watchdog_begin(void *opaque, const char *label, uint32_t deadline) {
    (void)opaque; (void)label; (void)deadline;
    return 1;
}
static void watchdog_end(void *opaque, int guard) {
    (void)opaque; (void)guard;
}
static uint64_t now_ns(void *opaque) {
    struct timespec now;
    (void)opaque;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
static void write_time(void *opaque, void *owner, uint64_t elapsed) {
    test_context *ctx = (test_context *)opaque;
    if (owner && elapsed < UINT64_MAX) atomic_fetch_add(&ctx->write_time_calls, 1);
}
static void open_error(void *opaque, const char *path, int error) {
    test_context *ctx = (test_context *)opaque;
    (void)path; atomic_store(&ctx->last_error, error);
    atomic_fetch_add(&ctx->open_errors, 1);
}
static void write_error(void *opaque, uint64_t offset, int error) {
    test_context *ctx = (test_context *)opaque;
    (void)offset; atomic_store(&ctx->last_error, error);
    atomic_fetch_add(&ctx->write_errors, 1);
}


typedef struct { hop_writer_queue *queue; int result; void *owner; const uint8_t *data; uint32_t length; test_context *ctx; } enqueue_args;
static void *enqueue_thread(void *opaque) {
    enqueue_args *args = (enqueue_args *)opaque;
    static const uint8_t bytes[] = {7, 8, 9};
    int owner = 1;
    args->result = hop_writer_queue_enqueue_write(args->queue,
        args->owner ? args->owner : &owner, 0, args->data ? args->data : bytes,
        args->length ? args->length : (uint32_t)sizeof(bytes));
    if (args->ctx) {
        pthread_mutex_lock(&args->ctx->gate_lock);
        args->ctx->enqueue_done = 1;
        pthread_cond_broadcast(&args->ctx->gate_cv);
        pthread_mutex_unlock(&args->ctx->gate_lock);
    }
    return NULL;
}
static void *stop_thread(void *opaque) {
    test_context *ctx = (test_context *)opaque;
    int result = hop_writer_queue_stop(ctx->stop_queue);
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->stop_result = result;
    ctx->stop_done = 1;
    pthread_cond_broadcast(&ctx->gate_cv);
    pthread_mutex_unlock(&ctx->gate_lock);
    return NULL;
}
static int wait_flag(test_context *ctx, int *flag, int seconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    pthread_mutex_lock(&ctx->gate_lock);
    while (!*flag) {
        int rc = pthread_cond_timedwait(&ctx->gate_cv, &ctx->gate_lock, &deadline);
        if (rc != 0) { pthread_mutex_unlock(&ctx->gate_lock); return 0; }
    }
    pthread_mutex_unlock(&ctx->gate_lock);
    return 1;
}

static int run_stop_reservation_race(hop_writer_queue *queue, test_context *ctx) {
    unsigned released_before = atomic_load(&ctx->released_calls);
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->pause_reserve = 1;
    pthread_mutex_unlock(&ctx->gate_lock);
    enqueue_args enqueue = {queue, 0, NULL, NULL, 0, ctx};
    pthread_t producer, stopper;
    int producer_started = 0, stopper_started = 0, ok = 0;
    if (pthread_create(&producer, NULL, enqueue_thread, &enqueue) != 0) goto done;
    producer_started = 1;
    if (!wait_flag(ctx, &ctx->reserve_entered, 5)) goto done;
    ctx->stop_queue = queue;
    if (pthread_create(&stopper, NULL, stop_thread, ctx) != 0) goto done;
    stopper_started = 1;
    if (!wait_flag(ctx, &ctx->stop_done, 5)) goto done;
    pthread_join(stopper, NULL); stopper_started = 0;
    if (ctx->stop_result || hop_writer_queue_enqueue_close(queue)) goto done;
    ok = 1;
done:
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->release_reserve = 1;
    pthread_cond_broadcast(&ctx->gate_cv);
    pthread_mutex_unlock(&ctx->gate_lock);
    if (producer_started) pthread_join(producer, NULL);
    if (stopper_started) pthread_join(stopper, NULL);
    return ok && !enqueue.result && atomic_load(&ctx->held_bytes) == 0 &&
        atomic_load(&ctx->released_calls) == released_before + 1 && hop_writer_queue_stop(queue);
}

static int run_full_queue_stop(hop_writer_queue *queue, test_context *ctx,
                               const char *path, void *owner) {
    unsigned released_before = atomic_load(&ctx->released_calls);
    unsigned reserved_before = atomic_load(&ctx->reserved_calls);
    unsigned char bytes[1024] = {0};
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->pause_worker = 1;
    ctx->enqueue_done = 0;
    pthread_mutex_unlock(&ctx->gate_lock);
    enqueue_args blocked = {queue, 1, NULL, NULL, 0, ctx};
    pthread_t producer, stopper;
    int producer_started = 0, stopper_started = 0, ok = 0;
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, owner, 0, bytes, sizeof(bytes)) ||
        !wait_flag(ctx, &ctx->worker_entered, 5)) goto done;
    for (unsigned i = 1; i < HOP_XFER_DEFAULT_RX_QUEUE_SLOTS; ++i)
        if (!hop_writer_queue_enqueue_write(queue, owner, (uint64_t)i * sizeof(bytes),
                                            bytes, sizeof(bytes))) goto done;
    if (pthread_create(&producer, NULL, enqueue_thread, &blocked) != 0) goto done;
    producer_started = 1;
    ctx->stop_queue = queue;
    if (pthread_create(&stopper, NULL, stop_thread, ctx) != 0) goto done;
    stopper_started = 1;
    /* The stop closes admission and wakes the producer while the worker is
       held in task_cancelled. Its bounded join cannot finish until released. */
    if (!wait_flag(ctx, &ctx->stop_done, 5) || ctx->stop_result ||
        !wait_flag(ctx, &ctx->enqueue_done, 5) || blocked.result) goto done;
    ok = 1;
done:
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->release_worker = 1;
    pthread_cond_broadcast(&ctx->gate_cv);
    pthread_mutex_unlock(&ctx->gate_lock);
    if (producer_started) pthread_join(producer, NULL);
    if (stopper_started) { pthread_join(stopper, NULL); stopper_started = 0; }
    int stopped = hop_writer_queue_stop(queue);
    int result = ok && !blocked.result && stopped &&
        atomic_load(&ctx->reserved_calls) == reserved_before + HOP_XFER_DEFAULT_RX_QUEUE_SLOTS &&
        atomic_load(&ctx->released_calls) == released_before + HOP_XFER_DEFAULT_RX_QUEUE_SLOTS &&
        atomic_load(&ctx->held_bytes) == 0;
    if (!result) fprintf(stderr, "full queue: ok=%d blocked=%d stop=%d reserve=%u/%u release=%u/%u held=%u\n",
        ok, blocked.result, ctx->stop_result, atomic_load(&ctx->reserved_calls), reserved_before,
        atomic_load(&ctx->released_calls), released_before, atomic_load(&ctx->held_bytes));
    return result;
}

static int run_byte_backpressure(hop_writer_queue *queue, test_context *ctx,
                                 const char *path, void *owner) {
    const uint32_t first_len = 7u * 1024u * 1024u;
    const uint32_t blocked_len = 2u * 1024u * 1024u;
    uint8_t *first = calloc(1, first_len);
    uint8_t *blocked_data = calloc(1, blocked_len);
    if (!first || !blocked_data) { free(first); free(blocked_data); return 0; }
    unsigned reserved_before = atomic_load(&ctx->reserved_calls);
    unsigned released_before = atomic_load(&ctx->released_calls);
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->pause_worker = 1;
    ctx->enqueue_done = 0;
    pthread_mutex_unlock(&ctx->gate_lock);
    int ok = hop_writer_queue_enqueue_open(queue, path) &&
        hop_writer_queue_enqueue_write(queue, owner, 0, first, first_len) &&
        wait_flag(ctx, &ctx->worker_entered, 5);
    enqueue_args blocked = {queue, 1, owner, blocked_data, blocked_len, ctx};
    pthread_t producer;
    pthread_t stopper;
    int started = 0, stop_started = 0;
    if (!ok || pthread_create(&producer, NULL, enqueue_thread, &blocked) != 0) { ok = 0; goto done; }
    started = 1;
    ctx->stop_queue = queue;
    if (pthread_create(&stopper, NULL, stop_thread, ctx) != 0) goto done;
    stop_started = 1;
    if (!wait_flag(ctx, &ctx->stop_done, 5) || ctx->stop_result ||
        !wait_flag(ctx, &ctx->enqueue_done, 5) || blocked.result) ok = 0;
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->release_worker = 1;
    pthread_cond_broadcast(&ctx->gate_cv);
    pthread_mutex_unlock(&ctx->gate_lock);
    pthread_join(producer, NULL); started = 0;
    pthread_join(stopper, NULL); stop_started = 0;
    ok = ok && !blocked.result && hop_writer_queue_stop(queue) &&
        atomic_load(&ctx->reserved_calls) == reserved_before + 1 &&
        atomic_load(&ctx->released_calls) == released_before + 1 &&
        atomic_load(&ctx->held_bytes) == 0;
done:
    pthread_mutex_lock(&ctx->gate_lock);
    ctx->release_worker = 1;
    pthread_cond_broadcast(&ctx->gate_cv);
    pthread_mutex_unlock(&ctx->gate_lock);
    if (stop_started) pthread_join(stopper, NULL);
    if (started) pthread_join(producer, NULL);
    free(first);
    free(blocked_data);
    return ok;
}

static int read_exact(const char *path, const unsigned char *expected, size_t length) {
    unsigned char actual[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, actual + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return 0; }
        done += (size_t)n;
    }
    unsigned char extra;
    ssize_t tail = read(fd, &extra, 1);
    close(fd);
    return tail == 0 && memcmp(actual, expected, length) == 0;
}

int main(void) {
    test_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    atomic_init(&ctx.held_bytes, 0);
    atomic_init(&ctx.reserved_calls, 0);
    atomic_init(&ctx.released_calls, 0);
    atomic_init(&ctx.write_time_calls, 0);
    atomic_init(&ctx.open_errors, 0);
    atomic_init(&ctx.write_errors, 0);
    atomic_init(&ctx.last_error, 0);
    atomic_init(&ctx.cancel_writes, 0);
    atomic_init(&fail_pool_malloc_once, 0);
    atomic_init(&io_fault, 0);
    pthread_mutex_init(&ctx.gate_lock, NULL);
    pthread_cond_init(&ctx.gate_cv, NULL);
    const hop_writer_queue_callbacks callbacks = {
        30000, reserve_rx, release_rx, task_cancelled, watchdog_begin, watchdog_end,
        now_ns, write_time, open_error, write_error
    };
    int task_token = 1;
    char path[] = "/tmp/hopdesk-writer-queue-XXXXXX";
    int temp = mkstemp(path);
    if (temp < 0) {
        perror("mkstemp");
        return 1;
    }
    close(temp);
    unlink(path);

    hop_writer_queue *queue = hop_writer_queue_create(&ctx, &callbacks);
    if (!queue) {
        fputs("FAIL: queue create\n", stderr);
        return 1;
    }
    const unsigned char first[] = {1, 2, 3, 4};
    const unsigned char second[] = {5, 6, 7, 8, 9};
    const unsigned char expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    if (!hop_writer_queue_enqueue_open(queue, path)) goto fail;
    ctx.reject_reservation = 1;
    if (hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    ctx.reject_reservation = 0;
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    if (!hop_writer_queue_enqueue_write(queue, &task_token, sizeof(first), second, sizeof(second))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    if (!read_exact(path, expected, sizeof(expected))) goto fail;
    if (atomic_load(&ctx.held_bytes) != 0 ||
        atomic_load(&ctx.reserved_calls) != 2 ||
        atomic_load(&ctx.released_calls) != 2 ||
        atomic_load(&ctx.write_time_calls) != 2 ||
        hop_writer_queue_error(queue)) goto fail;

    /* Fail the real buffer-pool allocation once; both reservations must roll back. */
    unsigned reserves_before = atomic_load(&ctx.reserved_calls);
    unsigned releases_before = atomic_load(&ctx.released_calls);
    unsigned held_before = atomic_load(&ctx.held_bytes);
    unsigned char large[128] = {0};
    atomic_store(&fail_pool_malloc_once, 1);
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        hop_writer_queue_enqueue_write(queue, &task_token, 9, large, sizeof(large))) goto fail;
    hop_writer_queue_drain(queue);
    if (atomic_load(&ctx.reserved_calls) != reserves_before + 1 ||
        atomic_load(&ctx.released_calls) != releases_before + 1 ||
        atomic_load(&ctx.held_bytes) != held_before || hop_writer_queue_error(queue)) goto fail;
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 9, large, sizeof(large))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    if (atomic_load(&ctx.held_bytes) != 0 || hop_writer_queue_error(queue)) goto fail;
    /* A cancelled Task may have a command accepted just before the worker runs;
     * the worker must skip the disk write and still release its exact budget. */
    char cancelled_path[] = "/tmp/hopdesk-writer-cancel-XXXXXX";
    int cancelled_fd = mkstemp(cancelled_path);
    if (cancelled_fd < 0) goto fail;
    close(cancelled_fd);
    unlink(cancelled_path);
    if (!hop_writer_queue_enqueue_open(queue, cancelled_path)) goto fail;
    atomic_store(&ctx.cancel_writes, 1);
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    struct stat cancelled_stat;
    if (stat(cancelled_path, &cancelled_stat) != 0 || cancelled_stat.st_size != 0) goto fail;
    atomic_store(&ctx.cancel_writes, 0);
    unlink(cancelled_path);
    /* A write without OPEN fails; a later successful write must not clear the sticky error. */
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.write_errors) != 1) goto fail;
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue)) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    hop_writer_queue_error_reset(queue);
    if (hop_writer_queue_error(queue)) goto fail;

    /* Inject access denied at the real native open syscall; the adapter and
       production queue still own error propagation and cleanup. */
    atomic_store(&io_fault, 2);
    if (!hop_writer_queue_enqueue_open(queue, path)) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.last_error) != EACCES ||
        !hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.held_bytes) != 0) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    hop_writer_queue_error_reset(queue);
    if (hop_writer_queue_error(queue)) goto fail;

    /* Fail after a real partial write, then verify ENOSPC release and retry. */
    atomic_store(&io_fault, 1);
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, second, sizeof(second))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.last_error) != ENOSPC ||
        atomic_load(&ctx.held_bytes) != 0) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    hop_writer_queue_error_reset(queue);
    if (hop_writer_queue_error(queue) ||
        !hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (hop_writer_queue_error(queue) || atomic_load(&ctx.held_bytes) != 0) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);

    /* A real native open failure persists after a later successful OPEN/WRITE. */
    char denied_path[4096];
    if (snprintf(denied_path, sizeof(denied_path), "%s/no-such-parent/file", path) < 0 ||
        !hop_writer_queue_enqueue_open(queue, denied_path)) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.open_errors) != 2 ||
        !hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) || atomic_load(&ctx.held_bytes) != 0) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    hop_writer_queue_error_reset(queue);
    if (hop_writer_queue_error(queue)) goto fail;

    /* STOPPING must time out while a producer owns a reservation callback, then retry cleanly. */
    if (!run_stop_reservation_race(queue, &ctx)) goto fail;
    if (hop_writer_queue_enqueue_close(queue) || !hop_writer_queue_stop(queue)) goto fail;
    hop_writer_queue_destroy(queue);
    queue = hop_writer_queue_create(&ctx, &callbacks);
    pthread_mutex_lock(&ctx.gate_lock);
    ctx.stop_done = 0;
    ctx.pause_worker = 0;
    ctx.worker_entered = 0;
    ctx.release_worker = 0;
    pthread_mutex_unlock(&ctx.gate_lock);
    if (!queue || !run_full_queue_stop(queue, &ctx, path, &task_token)) goto fail;
    hop_writer_queue_destroy(queue);
    queue = hop_writer_queue_create(&ctx, &callbacks);
    pthread_mutex_lock(&ctx.gate_lock);
    ctx.stop_done = 0; ctx.pause_worker = 0; ctx.worker_entered = 0; ctx.release_worker = 0;
    pthread_mutex_unlock(&ctx.gate_lock);
    if (!queue || !run_byte_backpressure(queue, &ctx, path, &task_token)) goto fail;
    hop_writer_queue_destroy(queue);
    unlink(path);
    pthread_cond_destroy(&ctx.gate_cv);
    pthread_mutex_destroy(&ctx.gate_lock);
    puts("PASS: macOS writer queue FIFO, sticky IO error, stop timeout/retry, full queue wake and exact release");
    return 0;

fail:
    hop_writer_queue_stop(queue);
    hop_writer_queue_destroy(queue);
    unlink(path);
    fputs("FAIL: macOS writer queue contract\n", stderr);
    return 1;
}
