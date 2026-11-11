/* Focused regression tests for the portable RX queue accounting module. */
#include "xfer_rx_queue.h"

#include <stdio.h>

static int failures;
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static void test_capacity_boundaries(void) {
    hop_xfer_rx_queue q;
    hop_xfer_rx_queue_init(&q, 2, 100);
    CHECK(hop_xfer_rx_queue_can_push(&q, 100));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 101));
    CHECK(hop_xfer_rx_queue_push(&q, 100));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 1));
    CHECK(!hop_xfer_rx_queue_push(&q, 1));
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    CHECK(!hop_xfer_rx_queue_dequeue(&q));
    CHECK(q.busy_slots == 1 && q.queued_slots == 0);
    CHECK(!hop_xfer_rx_queue_can_push(&q, 1));
    hop_xfer_rx_queue_complete(&q, 100);
    CHECK(hop_xfer_rx_queue_drained(&q));

    hop_xfer_rx_queue_init(&q, 0, 100);
    CHECK(!hop_xfer_rx_queue_can_push(&q, 0));
    CHECK(!hop_xfer_rx_queue_push(&q, 0));
}

static void test_reservation_lifecycle(void) {
    hop_xfer_rx_queue q;
    hop_xfer_rx_queue_init(&q, 3, 100);
    CHECK(hop_xfer_rx_queue_reserve(&q, 60));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 41));
    CHECK(hop_xfer_rx_queue_commit_reserved(&q));
    CHECK(q.reserved_slots == 0 && q.queued_slots == 1);
    CHECK(q.queued_bytes == 60);

    CHECK(hop_xfer_rx_queue_reserve(&q, 40));
    hop_xfer_rx_queue_rollback(&q, 40);
    CHECK(q.reserved_slots == 0 && q.queued_bytes == 60);
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    hop_xfer_rx_queue_complete(&q, 60);
    CHECK(hop_xfer_rx_queue_drained(&q));
    CHECK(q.queued_bytes == 0);
}

static void test_busy_and_queued_accounting(void) {
    hop_xfer_rx_queue q;
    hop_xfer_rx_queue_init(&q, 2, 100);
    CHECK(hop_xfer_rx_queue_push(&q, 60));
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    CHECK(hop_xfer_rx_queue_can_push(&q, 40));
    CHECK(hop_xfer_rx_queue_push(&q, 40));
    CHECK(!hop_xfer_rx_queue_can_push(&q, 1));
    CHECK(q.busy_slots == 1 && q.queued_slots == 1 && q.queued_bytes == 100);
    hop_xfer_rx_queue_complete(&q, 60);
    CHECK(q.busy_slots == 0 && q.queued_slots == 1 && q.queued_bytes == 40);
    CHECK(hop_xfer_rx_queue_dequeue(&q));
    hop_xfer_rx_queue_complete(&q, 40);
    CHECK(hop_xfer_rx_queue_drained(&q));
}

int main(void) {
    test_capacity_boundaries();
    test_reservation_lifecycle();
    test_busy_and_queued_accounting();
    if (failures) {
        fprintf(stderr, "RX queue tests failed: %d\n", failures);
        return 1;
    }
    puts("PASS: RX queue accounting boundaries and lifecycle");
    return 0;
}
