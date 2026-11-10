#include "../src/core/xfer_sender_context.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now;
    uint32_t last_batch;
    uint64_t last_task;
    int stopping;
    int cancelled;
    int aborted;
    int writes;
} fake_sender;

static uint64_t fake_now(void *user) {
    return ((fake_sender *)user)->now;
}
static int fake_wait(void *user, uint64_t duration_ns) {
    fake_sender *f = (fake_sender *)user;
    f->now += duration_ns;
    return !f->stopping;
}
static int fake_stopping(void *user) {
    return ((fake_sender *)user)->stopping;
}
static int fake_session_current(void *user, intptr_t socket_handle, uint64_t epoch) {
    (void)user;
    return socket_handle != (intptr_t)-1 && epoch != 0;
}
static int fake_write_frame(void *user, intptr_t socket_handle, uint64_t epoch,
                            const uint8_t *frame, size_t frame_size) {
    fake_sender *f = (fake_sender *)user;
    if (socket_handle == (intptr_t)-1 || !epoch || (!frame && frame_size)) return 0;
    f->writes++;
    return 1;
}
static int fake_cancelled(void *user, uint32_t batch, uint64_t task_id) {
    fake_sender *f = (fake_sender *)user;
    f->last_batch = batch;
    f->last_task = task_id;
    return f->cancelled;
}
static int fake_peer_aborted(void *user, uint32_t batch) {
    fake_sender *f = (fake_sender *)user;
    f->last_batch = batch;
    return f->aborted;
}
static void fake_stage_time(void *user, uint32_t batch, int stage, uint64_t ns) {
    (void)user; (void)batch; (void)stage; (void)ns;
}
static void fake_log(void *user, int level, const char *message) {
    (void)user; (void)level; (void)message;
}
static int fake_task_state(void *user, uint64_t task_id, int state) {
    (void)user; (void)task_id; (void)state; return 1;
}
static int fake_task_completion(void *user, uint64_t task_id) {
    (void)user; (void)task_id; return 1;
}
static void fake_task_finish(void *user, uint64_t task_id, int outcome) {
    (void)user; (void)task_id; (void)outcome;
}

static hop_xfer_sender_context make_context(fake_sender *f) {
    hop_xfer_sender_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.user = f;
    ctx.now_ns = fake_now;
    ctx.wait_ns = fake_wait;
    ctx.is_stopping = fake_stopping;
    ctx.session_current = fake_session_current;
    ctx.write_frame = fake_write_frame;
    ctx.is_cancelled = fake_cancelled;
    ctx.peer_aborted = fake_peer_aborted;
    ctx.status_stage_time = fake_stage_time;
    ctx.log_message = fake_log;
    ctx.task_set_state = fake_task_state;
    ctx.task_begin_completion = fake_task_completion;
    ctx.task_finish = fake_task_finish;
    return ctx;
}

int main(void) {
    fake_sender state;
    memset(&state, 0, sizeof(state));
    hop_xfer_sender_context ctx = make_context(&state);

    /* The platform sender has adopted the clock/wait subset first; the full
     * execution contract remains separately validated for the next slice. */
    hop_xfer_sender_context clock_only;
    memset(&clock_only, 0, sizeof(clock_only));
    clock_only.user = &state;
    clock_only.now_ns = fake_now;
    clock_only.wait_ns = fake_wait;
    clock_only.is_stopping = fake_stopping;
    assert(hop_xfer_sender_context_clock_valid(&clock_only));
    assert(!hop_xfer_sender_context_valid(&clock_only));
    clock_only.wait_ns = NULL;
    assert(!hop_xfer_sender_context_clock_valid(&clock_only));

    assert(hop_xfer_sender_context_clock_valid(&ctx));
    assert(hop_xfer_sender_context_valid(&ctx));
    assert(ctx.now_ns(ctx.user) == 0);
    assert(ctx.wait_ns(ctx.user, 250) == 1);
    assert(ctx.now_ns(ctx.user) == 250);
    state.stopping = 1;
    assert(ctx.is_stopping(ctx.user));
    assert(!ctx.wait_ns(ctx.user, 10));
    state.stopping = 0;
    assert(!ctx.is_stopping(ctx.user));
    assert(ctx.session_current(ctx.user, 7, 3));
    assert(!ctx.session_current(ctx.user, (intptr_t)-1, 3));
    const uint8_t frame[] = {1, 2, 3};
    assert(ctx.write_frame(ctx.user, 7, 3, frame, sizeof(frame)));
    assert(state.writes == 1);
    assert(ctx.is_cancelled(ctx.user, 42, 99) == 0);
    assert(state.last_batch == 42 && state.last_task == 99);
    state.cancelled = 1;
    assert(ctx.is_cancelled(ctx.user, 42, 99) == 1);
    state.aborted = 1;
    assert(ctx.peer_aborted(ctx.user, 42) == 1);
    assert(ctx.task_set_state(ctx.user, 99, 1));
    assert(ctx.task_begin_completion(ctx.user, 99));
    ctx.task_finish(ctx.user, 99, 2);

    /* Observability is optional and must not make a valid execution context
     * invalid. Every execution/lifecycle dependency remains mandatory. */
    ctx.status_stage_time = NULL;
    ctx.log_message = NULL;
    assert(hop_xfer_sender_context_valid(&ctx));

#define REQUIRE_INVALID(field) do { \
    hop_xfer_sender_context broken = ctx; \
    broken.field = NULL; \
    assert(!hop_xfer_sender_context_valid(&broken)); \
} while (0)
    REQUIRE_INVALID(now_ns);
    REQUIRE_INVALID(wait_ns);
    REQUIRE_INVALID(is_stopping);
    REQUIRE_INVALID(session_current);
    REQUIRE_INVALID(write_frame);
    REQUIRE_INVALID(is_cancelled);
    REQUIRE_INVALID(peer_aborted);
    REQUIRE_INVALID(task_set_state);
    REQUIRE_INVALID(task_begin_completion);
    REQUIRE_INVALID(task_finish);
#undef REQUIRE_INVALID

    assert(!hop_xfer_sender_context_valid(NULL));
    puts("PASS: sender context contract");
    return 0;
}
