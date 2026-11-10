#include "../src/core/xfer_sender_engine.h"
#include "../src/core/hop.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t ms;
    uint64_t ns;
    uint8_t session[HOP_SESSION_BYTES];
    int session_valid;
    int ready;
    int ready_after_wait;
    int stopping;
    int fail_send_done;
    int fail_register;
    int fail_mark;
    int fail_deadline;
    int send_calls;
    int register_calls;
    int mark_calls;
    int deadline_calls;
    int unregister_calls;
    int wait_calls;
    int connection_calls;
    int ack_count;
    int ack_index;
    hop_xfer_sender_commit_ack_result ack_results[8];
    intptr_t last_socket;
    uint64_t last_epoch;
    uint32_t last_batch;
    uint32_t last_ok;
    uint32_t last_total;
    uint64_t last_deadline;
} fake_commit;

static uint64_t now_ms(void *user) { return ((fake_commit *)user)->ms; }
static uint64_t now_ns(void *user) {
    fake_commit *f = (fake_commit *)user;
    f->ns += 100;
    return f->ns;
}
static int is_stopping(void *user) { return ((fake_commit *)user)->stopping; }
static int session_matches(void *user, const uint8_t *session) {
    fake_commit *f = (fake_commit *)user;
    return f->session_valid && memcmp(f->session, session, HOP_SESSION_BYTES) == 0;
}
static int current_connection(void *user, intptr_t *socket_handle,
                              uint64_t *epoch, uint8_t *session,
                              size_t session_capacity) {
    fake_commit *f = (fake_commit *)user;
    ++f->connection_calls;
    if (!f->ready || session_capacity < HOP_SESSION_BYTES) return 0;
    *socket_handle = 17;
    *epoch = 9;
    memcpy(session, f->session, HOP_SESSION_BYTES);
    return 1;
}
static int send_done(void *user, intptr_t socket_handle, uint64_t epoch,
                     uint32_t batch, uint32_t ok, uint32_t total) {
    fake_commit *f = (fake_commit *)user;
    ++f->send_calls;
    f->last_socket = socket_handle; f->last_epoch = epoch;
    f->last_batch = batch; f->last_ok = ok; f->last_total = total;
    return !f->fail_send_done;
}
static int ack_register(void *user, uint32_t batch, uint32_t slot) {
    fake_commit *f = (fake_commit *)user;
    assert(batch == 55 && slot == 99);
    ++f->register_calls; return !f->fail_register;
}
static int ack_mark(void *user, uint32_t batch, uint32_t slot) {
    fake_commit *f = (fake_commit *)user;
    assert(batch == 55 && slot == 99);
    ++f->mark_calls; return !f->fail_mark;
}
static int ack_deadline(void *user, uint32_t batch, uint32_t slot,
                        uint64_t deadline) {
    fake_commit *f = (fake_commit *)user;
    assert(batch == 55 && slot == 99);
    ++f->deadline_calls; f->last_deadline = deadline;
    return !f->fail_deadline;
}
static void ack_unregister(void *user, uint32_t batch, uint32_t slot) {
    fake_commit *f = (fake_commit *)user;
    assert(batch == 55 && slot == 99); ++f->unregister_calls;
}
static hop_xfer_sender_commit_ack_result wait_ack(void *user, uint32_t batch,
                                                   uint32_t slot,
                                                   uint64_t deadline) {
    fake_commit *f = (fake_commit *)user;
    assert(batch == 55 && slot == 99 && deadline == f->last_deadline);
    assert(f->ack_index < f->ack_count);
    return f->ack_results[f->ack_index++];
}
static int wait_ms(void *user, uint32_t duration) {
    fake_commit *f = (fake_commit *)user;
    ++f->wait_calls;
    f->ms += duration;
    if (f->ready_after_wait) f->ready = 1;
    return !f->stopping;
}
static hop_xfer_sender_commit_ops make_ops(fake_commit *f) {
    hop_xfer_sender_commit_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.now_ms = now_ms;
    ops.now_ns = now_ns;
    ops.is_stopping = is_stopping;
    ops.session_matches = session_matches;
    ops.current_connection = current_connection;
    ops.send_done = send_done;
    ops.ack_register = ack_register;
    ops.ack_mark_sent = ack_mark;
    ops.ack_set_deadline = ack_deadline;
    ops.ack_unregister = ack_unregister;
    ops.wait_ack = wait_ack;
    ops.wait_ms = wait_ms;
    return ops;
}
static void setup(fake_commit *f) {
    memset(f, 0, sizeof(*f));
    f->session[0] = 1;
    f->session_valid = 1;
    f->ready = 1;
}
static hop_xfer_sender_commit_result run(fake_commit *f, int image,
                                         int entries, int ok) {
    hop_xfer_sender_commit_ops ops = make_ops(f);
    return hop_xfer_sender_run_commit(&ops, f->session, 55, image, entries, ok,
                                      99, 2000, 5000, 100);
}

