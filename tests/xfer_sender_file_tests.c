#include "../src/core/xfer_sender_engine.h"
#include "../src/core/hop.h"
#include "../src/core/xfer_status.h"
#include "../src/core/xfer_tracking.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[32];
    uint32_t actual_size;
    uint64_t position;
    uint64_t fail_read_at;
    int fail_open;
    int fail_read;
    int fail_begin;
    int fail_chunk;
    int fail_end;
    int fail_ack_register;
    int fail_ack_mark;
    int fail_ack_deadline;
    int cancel_after_chunk;
    int abort_after_chunk;
    int cancelled;
    int aborted;
    int opens, closes, reads, chunks, messages;
    int ack_registers, ack_marks, ack_deadlines, ack_unregisters;
    int yields, status_reads, logs;
    uint8_t begin[256];
    uint32_t begin_length;
    uint8_t end[9];
    uint8_t last_message_type;
    uint64_t offsets[8];
    uint32_t lengths[8];
    uint32_t crcs[8];
    uint64_t read_times[8];
    uint64_t crc_times[8];
    uint64_t deadline;
    uint64_t now;
} fake_file;

static int is_cancelled(void *user, uint32_t batch) {
    (void)batch; return ((fake_file *)user)->cancelled;
}
static int is_aborted(void *user, uint32_t batch) {
    (void)batch; return ((fake_file *)user)->aborted;
}
static int open_file(void *user, const void *native, intptr_t *handle,
                     uint32_t *error) {
    fake_file *f = (fake_file *)user;
    assert(native == f);
    ++f->opens;
    if (error) *error = f->fail_open ? 5u : 0u;
    if (f->fail_open) return 0;
    f->position = 0;
    *handle = 1;
    return 1;
}
static hop_xfer_sender_read_result read_file(void *user, intptr_t handle,
                                              uint8_t *buffer, uint32_t request,
                                              uint32_t *got, uint32_t *error) {
    fake_file *f = (fake_file *)user;
    assert(handle == 1);
    ++f->reads;
    if (error) *error = 0;
    if (f->fail_read && f->position >= f->fail_read_at) {
        if (error) *error = 123u;
        if (got) *got = 0;
        return HOP_XFER_SENDER_READ_ERROR;
    }
    if (f->position >= f->actual_size) {
        if (got) *got = 0;
        return HOP_XFER_SENDER_READ_EOF;
    }
    uint64_t available = (uint64_t)f->actual_size - f->position;
    uint32_t n = (uint32_t)(available < request ? available : request);
    memcpy(buffer, f->data + f->position, n);
    f->position += n;
    if (got) *got = n;
    return HOP_XFER_SENDER_READ_OK;
}
static void close_file(void *user, intptr_t handle) {
    assert(handle == 1); ++((fake_file *)user)->closes;
}
static int send_message(void *user, uint8_t type, const uint8_t *payload,
                        uint32_t length, uint32_t batch) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77);
    ++f->messages;
    f->last_message_type = type;
    if (type == HOP_MSG_FILE_BEGIN) {
        assert(length <= sizeof(f->begin));
        memcpy(f->begin, payload, length);
        f->begin_length = length;
        return !f->fail_begin;
    }
    assert(type == HOP_MSG_FILE_END && length == sizeof(f->end));
    memcpy(f->end, payload, length);
    return !f->fail_end;
}
static int send_chunk(void *user, uint32_t batch, uint32_t id,
                      uint64_t offset, const uint8_t *data, uint32_t length,
                      uint32_t crc, uint64_t read_ns, uint64_t crc_ns) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && id == 3 && f->chunks < 8);
    assert(crc == hop_crc32(0, data, length));
    f->offsets[f->chunks] = offset;
    f->lengths[f->chunks] = length;
    f->crcs[f->chunks] = crc;
    f->read_times[f->chunks] = read_ns;
    f->crc_times[f->chunks] = crc_ns;
    ++f->chunks;
    if (f->cancel_after_chunk) f->cancelled = 1;
    if (f->abort_after_chunk) f->aborted = 1;
    return !f->fail_chunk;
}
static int ack_register(void *user, uint32_t batch, uint32_t id) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && id == 3);
    ++f->ack_registers; return !f->fail_ack_register;
}
static int ack_mark(void *user, uint32_t batch, uint32_t id) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && id == 3);
    ++f->ack_marks; return !f->fail_ack_mark;
}
static int ack_deadline(void *user, uint32_t batch, uint32_t id,
                        uint64_t deadline) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && id == 3);
    ++f->ack_deadlines; f->deadline = deadline;
    return !f->fail_ack_deadline;
}
static void ack_unregister(void *user, uint32_t batch, uint32_t id) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && id == 3);
    ++f->ack_unregisters;
}
static uint64_t now_ns(void *user) {
    fake_file *f = (fake_file *)user;
    f->now += 100;
    return f->now;
}
static void status_stage_time(void *user, uint32_t batch, int stage,
                              uint64_t elapsed) {
    fake_file *f = (fake_file *)user;
    assert(batch == 77 && stage == HOP_XFER_TIME_READ && elapsed == 100);
    ++f->status_reads;
}
static void yield_cpu(void *user) { ++((fake_file *)user)->yields; }
static void log_message(void *user, int level, const char *message) {
    (void)level; assert(message && message[0]); ++((fake_file *)user)->logs;
}
static hop_xfer_sender_file_ops make_ops(fake_file *f) {
    hop_xfer_sender_file_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.cancel_requested = is_cancelled;
    ops.peer_aborted = is_aborted;
    ops.open_file = open_file;
    ops.read_file = read_file;
    ops.close_file = close_file;
    ops.send_message = send_message;
    ops.send_chunk = send_chunk;
    ops.ack_register = ack_register;
    ops.ack_mark_sent = ack_mark;
    ops.ack_set_deadline = ack_deadline;
    ops.ack_unregister = ack_unregister;
    ops.now_ns = now_ns;
    ops.status_stage_time = status_stage_time;
    ops.yield_cpu = yield_cpu;
    ops.log_message = log_message;
    return ops;
}
static void setup(fake_file *f) {
    memset(f, 0, sizeof(*f));
    f->actual_size = 10;
    for (uint32_t i = 0; i < sizeof(f->data); ++i) f->data[i] = (uint8_t)i;
}
static hop_xfer_sender_send_result run(fake_file *f, uint32_t flags,
                                       uint64_t size, uint8_t *begin,
                                       uint8_t *chunk, uint64_t *deadline) {
    hop_xfer_sender_file_ops ops = make_ops(f);
    hop_xfer_sender_file_entry entry = {
        f, "a.txt", "dir/a.txt", size, 1234, flags
    };
    return hop_xfer_sender_send_file_entry(
        &ops, 77, 3, &entry, begin, (uint32_t)sizeof(begin),
        chunk, 8, 5000000ull,
        HOP_FILE_FLAG_DIR, HOP_FILE_OK, HOP_FILE_IO_ERROR,
        HOP_MSG_FILE_BEGIN, HOP_MSG_FILE_END, deadline);
}

