#include "xfer_writer_worker.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

typedef struct {
    int next;
    int ran;
    int completed;
    int exited;
} test_context;

static int take_one(void *opaque, void *task_out) {
    test_context *ctx = (test_context *)opaque;
    if (ctx->next++) return 0;
    *(int *)task_out = 42;
    return 1;
}

static void run_one(void *opaque, const void *task) {
    test_context *ctx = (test_context *)opaque;
    if (*(const int *)task == 42) ctx->ran++;
}

static void complete_one(void *opaque, const void *task) {
    test_context *ctx = (test_context *)opaque;
    (void)task;
    ctx->completed++;
}

static void on_exit(void *opaque) {
    test_context *ctx = (test_context *)opaque;
    ctx->exited++;
}

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
#endif
}

static int take_slow(void *opaque, void *task_out) {
    (void)opaque;
    (void)task_out;
    sleep_ms(3200);
    return 0;
}

static int run_timeout_retry(hop_writer_worker *worker) {
    test_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    const hop_writer_worker_callbacks callbacks = {
        take_slow, run_one, complete_one, on_exit
    };
    if (!hop_writer_worker_start(worker, &ctx, sizeof(int), &callbacks)) return 0;
    if (hop_writer_worker_join(worker) != 0 || worker->state != 1 || !worker->task_storage) return 0;
    if (!hop_writer_worker_join(worker) || worker->state != 2 || worker->task_storage) return 0;
    return ctx.exited == 1;
}

static int run_once(hop_writer_worker *worker) {
    test_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    const hop_writer_worker_callbacks callbacks = {
        take_one, run_one, complete_one, on_exit
    };
    if (!hop_writer_worker_start(worker, &ctx, sizeof(int), &callbacks)) return 0;
    if (!hop_writer_worker_join(worker)) return 0;
    return ctx.next == 2 && ctx.ran == 1 && ctx.completed == 1 && ctx.exited == 1;
}

int main(void) {
    hop_writer_worker worker;
    memset(&worker, 0, sizeof(worker));
    if (!run_once(&worker) || !run_once(&worker)) {
        fputs("FAIL: worker dispatch, completion, join, or restart lifecycle\n", stderr);
        return 1;
    }
    if (!run_timeout_retry(&worker) || !run_once(&worker)) {
        fputs("FAIL: timed join did not preserve worker state for retry\n", stderr);
        return 1;
    }
    puts("PASS: writer worker dispatch, completion, timed join retry, and restart");
    return 0;
}
