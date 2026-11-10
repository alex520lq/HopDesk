#include "../src/core/xfer_sender_scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct test_job {
    struct test_job *next;
    uint32_t batch;
    uint64_t retry_at;
    uint64_t give_up_at;
    int cancelled;
} test_job;

static void *job_next(void *user, const void *node) {
    (void)user;
    return ((const test_job *)node)->next;
}
static void job_set_next(void *user, void *node, void *next) {
    (void)user;
    ((test_job *)node)->next = (test_job *)next;
}
static int job_cancelled(void *user, const void *node) {
    (void)user;
    return ((const test_job *)node)->cancelled;
}
static uint32_t job_batch(void *user, const void *node) {
    (void)user;
    return ((const test_job *)node)->batch;
}
static uint64_t job_retry_at(void *user, const void *node) {
    (void)user;
    return ((const test_job *)node)->retry_at;
}
static uint64_t job_give_up_at(void *user, const void *node) {
    (void)user;
    return ((const test_job *)node)->give_up_at;
}
static const hop_xfer_sender_queue_ops g_queue_ops = {
    NULL, job_next, job_set_next, job_cancelled, job_batch,
    job_retry_at, job_give_up_at
};

typedef struct {
    int locked;
    int stopping;
    int peer_ready;
    int fail_begin;
    int fail_wait;
    int set_peer_ready_after_wait;
    int stop_after_wait;
    uint64_t now_ms;
    uint64_t advance_ms;
    uint32_t cancel_batch;
    uint32_t waits;
    uint32_t claims;
    void *claimed;
    int claimed_expired;
} fake_adapter;

static int begin_pass(void *user, int *peer_ready, uint64_t *now_ms,
                      uint32_t *cancel_batch) {
    fake_adapter *f = (fake_adapter *)user;
    if (f->fail_begin || f->locked) return 0;
    f->locked = 1;
    *peer_ready = f->peer_ready;
    *now_ms = f->now_ms;
    *cancel_batch = f->cancel_batch;
    return 1;
}
static int is_stopping(void *user) {
    fake_adapter *f = (fake_adapter *)user;
    assert(f->locked);
    return f->stopping;
}
static int wait_for_work_locked(void *user, uint32_t timeout_ms) {
    fake_adapter *f = (fake_adapter *)user;
    assert(f->locked && timeout_ms > 0);
    f->waits++;
    if (f->fail_wait) return 0;
    f->now_ms += f->advance_ms;
    if (f->set_peer_ready_after_wait) f->peer_ready = 1;
    if (f->stop_after_wait) f->stopping = 1;
    return 1;
}
static void claim_job_locked(void *user, void *job, int expired) {
    fake_adapter *f = (fake_adapter *)user;
    assert(f->locked && job);
    f->claims++;
    f->claimed = job;
    f->claimed_expired = expired;
}
static void end_pass(void *user) {
    fake_adapter *f = (fake_adapter *)user;
    assert(f->locked);
    f->locked = 0;
}
static hop_xfer_sender_scheduler_ops make_ops(fake_adapter *f) {
    hop_xfer_sender_scheduler_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.begin_pass = begin_pass;
    ops.is_stopping = is_stopping;
    ops.wait_for_work_locked = wait_for_work_locked;
    ops.claim_job_locked = claim_job_locked;
    ops.end_pass = end_pass;
    return ops;
}
static void push(hop_xfer_sender_queue *queue, test_job *job) {
    assert(hop_xfer_sender_queue_push_back(queue, &g_queue_ops, job));
}

static void test_ready_head_claimed_without_wait(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 1, .retry_at = 10};
    fake_adapter f = {.peer_ready = 1, .now_ms = 10};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_READY);
    assert(selected == &a && f.claimed == &a && !f.claimed_expired);
    assert(!f.waits && f.claims == 1 && !f.locked);
    assert(hop_xfer_sender_queue_empty(&queue));
}