static void test_begin_buffer_capacity_is_independent_of_chunk_size(void) {
    fake_file f; setup(&f);
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    hop_xfer_sender_file_ops ops = make_ops(&f);
    hop_xfer_sender_file_entry entry = {
        &f, "a.txt", "dir/a.txt", 10, 1234, 0
    };
    assert(hop_xfer_sender_send_file_entry(
        &ops, 77, 3, &entry, begin, 8, chunk, 8, 5000000ull,
        HOP_FILE_FLAG_DIR, HOP_FILE_OK, HOP_FILE_IO_ERROR,
        HOP_MSG_FILE_BEGIN, HOP_MSG_FILE_END, &deadline) ==
        HOP_XFER_SENDER_SEND_RETRYABLE);
    assert(f.ack_registers == 0 && f.messages == 0);
}

static void test_success_and_crc(void) {
    fake_file f; setup(&f);
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    assert(run(&f, 0, 10, begin, chunk, &deadline) == HOP_XFER_SENDER_SEND_PENDING);
    assert(f.opens == 1 && f.closes == 1 && f.chunks == 2);
    assert(f.offsets[0] == 0 && f.lengths[0] == 8);
    assert(f.offsets[1] == 8 && f.lengths[1] == 2);
    assert(f.begin_length == 4+4+8+8+1+2+5+2+9);
    assert(hop_get_be32(f.begin) == 77 && hop_get_be32(f.begin+4) == 3);
    assert(hop_get_be64(f.begin+8) == 10 && hop_get_be64(f.begin+16) == 1234);
    assert(f.begin[24] == 0 && hop_get_be16(f.begin+25) == 5);
    assert(memcmp(f.begin+27,"a.txt",5) == 0);
    assert(hop_get_be16(f.begin+32) == 9);
    assert(memcmp(f.begin+34,"dir/a.txt",9) == 0);
    uint32_t expected = hop_xfer_file_crc_step(0, 0, f.crcs[0]);
    expected = hop_xfer_file_crc_step(expected, 8, f.crcs[1]);
    assert(f.end[4] == HOP_FILE_OK && hop_get_be32(f.end+5) == expected);
    assert(f.ack_registers == 1 && f.ack_marks == 1 && f.ack_deadlines == 1);
    assert(f.ack_unregisters == 0 && deadline == f.deadline);
    assert(f.yields == 2);
}
static void test_directory_and_open_failure(void) {
    fake_file f; setup(&f);
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    assert(run(&f, HOP_FILE_FLAG_DIR, 0, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PENDING);
    assert(f.opens == 0 && f.chunks == 0 && f.end[4] == HOP_FILE_OK);

    setup(&f); f.fail_open = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PENDING);
    assert(f.opens == 1 && f.closes == 0 && f.chunks == 0);
    assert(f.end[4] == HOP_FILE_IO_ERROR && f.logs > 0);
}
static void test_read_failures_and_source_change(void) {
    fake_file f; setup(&f); f.fail_read = 1; f.fail_read_at = 8;
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PENDING);
    assert(f.chunks == 1 && f.status_reads == 1 && f.end[4] == HOP_FILE_IO_ERROR);

    setup(&f); f.actual_size = 5;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PENDING);
    assert(f.chunks == 1 && f.end[4] == HOP_FILE_IO_ERROR);

    setup(&f); f.actual_size = 12;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PENDING);
    assert(f.chunks == 2 && f.end[4] == HOP_FILE_IO_ERROR);
}
static void test_cancel_abort_and_transport_failures(void) {
    fake_file f; setup(&f); f.cancel_after_chunk = 1;
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_CANCELLED);
    assert(f.closes == 1 && f.ack_unregisters == 1 && f.messages == 1);

    setup(&f); f.abort_after_chunk = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_PEER_ABORT);
    assert(f.closes == 1 && f.ack_unregisters == 1 && f.messages == 1);

    setup(&f); f.fail_chunk = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_DEAD);
    assert(f.closes == 1 && f.ack_unregisters == 1);

    setup(&f); f.fail_begin = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_DEAD);
    assert(f.ack_unregisters == 1 && f.opens == 0);
}
static void test_ack_reservation_failures(void) {
    fake_file f; setup(&f);
    uint8_t begin[256], chunk[8]; uint64_t deadline = 0;
    f.fail_ack_register = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_RETRYABLE);
    assert(f.messages == 0 && f.ack_unregisters == 0);

    setup(&f); f.fail_ack_mark = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_RETRYABLE);
    assert(f.ack_unregisters == 1 && f.ack_marks == 1);

    setup(&f); f.fail_ack_deadline = 1;
    assert(run(&f, 0, 10, begin, chunk, &deadline) ==
           HOP_XFER_SENDER_SEND_RETRYABLE);
    assert(f.ack_unregisters == 1 && f.ack_deadlines == 1);
}
int main(void) {
    test_begin_buffer_capacity_is_independent_of_chunk_size();
    test_success_and_crc();
    test_directory_and_open_failure();
    test_read_failures_and_source_change();
    test_cancel_abort_and_transport_failures();
    test_ack_reservation_failures();
    puts("PASS: shared sender file IO, CRC, ACK reservation and cancellation");
    return 0;
}
