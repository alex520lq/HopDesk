#include "../src/core/xfer_sender_retry.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_initial_and_repeated_retry(void) {
    hop_xfer_sender_retry_plan plan;
    assert(hop_xfer_sender_plan_retry(
        1000, 0, 0, 5, 30000, 250, &plan) ==
        HOP_XFER_SENDER_RETRY_SCHEDULED);
    assert(plan.attempt == 1);
    assert(plan.give_up_at_ms == 31000);
    assert(plan.retry_at_ms == 1250);

    assert(hop_xfer_sender_plan_retry(
        2000, plan.give_up_at_ms, plan.attempt, 5, 30000, 500, &plan) ==
        HOP_XFER_SENDER_RETRY_SCHEDULED);
    assert(plan.attempt == 2);
    assert(plan.give_up_at_ms == 31000);
    assert(plan.retry_at_ms == 2500);
}

static void test_attempt_and_deadline_exhaustion(void) {
    hop_xfer_sender_retry_plan plan;
    assert(hop_xfer_sender_plan_retry(
        1000, 0, 4, 5, 30000, 250, &plan) ==
        HOP_XFER_SENDER_RETRY_EXHAUSTED);
    assert(plan.attempt == 5 && plan.give_up_at_ms == 31000);
    assert(plan.retry_at_ms == 0);

    assert(hop_xfer_sender_plan_retry(
        31000, 31000, 2, 5, 30000, 250, &plan) ==
        HOP_XFER_SENDER_RETRY_EXHAUSTED);
    assert(plan.attempt == 3 && plan.give_up_at_ms == 31000);
    assert(plan.retry_at_ms == 0);
}

static void test_saturating_deadlines_and_invalid_input(void) {
    hop_xfer_sender_retry_plan plan;
    assert(hop_xfer_sender_plan_retry(
        UINT64_MAX - 5, 0, 0, 5, 10, 20, &plan) ==
        HOP_XFER_SENDER_RETRY_SCHEDULED);
    assert(plan.give_up_at_ms == UINT64_MAX);
    assert(plan.retry_at_ms == UINT64_MAX);

    assert(hop_xfer_sender_plan_retry(
        10, 20, UINT32_MAX, 5, 100, 1, &plan) ==
        HOP_XFER_SENDER_RETRY_EXHAUSTED);
    assert(plan.attempt == UINT32_MAX);
    assert(hop_xfer_sender_plan_retry(
        10, 20, 0, 0, 100, 1, &plan) ==
        HOP_XFER_SENDER_RETRY_INVALID);
    assert(hop_xfer_sender_plan_retry(
        10, 20, 0, 5, 100, 1, NULL) ==
        HOP_XFER_SENDER_RETRY_INVALID);
}

int main(void) {
    test_initial_and_repeated_retry();
    test_attempt_and_deadline_exhaustion();
    test_saturating_deadlines_and_invalid_input();
    puts("PASS: shared Sender retry budget, attempt limit and deadline policy");
    return 0;
}
