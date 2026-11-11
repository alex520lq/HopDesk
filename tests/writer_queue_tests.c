#include "xfer_writer_queue.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    volatile LONG held_bytes;
    volatile LONG reserved_calls;
    volatile LONG released_calls;
    volatile LONG write_time_calls;
    volatile LONG open_errors;
    volatile LONG write_errors;
    int reject_reservation;
    volatile LONG cancel_writes;
} test_context;

static int reserve_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (!owner || ctx->reject_reservation) return 0;
    InterlockedAdd(&ctx->held_bytes, (LONG)bytes);
    InterlockedIncrement(&ctx->reserved_calls);
    return 1;
}
static int task_cancelled(void *opaque, void *owner) {
    test_context *ctx = (test_context *)opaque;
    return owner && InterlockedCompareExchange(&ctx->cancel_writes, 0, 0) != 0;
}

static void release_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (owner) {
        InterlockedAdd(&ctx->held_bytes, -(LONG)bytes);
        InterlockedIncrement(&ctx->released_calls);
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
    LARGE_INTEGER value, frequency;
    (void)opaque;
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    uint64_t ticks = (uint64_t)value.QuadPart;
    uint64_t hz = (uint64_t)frequency.QuadPart;
    return (ticks / hz) * 1000000000ULL + (ticks % hz) * 1000000000ULL / hz;
}
static void write_time(void *opaque, void *owner, uint64_t elapsed) {
    test_context *ctx = (test_context *)opaque;
    if (owner && elapsed < UINT64_MAX) InterlockedIncrement(&ctx->write_time_calls);
}
static void open_error(void *opaque, const wchar_t *path, unsigned long error) {
    test_context *ctx = (test_context *)opaque;
    (void)path; (void)error;
    InterlockedIncrement(&ctx->open_errors);
}
static void write_error(void *opaque, uint64_t offset, unsigned long error) {
    test_context *ctx = (test_context *)opaque;
    (void)offset; (void)error;
    InterlockedIncrement(&ctx->write_errors);
}

static int read_exact(const wchar_t *path, const unsigned char *expected, DWORD length) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    unsigned char actual[64];
    DWORD got = 0;
    int ok = length <= sizeof(actual) &&
             ReadFile(file, actual, length, &got, NULL) && got == length &&
             memcmp(actual, expected, length) == 0;
    CloseHandle(file);
    return ok;
}

int main(void) {
    test_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    const hop_writer_queue_callbacks callbacks = {
        30000, reserve_rx, release_rx, task_cancelled, watchdog_begin, watchdog_end,
        now_ns, write_time, open_error, write_error
    };
    int task_token = 1;
    wchar_t temp_dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        !GetTempFileNameW(temp_dir, L"hdq", 0, path)) {
        fputs("FAIL: unable to create temporary file path\n", stderr);
        return 1;
    }
    DeleteFileW(path);

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
    if (InterlockedCompareExchange(&ctx.held_bytes, 0, 0) != 0 ||
        InterlockedCompareExchange(&ctx.reserved_calls, 0, 0) != 2 ||
        InterlockedCompareExchange(&ctx.released_calls, 0, 0) != 2 ||
        InterlockedCompareExchange(&ctx.write_time_calls, 0, 0) != 2 ||
        hop_writer_queue_error(queue)) goto fail;
    /* A cancelled Task may have a command accepted just before the worker runs;
     * the worker must skip the disk write and still release its exact budget. */
    wchar_t cancelled_path[MAX_PATH];
    if (!GetTempFileNameW(temp_dir, L"hdc", 0, cancelled_path)) goto fail;
    DeleteFileW(cancelled_path);
    if (!hop_writer_queue_enqueue_open(queue, cancelled_path)) goto fail;
    InterlockedExchange(&ctx.cancel_writes, 1);
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    HANDLE cancelled_file = CreateFileW(cancelled_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (cancelled_file == INVALID_HANDLE_VALUE) goto fail;
    LARGE_INTEGER cancelled_size;
    int cancelled_empty = GetFileSizeEx(cancelled_file, &cancelled_size) && cancelled_size.QuadPart == 0;
    CloseHandle(cancelled_file);
    if (!cancelled_empty) goto fail;
    InterlockedExchange(&ctx.cancel_writes, 0);
    DeleteFileW(cancelled_path);
    if (!hop_writer_queue_stop(queue)) goto fail;
    hop_writer_queue_destroy(queue);
    DeleteFileW(path);
    puts("PASS: Windows writer queue FIFO writes, task-budget rollback/release, drain and stop");
    return 0;

fail:
    hop_writer_queue_stop(queue);
    hop_writer_queue_destroy(queue);
    DeleteFileW(path);
    fputs("FAIL: Windows writer queue contract\n", stderr);
    return 1;
}
