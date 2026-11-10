#include "../src/core/xfer_sender_queue.h"

#include <assert.h>
#include <stdio.h>

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

static const hop_xfer_sender_queue_ops g_ops = {
    NULL, job_next, job_set_next, job_cancelled, job_batch,
    job_retry_at, job_give_up_at
};

static void queue_push(hop_xfer_sender_queue *queue, test_job *job) {
    job->next = NULL;
    if (queue->tail) ((test_job *)queue->tail)->next = job;
    else queue->head = job;
    queue->tail = job;
}

static void test_cancelled_middle_and_tail(void) {
    test_job a = {.batch = 1}, b = {.batch = 2}, c = {.batch = 3};
    hop_xfer_sender_queue queue = {0};
    queue_push(&queue, &a);
    queue_push(&queue, &b);
    queue_push(&queue, &c);

    b.cancelled = 1;
    assert(hop_xfer_sender_queue_take_cancelled(&queue, &g_ops) == &b);
    assert(queue.head == &a && queue.tail == &c);
    assert(a.next == &c && b.next == NULL);

    c.cancelled = 1;
    assert(hop_xfer_sender_queue_take_cancelled(&queue, &g_ops) == &c);
    assert(queue.head == &a && queue.tail == &a);
    assert(a.next == NULL && c.next == NULL);

    a.cancelled = 1;
    assert(hop_xfer_sender_queue_take_cancelled(&queue, &g_ops) == &a);
    assert(queue.head == NULL && queue.tail == NULL && a.next == NULL);
    assert(hop_xfer_sender_queue_take_cancelled(&queue, &g_ops) == NULL);
}

static void test_head_readiness_and_fifo(void) {
    test_job a = {.batch = 10, .retry_at = 100};
    test_job b = {.batch = 11, .retry_at = 0};
    hop_xfer_sender_queue queue = {0};
    hop_xfer_sender_queue_take_result result;

    queue_push(&queue, &a);
    queue_push(&queue, &b);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 99, 0, 1, 1, &result) == NULL);
    assert(result == HOP_XFER_SENDER_QUEUE_NONE);
    assert(queue.head == &a && queue.tail == &b);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 100, 0, 0, 1, &result) == NULL);
    assert(result == HOP_XFER_SENDER_QUEUE_NONE);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 100, 0, 1, 0, &result) == NULL);
    assert(result == HOP_XFER_SENDER_QUEUE_NONE);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 100, 0, 1, 1, &result) == &a);
    assert(result == HOP_XFER_SENDER_QUEUE_READY);
    assert(queue.head == &b && queue.tail == &b);
    assert(a.next == NULL);
}

static void test_expiry_while_offline(void) {
    test_job cancelled = {.batch = 20};
    test_job deadline = {.batch = 21, .give_up_at = 50};
    test_job ready = {.batch = 22, .retry_at = 0};
    hop_xfer_sender_queue queue = {0};
    hop_xfer_sender_queue_take_result result;

    queue_push(&queue, &cancelled);
    queue_push(&queue, &deadline);
    queue_push(&queue, &ready);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 1, 20, 0, 0, &result) == &cancelled);
    assert(result == HOP_XFER_SENDER_QUEUE_EXPIRED);
    assert(queue.head == &deadline && queue.tail == &ready);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 50, 0, 0, 0, &result) == &deadline);
    assert(result == HOP_XFER_SENDER_QUEUE_EXPIRED);
    assert(queue.head == &ready && queue.tail == &ready);

    assert(hop_xfer_sender_queue_take_head(
        &queue, &g_ops, 50, 0, 0, 1, &result) == NULL);
    assert(result == HOP_XFER_SENDER_QUEUE_NONE);
    assert(queue.head == &ready && queue.tail == &ready);
}

int main(void) {
    test_cancelled_middle_and_tail();
    test_head_readiness_and_fifo();
    test_expiry_while_offline();
    puts("PASS: shared Sender queue cancellation, FIFO, retry gate and expiry policy");
    return 0;
}