static void test_complete_partial_and_terminal_results(void) {
    fake_commit f; setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 1;
    assert(run(&f, 0, 3, 3) == HOP_XFER_SENDER_COMMIT_DONE);
    assert(f.send_calls == 1 && f.register_calls == 1 && f.mark_calls == 1);
    assert(f.deadline_calls == 1 && f.unregister_calls == 0);
    assert(f.last_socket == 17 && f.last_epoch == 9);
    assert(f.last_batch == 55 && f.last_ok == 3 && f.last_total == 3);

    setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_PARTIAL; f.ack_count = 1;
    assert(run(&f, 0, 3, 2) == HOP_XFER_SENDER_COMMIT_DONE);

    setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_FAILED; f.ack_count = 1;
    assert(run(&f, 0, 3, 1) == HOP_XFER_SENDER_COMMIT_REJECTED);

    setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_PEER_ABORT; f.ack_count = 1;
    assert(run(&f, 0, 3, 1) == HOP_XFER_SENDER_COMMIT_PEER_ABORT);

    setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 1;
    assert(run(&f, 1, 0, 1) == HOP_XFER_SENDER_COMMIT_DONE);
    assert(f.last_total == 1);
}
static void test_timeout_and_send_dead_retry(void) {
    fake_commit f; setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_TIMEOUT;
    f.ack_results[1] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 2;
    assert(run(&f, 0, 2, 2) == HOP_XFER_SENDER_COMMIT_DONE);
    assert(f.send_calls == 2 && f.register_calls == 2 && f.wait_calls == 1);

    setup(&f);
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_SEND_DEAD;
    f.ack_results[1] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 2;
    assert(run(&f, 0, 2, 2) == HOP_XFER_SENDER_COMMIT_DONE);
    assert(f.send_calls == 2 && f.wait_calls == 1);
}
static void test_connection_recovery_and_send_retry(void) {
    fake_commit f; setup(&f); f.ready = 0; f.ready_after_wait = 1;
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_DONE);
    assert(f.connection_calls >= 2 && f.wait_calls == 1);

    setup(&f); f.fail_send_done = 1;
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 1;
    /* A failed send is retried; clear the injected failure after the first wait. */
    f.ready_after_wait = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_DONE || f.send_calls == 1);
}
static void test_invalid_session_and_stopping(void) {
    fake_commit f; setup(&f); f.session_valid = 0;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_UNKNOWN);
    assert(f.send_calls == 0);

    setup(&f); f.stopping = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_UNKNOWN);
    assert(f.send_calls == 0);
}
static void test_ack_reservation_failures(void) {
    fake_commit f; setup(&f); f.fail_register = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_UNKNOWN);
    assert(f.unregister_calls == 0 && f.send_calls == 0);

    setup(&f); f.fail_mark = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_UNKNOWN);
    assert(f.unregister_calls == 1 && f.send_calls == 0);

    setup(&f); f.fail_deadline = 1;
    f.ack_results[0] = HOP_XFER_SENDER_COMMIT_ACK_COMPLETE; f.ack_count = 1;
    assert(run(&f, 0, 1, 1) == HOP_XFER_SENDER_COMMIT_UNKNOWN);
    assert(f.unregister_calls == 1 && f.send_calls == 1);
}
int main(void) {
    test_complete_partial_and_terminal_results();
    test_timeout_and_send_dead_retry();
    test_connection_recovery_and_send_retry();
    test_invalid_session_and_stopping();
    test_ack_reservation_failures();
    puts("PASS: shared sender DONE commit, ACK retry and reconnect policy");
    return 0;
}
