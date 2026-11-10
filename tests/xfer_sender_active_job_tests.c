#include "../src/core/xfer_sender_active_job.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_begin_and_cancel_target(void) {
    hop_xfer_sender_active_job active;
    memset(&active, 0, sizeof(active));
    assert(!hop_xfer_sender_active_job_is_active(&active));
    assert(hop_xfer_sender_active_job_cancel_target(&active, 77) == 77);
    assert(hop_xfer_sender_active_job_begin(&active, 12, 9001));
    assert(hop_xfer_sender_active_job_is_active(&active));
    assert(active.batch == 12 && active.task_id == 9001);
    assert(hop_xfer_sender_active_job_cancel_target(&active, 77) == 12);
    assert(!active.abort_batch && !active.abort_reason);
}

static void test_abort_is_scoped_to_active_batch(void) {
    hop_xfer_sender_active_job active = {0};
    assert(hop_xfer_sender_active_job_begin(&active, 20, 2001));
    assert(!hop_xfer_sender_active_job_record_peer_abort(&active, 21, 1));
    assert(!active.abort_batch && !active.abort_reason);
    assert(hop_xfer_sender_active_job_record_peer_abort(&active, 20, 1));
    assert(active.abort_batch == 20 && active.abort_reason == 1);
}

static void test_cancel_take_clears_exact_active_job(void) {
    hop_xfer_sender_active_job active = {0};
    assert(hop_xfer_sender_active_job_begin(&active, 30, 3001));
    assert(hop_xfer_sender_active_job_record_peer_abort(&active, 30, 2));
    assert(!hop_xfer_sender_active_job_take_cancelled_and_clear(
        &active, 30, 3002, 2));
    assert(active.batch == 30 && active.task_id == 3001);
    assert(hop_xfer_sender_active_job_take_cancelled_and_clear(
        &active, 30, 3001, 2));
    assert(!hop_xfer_sender_active_job_is_active(&active));
    assert(active.abort_batch == 30 && active.abort_reason == 2);
}

static void test_non_cancel_abort_still_clears_active(void) {
    hop_xfer_sender_active_job active = {0};
    assert(hop_xfer_sender_active_job_begin(&active, 40, 4001));
    assert(hop_xfer_sender_active_job_record_peer_abort(&active, 40, 3));
    assert(!hop_xfer_sender_active_job_take_cancelled_and_clear(
        &active, 40, 4001, 2));
    assert(!hop_xfer_sender_active_job_is_active(&active));
}

static void test_clear_and_session_reset(void) {
    hop_xfer_sender_active_job active = {0};
    assert(hop_xfer_sender_active_job_begin(&active, 50, 5001));
    assert(hop_xfer_sender_active_job_record_peer_abort(&active, 50, 2));
    assert(hop_xfer_sender_active_job_clear_if_matches(&active, 50, 5001));
    assert(!hop_xfer_sender_active_job_clear_if_matches(&active, 50, 5001));
    assert(active.abort_batch == 50);
    assert(hop_xfer_sender_active_job_begin(&active, 51, 5002));
    assert(!active.abort_batch && !active.abort_reason);
    assert(hop_xfer_sender_active_job_record_peer_abort(&active, 51, 2));
    hop_xfer_sender_active_job_reset_peer_abort(&active);
    assert(hop_xfer_sender_active_job_is_active(&active));
    assert(!active.abort_batch && !active.abort_reason);
}

static void test_invalid_begin_is_rejected(void) {
    hop_xfer_sender_active_job active = {0};
    assert(!hop_xfer_sender_active_job_begin(&active, 0, 1));
    assert(!hop_xfer_sender_active_job_begin(&active, 1, 0));
    assert(!hop_xfer_sender_active_job_is_active(&active));
}

int main(void) {
    test_begin_and_cancel_target();
    test_abort_is_scoped_to_active_batch();
    test_cancel_take_clears_exact_active_job();
    test_non_cancel_abort_still_clears_active();
    test_clear_and_session_reset();
    test_invalid_begin_is_rejected();
    puts("PASS: Sender active-job and peer-ABORT arbitration");
    return 0;
}
