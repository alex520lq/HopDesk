#include "xfer_writer_queue.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile LONG fail_pool_malloc_once;
void *hop_test_pool_malloc(size_t size) {
    if (InterlockedExchange(&fail_pool_malloc_once, 0)) return NULL;
    return malloc(size);
}

typedef struct {
    volatile LONG held_bytes;
    volatile LONG reserved_calls;
    volatile LONG released_calls;
    volatile LONG write_time_calls;
    volatile LONG open_errors;
    volatile LONG write_errors;
    int reject_reservation;
    volatile LONG cancel_writes;
    HANDLE reserve_entered, release_reserve, stop_done;
    volatile LONG pause_reserve, stop_result;
    hop_writer_queue *stop_queue;
} test_context;

static int reserve_rx(void *opaque, void *owner, uint32_t bytes) {
    test_context *ctx = (test_context *)opaque;
    if (!owner || ctx->reject_reservation) return 0;
    if (InterlockedCompareExchange(&ctx->pause_reserve, 0, 0)) {
        SetEvent(ctx->reserve_entered);
        WaitForSingleObject(ctx->release_reserve, INFINITE);
    }
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


typedef struct { hop_writer_queue *queue; int result; } enqueue_args;
static DWORD WINAPI enqueue_thread(LPVOID opaque) {
    enqueue_args *args = (enqueue_args *)opaque;
    static const uint8_t bytes[] = {7, 8, 9};
    int owner = 1;
    args->result = hop_writer_queue_enqueue_write(args->queue, &owner, 0, bytes, sizeof(bytes));
    return 0;
}
static DWORD WINAPI stop_thread(LPVOID opaque) {
    test_context *ctx = (test_context *)opaque;
    InterlockedExchange(&ctx->stop_result, hop_writer_queue_stop(ctx->stop_queue));
    SetEvent(ctx->stop_done);
    return 0;
}

static int run_stop_reservation_race(hop_writer_queue *queue, test_context *ctx) {
    LONG released_before = InterlockedCompareExchange(&ctx->released_calls, 0, 0);
    InterlockedExchange(&ctx->pause_reserve, 1);
    enqueue_args enqueue = {queue, 0};
    HANDLE producer = CreateThread(NULL, 0, enqueue_thread, &enqueue, 0, NULL);
    HANDLE stopper = NULL;
    int ok = 0;
    if (!producer || WaitForSingleObject(ctx->reserve_entered, 5000) != WAIT_OBJECT_0) goto done;
    ctx->stop_queue = queue;
    stopper = CreateThread(NULL, 0, stop_thread, ctx, 0, NULL);
    if (!stopper || WaitForSingleObject(ctx->stop_done, 5000) != WAIT_OBJECT_0) goto done;
    if (InterlockedCompareExchange(&ctx->stop_result, 0, 0) ||
        hop_writer_queue_enqueue_close(queue)) goto done;
    ok = 1;
done:
    SetEvent(ctx->release_reserve);
    if (producer) { WaitForSingleObject(producer, INFINITE); CloseHandle(producer); }
    if (stopper) { WaitForSingleObject(stopper, INFINITE); CloseHandle(stopper); }
    return ok && !enqueue.result &&
        InterlockedCompareExchange(&ctx->held_bytes, 0, 0) == 0 &&
        InterlockedCompareExchange(&ctx->released_calls, 0, 0) == released_before + 1 &&
        hop_writer_queue_stop(queue);
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
    ctx.reserve_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx.release_reserve = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx.stop_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ctx.reserve_entered || !ctx.release_reserve || !ctx.stop_done) return 1;
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

    /* Fail the real buffer-pool allocation once; both reservations must roll back. */
    LONG reserves_before = InterlockedCompareExchange(&ctx.reserved_calls, 0, 0);
    LONG releases_before = InterlockedCompareExchange(&ctx.released_calls, 0, 0);
    LONG held_before = InterlockedCompareExchange(&ctx.held_bytes, 0, 0);
    unsigned char large[128] = {0};
    InterlockedExchange(&fail_pool_malloc_once, 1);
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        hop_writer_queue_enqueue_write(queue, &task_token, 9, large, sizeof(large))) goto fail;
    hop_writer_queue_drain(queue);
    if (InterlockedCompareExchange(&ctx.reserved_calls, 0, 0) != reserves_before + 1 ||
        InterlockedCompareExchange(&ctx.released_calls, 0, 0) != releases_before + 1 ||
        InterlockedCompareExchange(&ctx.held_bytes, 0, 0) != held_before ||
        hop_writer_queue_error(queue)) goto fail;
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 9, large, sizeof(large))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    if (InterlockedCompareExchange(&ctx.held_bytes, 0, 0) != 0 || hop_writer_queue_error(queue)) goto fail;
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
    /* A write without OPEN fails; a later successful write must not clear the sticky error. */
    if (!hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue) ||
        InterlockedCompareExchange(&ctx.write_errors, 0, 0) != 1) goto fail;
    if (!hop_writer_queue_enqueue_open(queue, path) ||
        !hop_writer_queue_enqueue_write(queue, &task_token, 0, first, sizeof(first))) goto fail;
    hop_writer_queue_drain(queue);
    if (!hop_writer_queue_error(queue)) goto fail;
    if (!hop_writer_queue_enqueue_close(queue)) goto fail;
    hop_writer_queue_drain(queue);
    hop_writer_queue_error_reset(queue);
    if (hop_writer_queue_error(queue)) goto fail;

    /* STOPPING must time out while a producer owns a reservation callback, then retry cleanly. */
    if (!run_stop_reservation_race(queue, &ctx)) goto fail;
    if (hop_writer_queue_enqueue_close(queue) || !hop_writer_queue_stop(queue)) goto fail;
    hop_writer_queue_destroy(queue);
    DeleteFileW(path);
    CloseHandle(ctx.stop_done); CloseHandle(ctx.release_reserve); CloseHandle(ctx.reserve_entered);
    puts("PASS: Windows writer queue FIFO, sticky IO error, stop timeout/retry and reservation release");
    return 0;

fail:
    hop_writer_queue_stop(queue);
    hop_writer_queue_destroy(queue);
    DeleteFileW(path);
    fputs("FAIL: Windows writer queue contract\n", stderr);
    return 1;
}
