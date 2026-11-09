#include "xfer_writer_queue.h"

#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>

typedef struct {
    atomic_uint held_bytes;
    atomic_uint reserved_calls;
    atomic_uint released_calls;
    atomic_uint write_time_calls;
    atomic_uint open_errors;
    atomic_uint write_errors;
    int reject_reservation;
    atomic_int cancel_writes;
} test_context;

static int reserve_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (!owner || ctx->reject_reservation) return 0;
    atomic_fetch_add(&ctx->held_bytes, bytes);
    atomic_fetch_add(&ctx->reserved_calls, 1);
    return 1;
}
static int task_cancelled(void *opaque, void *owner) {
    test_context *ctx = (test_context *)opaque;
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
    (void)path; (void)error;
    atomic_fetch_add(&ctx->open_errors, 1);
}
static void write_error(void *opaque, uint64_t offset, int error) {
    test_context *ctx = (test_context *)opaque;
    (void)offset; (void)error;
    atomic_fetch_add(&ctx->write_errors, 1);
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
    atomic_init(&ctx.cancel_writes, 0);
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
    if (!hop_writer_queue_stop(queue)) goto fail;
    hop_writer_queue_destroy(queue);
    unlink(path);
    puts("PASS: macOS writer queue FIFO writes, task-budget rollback/release, drain and stop");
    return 0;

fail:
    hop_writer_queue_stop(queue);
    hop_writer_queue_destroy(queue);
    unlink(path);
    fputs("FAIL: macOS writer queue contract\n", stderr);
    return 1;
}