static void test_cancelled_middle_job_is_selected_offline(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 1, .retry_at = 500};
    test_job b = {.batch = 2, .retry_at = 500, .cancelled = 1};
    test_job c = {.batch = 3, .retry_at = 500};
    fake_adapter f = {.peer_ready = 0, .now_ms = 10};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a); push(&queue, &b); push(&queue, &c);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_EXPIRED);
    assert(selected == &b && f.claimed == &b && f.claimed_expired);
    assert(queue.head == &a && queue.tail == &c && a.next == &c);
    assert(!f.waits && !f.locked);
}

static void test_offline_wait_refreshes_peer_before_selection(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 4, .retry_at = 100};
    fake_adapter f = {
        .peer_ready = 0, .now_ms = 50, .advance_ms = 50,
        .set_peer_ready_after_wait = 1
    };
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_READY);
    assert(selected == &a && f.waits == 1 && f.claims == 1);
    assert(!f.locked && hop_xfer_sender_queue_empty(&queue));
}

static void test_batch_cancel_expires_offline(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 10, .retry_at = 500};
    fake_adapter f = {.peer_ready = 0, .now_ms = 1, .cancel_batch = 10};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_EXPIRED);
    assert(selected == &a && f.claimed_expired && !f.waits && !f.locked);
    assert(hop_xfer_sender_queue_empty(&queue));
}

static void test_deadline_expires_while_offline(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 5, .retry_at = 0, .give_up_at = 100};
    fake_adapter f = {.peer_ready = 0, .now_ms = 50, .advance_ms = 50};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_EXPIRED);
    assert(selected == &a && f.waits == 1 && f.claimed_expired);
    assert(!f.locked && hop_xfer_sender_queue_empty(&queue));
}

static void test_fifo_head_is_not_overtaken(void) {
    hop_xfer_sender_queue queue = {0};
    test_job head = {.batch = 6, .retry_at = 100};
    test_job later = {.batch = 7, .retry_at = 0};
    fake_adapter f = {.peer_ready = 1, .now_ms = 50, .advance_ms = 50};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &head); push(&queue, &later);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_READY);
    assert(selected == &head && f.waits == 1);
    assert(queue.head == &later && queue.tail == &later);
    assert(!f.locked);
}

static void test_stop_after_wait_leaves_queue_owned_by_shutdown(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 8, .retry_at = 500};
    fake_adapter f = {.peer_ready = 0, .now_ms = 1, .stop_after_wait = 1};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = (void *)1;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_STOPPED);
    assert(selected == NULL && f.waits == 1 && !f.claims);
    assert(queue.head == &a && queue.tail == &a && !f.locked);
}

static void test_wait_failure_releases_native_lock(void) {
    hop_xfer_sender_queue queue = {0};
    test_job a = {.batch = 9, .retry_at = 500};
    fake_adapter f = {.peer_ready = 0, .fail_wait = 1};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    push(&queue, &a);
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_ERROR);
    assert(!selected && !f.locked && !f.claims);
    assert(queue.head == &a);
}

static void test_begin_pass_failure_does_not_leave_lock(void) {
    hop_xfer_sender_queue queue = {0};
    fake_adapter f = {.fail_begin = 1};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    void *selected = NULL;
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_ERROR);
    assert(!selected && !f.locked && !f.waits);
}

static void test_invalid_contract_does_not_lock(void) {
    hop_xfer_sender_queue queue = {0};
    fake_adapter f = {0};
    hop_xfer_sender_scheduler_ops ops = make_ops(&f);
    ops.end_pass = NULL;
    void *selected = NULL;
    assert(hop_xfer_sender_scheduler_wait_next(
        &queue, &g_queue_ops, &ops, 200, &selected) ==
        HOP_XFER_SENDER_SCHEDULER_ERROR);
    assert(!f.locked && !selected);
}

int main(void) {
    test_ready_head_claimed_without_wait();
    test_cancelled_middle_job_is_selected_offline();
    test_offline_wait_refreshes_peer_before_selection();
    test_deadline_expires_while_offline();
    test_batch_cancel_expires_offline();
    test_fifo_head_is_not_overtaken();
    test_stop_after_wait_leaves_queue_owned_by_shutdown();
    test_wait_failure_releases_native_lock();
    test_begin_pass_failure_does_not_leave_lock();
    test_invalid_contract_does_not_lock();
    puts("PASS: Sender scheduler wait, FIFO, cancellation, expiry and ownership");
    return 0;
}
