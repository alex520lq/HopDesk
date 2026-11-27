#include "../src/core/xfer_sender_engine.h"
#include "../src/core/xfer_status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ENTRY_COUNT 8
typedef struct { const char *path; uint64_t size; } fake_entry;
typedef struct {
    fake_entry entries[ENTRY_COUNT];
    int send_calls[ENTRY_COUNT];
    int ack_calls[ENTRY_COUNT];
    int events[32];
    int event_count;
    int send_dead_index;
    int send_dead_once;
    int ack_result[ENTRY_COUNT];
    int ack_first_result[ENTRY_COUNT];
    int cancel;
    int abort;
    int wait_ok;
    int unregister_calls;
    int reconnect_calls;
    int feedback_calls;
    int completed[ENTRY_COUNT];
    int failed[ENTRY_COUNT];
    unsigned waits[32];
    int wait_count;
    int logs;
} fake_sender;

static const void *entry_at(void *user, int index) {
    fake_sender *f = (fake_sender *)user;
    return index >= 0 && index < ENTRY_COUNT ? &f->entries[index] : NULL;
}
static const char *entry_path(void *user, const void *entry) {
    (void)user;
    return ((const fake_entry *)entry)->path;
}
static uint64_t entry_size(void *user, const void *entry) {
    (void)user;
    return ((const fake_entry *)entry)->size;
}
static int is_cancelled(void *user, uint32_t batch) {
    (void)batch;
    return ((fake_sender *)user)->cancel;
}
static int is_aborted(void *user, uint32_t batch) {
    (void)batch;
    return ((fake_sender *)user)->abort;
}
static hop_xfer_sender_send_result send_entry(void *user, uint32_t batch,
                                               uint32_t id, const void *entry,
                                               uint64_t *deadline) {
    fake_sender *f = (fake_sender *)user;
    (void)batch;
    int index = (int)id - 1;
    assert(entry == &f->entries[index]);
    ++f->send_calls[index];
    if (f->event_count < 32) f->events[f->event_count++] = (int)id;
    if (index == f->send_dead_index && f->send_dead_once) {
        f->send_dead_once = 0;
        return HOP_XFER_SENDER_SEND_DEAD;
    }
    *deadline = 1000000ull + id;
    return HOP_XFER_SENDER_SEND_PENDING;
}
static hop_xfer_sender_ack_result wait_ack(void *user, uint32_t batch,
                                            uint32_t id, uint64_t deadline,
                                            uint64_t *rtt) {
    fake_sender *f = (fake_sender *)user;
    (void)batch;
    assert(deadline == 1000000ull + id);
    ++f->ack_calls[id - 1];
    if (f->event_count < 32) f->events[f->event_count++] = -(int)id;
    if (rtt) *rtt = 250;
    if (f->ack_calls[id - 1] == 1 && f->ack_first_result[id - 1] >= 0)
        return (hop_xfer_sender_ack_result)f->ack_first_result[id - 1];
    return (hop_xfer_sender_ack_result)f->ack_result[id - 1];
}
static void unregister_batch(void *user, uint32_t batch) {
    (void)batch;
    ++((fake_sender *)user)->unregister_calls;
}
static void feedback(void *user, uint64_t rtt) {
    assert(rtt == 250);
    ++((fake_sender *)user)->feedback_calls;
}
static void status_phase(void *user, uint32_t batch, int index,
                         int phase, uint32_t attempts) {
    fake_sender *f = (fake_sender *)user;
    (void)batch;
    (void)attempts;
    if (phase == HOP_XFER_FILE_COMPLETED) ++f->completed[index];
    if (phase == HOP_XFER_FILE_FAILED) ++f->failed[index];
}
static unsigned retry_delay(void *user, unsigned attempt) {
    (void)user;
    return attempt * 10u;
}
static int wait_ms(void *user, unsigned duration) {
    fake_sender *f = (fake_sender *)user;
    if (f->wait_count < 32) f->waits[f->wait_count++] = duration;
    return f->wait_ok;
}
static void request_reconnect(void *user) {
    ++((fake_sender *)user)->reconnect_calls;
}
static void log_message(void *user, int level, const char *message) {
    (void)level;
    assert(message && message[0]);
    ++((fake_sender *)user)->logs;
}

