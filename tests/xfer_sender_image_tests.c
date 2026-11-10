#include "../src/core/xfer_sender_engine.h"
#include "../src/core/hop.h"
#include "../src/core/xfer_status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int cancelled;
    int aborted;
    int cancel_after_chunk;
    int abort_after_chunk;
    int fail_begin;
    int fail_chunk;
    int fail_end;
    int begin_calls;
    int chunk_calls;
    int end_calls;
    int yield_calls;
    int feedback_calls;
    int status_calls;
    uint32_t width, height, stride;
    uint64_t offsets[8];
    uint32_t lengths[8];
    uint32_t crcs[8];
    uint64_t crc_times[8];
    uint64_t now;
} fake_image;

static int image_cancelled(void *user, uint32_t batch) {
    (void)batch;
    return ((fake_image *)user)->cancelled;
}
static int image_aborted(void *user, uint32_t batch) {
    (void)batch;
    return ((fake_image *)user)->aborted;
}
static uint64_t image_now(void *user) {
    fake_image *f = (fake_image *)user;
    f->now += 100;
    return f->now;
}
static int image_begin(void *user, uint32_t batch, uint32_t width,
                       uint32_t height, uint32_t stride) {
    fake_image *f = (fake_image *)user;
    assert(batch == 42);
    ++f->begin_calls;
    f->width = width; f->height = height; f->stride = stride;
    return !f->fail_begin;
}
static int image_chunk(void *user, uint32_t batch, uint32_t id,
                       uint64_t offset, const uint8_t *data,
                       uint32_t length, uint32_t crc, uint64_t crc_ns) {
    fake_image *f = (fake_image *)user;
    assert(batch == 42 && id == 1);
    assert(f->chunk_calls < 8);
    assert(crc == hop_crc32(0, data, length));
    f->offsets[f->chunk_calls] = offset;
    f->lengths[f->chunk_calls] = length;
    f->crcs[f->chunk_calls] = crc;
    f->crc_times[f->chunk_calls] = crc_ns;
    ++f->chunk_calls;
    if (f->cancel_after_chunk) f->cancelled = 1;
    if (f->abort_after_chunk) f->aborted = 1;
    return !f->fail_chunk;
}
static int image_end(void *user, uint32_t batch, uint32_t id,
                     uint8_t status, uint32_t file_crc) {
    fake_image *f = (fake_image *)user;
    assert(batch == 42 && id == 1);
    assert(status == HOP_FILE_OK && file_crc == 0);
    ++f->end_calls;
    return !f->fail_end;
}
static void image_yield(void *user) { ++((fake_image *)user)->yield_calls; }
static void image_feedback(void *user, uint64_t rtt) {
    assert(rtt == 0);
    ++((fake_image *)user)->feedback_calls;
}
static void image_status(void *user, uint32_t batch, int index,
                         int phase, uint32_t attempts) {
    fake_image *f = (fake_image *)user;
    assert(batch == 42 && index == 0);
    assert(phase == HOP_XFER_FILE_COMPLETED && attempts == 1);
    ++f->status_calls;
}
static hop_xfer_sender_image_ops make_ops(fake_image *f) {
    hop_xfer_sender_image_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.cancel_requested = image_cancelled;
    ops.peer_aborted = image_aborted;
    ops.now_ns = image_now;
    ops.send_image_begin = image_begin;
    ops.send_image_chunk = image_chunk;
    ops.send_image_end = image_end;
    ops.yield_cpu = image_yield;
    ops.rate_feedback = image_feedback;
    ops.status_file_phase = image_status;
    return ops;
}
static void setup(fake_image *f) { memset(f, 0, sizeof(*f)); }

static void test_chunk_order_crc_and_completion(void) {
    fake_image f; setup(&f);
    hop_xfer_sender_image_ops ops = make_ops(&f);
    const uint8_t pixels[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 8, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_DONE);
    assert(f.begin_calls == 1 && f.chunk_calls == 3 && f.end_calls == 1);
    assert(f.offsets[0] == 0 && f.lengths[0] == 6);
    assert(f.offsets[1] == 6 && f.lengths[1] == 6);
    assert(f.offsets[2] == 12 && f.lengths[2] == 4);
    assert(f.crc_times[0] == 100 && f.crc_times[1] == 100 && f.crc_times[2] == 100);
    assert(f.yield_calls == 3 && f.feedback_calls == 1 && f.status_calls == 1);
    assert(next == 1 && ok == 1);
}
static void test_cancel_and_abort_stop_before_end(void) {
    fake_image f; setup(&f); f.cancel_after_chunk = 1;
    hop_xfer_sender_image_ops ops = make_ops(&f);
    const uint8_t pixels[16] = {0};
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 8, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_CANCELLED);
    assert(f.chunk_calls == 1 && f.end_calls == 0 && next == 0 && ok == 0);

    setup(&f); f.abort_after_chunk = 1; ops = make_ops(&f); next = 0; ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 8, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_PEER_ABORT);
    assert(f.chunk_calls == 1 && f.end_calls == 0 && next == 0 && ok == 0);
}
static void test_transport_and_input_failures(void) {
    fake_image f; setup(&f); f.fail_begin = 1;
    hop_xfer_sender_image_ops ops = make_ops(&f);
    const uint8_t pixels[16] = {0};
    int next = 0, ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 8, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_NET_DOWN);
    assert(f.chunk_calls == 0 && f.end_calls == 0);

    setup(&f); f.fail_chunk = 1; ops = make_ops(&f); next = 0; ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 8, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_NET_DOWN);
    assert(f.chunk_calls == 1 && f.end_calls == 0);

    setup(&f); ops = make_ops(&f); next = 0; ok = 0;
    assert(hop_xfer_sender_run_image(&ops, 42, pixels, 2, 2, 7, 6,
                                     HOP_FILE_OK, 1, &next, &ok) ==
           HOP_XFER_SENDER_JOB_NET_DOWN);
    assert(f.begin_calls == 0);
}
int main(void) {
    test_chunk_order_crc_and_completion();
    test_cancel_and_abort_stop_before_end();
    test_transport_and_input_failures();
    puts("PASS: sender image sequencing, CRC, cancellation and failure handling");
    return 0;
}