static void setup(fake_sender *f) {
    memset(f, 0, sizeof(*f));
    f->send_dead_index = -1;
    f->wait_ok = 1;
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        static const char *paths[ENTRY_COUNT] = {
            "a.txt", "b.txt", "c.txt", "d.txt",
            "e.txt", "f.txt", "g.txt", "h.txt"
        };
        f->entries[i].path = paths[i];
        f->ack_result[i] = HOP_XFER_SENDER_ACK_OK;
        f->ack_first_result[i] = -1;
    }
}
static hop_xfer_sender_engine_ops make_ops(fake_sender *f) {
    hop_xfer_sender_engine_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.cancel_requested = is_cancelled;
    ops.peer_aborted = is_aborted;
    ops.entry_at = entry_at;
    ops.entry_path = entry_path;
    ops.send_entry = send_entry;
    ops.wait_ack = wait_ack;
    ops.unregister_batch = unregister_batch;
    ops.rate_feedback = feedback;
    ops.status_file_phase = status_phase;
    ops.retry_delay_ms = retry_delay;
    ops.wait_ms = wait_ms;
    ops.request_reconnect = request_reconnect;
    ops.log_message = log_message;
    return ops;
}

static void test_happy_path_bounded_window(void) {
    fake_sender f; setup(&f);
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 7, 6, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(next == 6 && ok == 6);
    for (int i = 0; i < 6; ++i) {
        assert(f.send_calls[i] == 1 && f.ack_calls[i] == 1);
        assert(f.completed[i] == 1 && f.failed[i] == 0);
    }
    assert(f.unregister_calls == 2);
}

static void test_badpath_is_acknowledged_but_not_successful(void) {
    fake_sender f; setup(&f);
    f.ack_result[1] = HOP_XFER_SENDER_ACK_BADPATH;
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 8, 3, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(next == 3 && ok == 2);
    assert(f.failed[1] == 1 && f.completed[1] == 0);
}

static void test_disconnect_drains_confirmed_prefix(void) {
    fake_sender f; setup(&f);
    f.send_dead_index = 1;
    f.send_dead_once = 1;
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 9, 3, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_NET_DOWN);
    assert(next == 1 && ok == 1);
    assert(f.send_calls[0] == 1 && f.send_calls[1] == 1 && f.send_calls[2] == 0);
    assert(hop_xfer_sender_run_files(&ops, 9, 3, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(next == 3 && ok == 3);
    assert(f.send_calls[0] == 1 && f.send_calls[1] == 2 && f.send_calls[2] == 1);
}

static void test_timeout_reconnects_without_checkpoint_advance(void) {
    fake_sender f; setup(&f);
    f.ack_result[0] = HOP_XFER_SENDER_ACK_TIMEOUT;
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 10, 2, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_NET_DOWN);
    assert(next == 0 && ok == 0 && f.reconnect_calls == 1);
}

static void test_ack_retry_and_cancel_abort(void) {
    fake_sender f; setup(&f);
    f.ack_first_result[0] = HOP_XFER_SENDER_ACK_OTHER_REMOTE;
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    int next = 0, ok = 0;
    /* The first remote failure is retried; the second attempt is ACK_OK. */
    assert(hop_xfer_sender_run_files(&ops, 11, 1, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(next == 1 && ok == 1);
    assert(f.send_calls[0] == 2 && f.ack_calls[0] == 2);
    assert(f.wait_count == 1 && f.waits[0] == 10);

    setup(&f); f.cancel = 1; ops = make_ops(&f); next = 0; ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 12, 1, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_CANCELLED);
    assert(next == 0 && f.send_calls[0] == 0);

    setup(&f); f.abort = 1; ops = make_ops(&f); next = 0; ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 13, 1, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_PEER_ABORT);
    assert(next == 0 && f.send_calls[0] == 0);
}

static void test_mixed_small_and_large_window_budget(void) {
    fake_sender f; setup(&f);
    f.entries[0].size = 1024ull;
    f.entries[1].size = 100ull * 1024ull * 1024ull;
    f.entries[2].size = 2048ull;
    f.entries[3].size = 2048ull;
    f.entries[4].size = 50ull * 1024ull * 1024ull;
    hop_xfer_sender_engine_ops ops = make_ops(&f);
    ops.entry_size = entry_size;
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_files(&ops, 14, 5, &next, &ok, 3) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(next == 5 && ok == 5);
    /* Window order: [1] -> ack1, [2] -> ack2, [3,4] -> ack3,ack4, [5] -> ack5. */
    const int expected[10] = {1, -1, 2, -2, 3, 4, -3, -4, 5, -5};
    assert(f.event_count == 10);
    for (int i = 0; i < 10; ++i) assert(f.events[i] == expected[i]);
}

int main(void) {
    test_happy_path_bounded_window();
    test_badpath_is_acknowledged_but_not_successful();
    test_disconnect_drains_confirmed_prefix();
    test_timeout_reconnects_without_checkpoint_advance();
    test_ack_retry_and_cancel_abort();
    test_mixed_small_and_large_window_budget();
    puts("PASS: sender engine window, checkpoint, retry, cancellation and reconnect policy");
    return 0;
}
