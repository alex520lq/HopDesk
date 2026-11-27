#include "xfer_rx_core.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int calls; uint32_t batch, id, status; } ack_capture;
static void capture_file_ack(void *context, uint32_t batch, uint32_t id, uint8_t status) {
    ack_capture *out = (ack_capture *)context;
    out->calls++; out->batch = batch; out->id = id; out->status = status;
}
static void capture_done_ack(void *context, uint32_t batch, uint8_t status) {
    ack_capture *out = (ack_capture *)context;
    out->calls++; out->batch = batch; out->id = HOP_DONE_ACK_SLOT; out->status = status;
}

static void test_ack_handler(void) {
    ack_capture out = {0};
    uint8_t file[9] = {0};
    hop_put_be32(file, 42); hop_put_be32(file + 4, 7); file[8] = 1;
    assert(hop_xfer_rx_handle_file_ack(file, sizeof(file), capture_file_ack, &out));
    assert(out.calls == 1 && out.batch == 42 && out.id == 7 && out.status == 1);
    assert(!hop_xfer_rx_handle_file_ack(file, 8, capture_file_ack, &out));
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), NULL, &out));
    hop_put_be32(file + 4, 0);
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), capture_file_ack, &out));
    hop_put_be32(file + 4, HOP_XFER_FILE_LIMIT + 1u);
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), capture_file_ack, &out));
    assert(out.calls == 1);

    uint8_t done[5] = {0};
    hop_put_be32(done, 99); done[4] = HOP_DONE_ACK_FAILED;
    assert(hop_xfer_rx_handle_done_ack(done, sizeof(done), capture_done_ack, &out));
    assert(out.calls == 2 && out.batch == 99 && out.id == HOP_DONE_ACK_SLOT &&
           out.status == HOP_DONE_ACK_FAILED);
    assert(!hop_xfer_rx_handle_done_ack(NULL, sizeof(done), capture_done_ack, &out));
    assert(!hop_xfer_rx_handle_done_ack(done, 4, capture_done_ack, &out));
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), NULL, &out));
    hop_put_be32(done, 0);
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), capture_done_ack, &out));
    hop_put_be32(done, 99); done[4] = HOP_DONE_ACK_FAILED + 1;
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), capture_done_ack, &out));
    assert(out.calls == 2);
}

static void test_batch_rules(void) {
    uint8_t reason = 99;
    assert(hop_xfer_rx_batch_begin_decide(0,0,0,0,0,&reason) == HOP_RX_BATCH_BEGIN_INVALID);
    assert(hop_xfer_rx_batch_begin_decide(7,1,0,1,7,&reason) == HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED);
    assert(hop_xfer_rx_batch_begin_decide(7,0,4,1,7,&reason) == HOP_RX_BATCH_BEGIN_REJECT_DEAD);
    assert(reason == 4);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,1,7,&reason) == HOP_RX_BATCH_BEGIN_RESUME);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,1,8,&reason) == HOP_RX_BATCH_BEGIN_START);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,0,0,&reason) == HOP_RX_BATCH_BEGIN_START);
    assert(reason == 0);

    assert(hop_xfer_rx_batch_abort_decide(0,1,7,1) == HOP_RX_BATCH_ABORT_IGNORE);
    assert(hop_xfer_rx_batch_abort_decide(7,1,7,1) == HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE);
    assert(hop_xfer_rx_batch_abort_decide(8,1,7,1) == HOP_RX_BATCH_ABORT_REPLY_ONLY);
    assert(hop_xfer_rx_batch_abort_decide(8,1,7,0) == HOP_RX_BATCH_ABORT_IGNORE);
    assert(hop_xfer_rx_batch_abort_decide(7,0,0,1) == HOP_RX_BATCH_ABORT_REPLY_ONLY);

    assert(!hop_xfer_rx_batch_is_active(0,1,7));
    assert(!hop_xfer_rx_batch_is_active(7,0,7));
    assert(!hop_xfer_rx_batch_is_active(7,1,8));
    assert(hop_xfer_rx_batch_is_active(7,1,7));

    assert(hop_xfer_rx_batch_finish_decide(7,0,7,1,3,3,0,1) == HOP_RX_BATCH_FINISH_STALE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,0,3,3,0,1) == HOP_RX_BATCH_FINISH_CANCELLED);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,0,1) == HOP_RX_BATCH_FINISH_COMPLETE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,1,1) == HOP_RX_BATCH_FINISH_COMPLETE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,1,0) == HOP_RX_BATCH_FINISH_IMAGE_FAILED);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,2,3,0,1) == HOP_RX_BATCH_FINISH_PARTIAL);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,2,3,1,1) == HOP_RX_BATCH_FINISH_IMAGE_FAILED);
}

static void test_batch_state(void) {
    hop_xfer_rx_batch_state state;
    hop_xfer_rx_batch_state_init(&state);
    assert(!state.active && !state.batch && !state.generation);
    assert(!hop_xfer_rx_batch_state_begin(&state, 0));
    assert(hop_xfer_rx_batch_state_begin(&state, 17));
    assert(state.active && state.batch == 17 && state.generation == 1);
    assert(hop_xfer_rx_batch_state_matches(&state, 17));
    assert(!hop_xfer_rx_batch_state_matches(&state, 18));
    assert(!hop_xfer_rx_batch_state_begin(&state, 18));
    assert(!hop_xfer_rx_batch_state_end(&state, 18));
    assert(hop_xfer_rx_batch_state_end(&state, 17));
    assert(!state.active && !state.batch && state.generation == 1);
    assert(!hop_xfer_rx_batch_state_matches(&state, 17));
    assert(hop_xfer_rx_batch_state_begin(&state, 17));
    assert(state.generation == 2);
    hop_xfer_rx_batch_state_reset(&state);
    assert(!state.active && !state.batch && state.generation == 2);
}

static int s_dispatch_calls;
static uint8_t s_dispatch_last_byte;
static void dispatch_capture(void *context, const uint8_t *payload, uint32_t length) {
    int *seen = (int *)context;
    assert(payload && length == 1);
    s_dispatch_last_byte = payload[0];
    (*seen)++;
    s_dispatch_calls++;
}

static void test_dispatch(void) {
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_BEGIN) == HOP_RX_DISPATCH_FILE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_IMAGE_BEGIN) == HOP_RX_DISPATCH_IMAGE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_CHUNK) == HOP_RX_DISPATCH_FILE_CHUNK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_END) == HOP_RX_DISPATCH_FILE_END);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_ACK) == HOP_RX_DISPATCH_FILE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_ABORT) == HOP_RX_DISPATCH_ABORT);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE) == HOP_RX_DISPATCH_DONE);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE_ACK) == HOP_RX_DISPATCH_DONE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_HELLO) == HOP_RX_DISPATCH_UNKNOWN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_HELLO) == HOP_RX_DISPATCH_UNKNOWN);
    assert(hop_xfer_rx_dispatch_classify(255) == HOP_RX_DISPATCH_UNKNOWN);

    int seen = 0;
    const uint8_t payload[] = { 0x5a };
    const hop_xfer_rx_dispatch_handlers handlers = { dispatch_capture, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    assert(hop_xfer_rx_dispatch_message(HOP_MSG_FILE_BEGIN, payload, 1, &handlers, &seen));
    assert(seen == 1 && s_dispatch_calls == 1 && s_dispatch_last_byte == 0x5a);
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_IMAGE_BEGIN, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_XFER_HELLO, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(255, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_FILE_BEGIN, payload, 1, NULL, &seen));
}

typedef struct { uint64_t bytes; unsigned calls; int allow; } budget_probe;
static int reserve_budget_cb(void *opaque, uint64_t bytes) {
    budget_probe *probe = (budget_probe *)opaque;
    probe->calls++;
    if (!probe->allow) return 0;
    probe->bytes += bytes;
    return 1;
}

static void test_entry_reservations(void) {
    hop_xfer_rx_entry_reservations entries;
    hop_xfer_rx_entry_reservations_reset(&entries);
    budget_probe probe = {0, 0, 1};

    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 100, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.calls == 1 && probe.bytes == 100);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 100, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_ALREADY_HELD);
    assert(probe.calls == 1 && probe.bytes == 100);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 101, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT);
    assert(probe.calls == 1 && probe.bytes == 100);

    probe.allow = 0;
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 2, 200, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED);
    assert(!entries.reserved[1] && entries.sizes[1] == 0);
    probe.allow = 1;
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 2, 200, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.calls == 3 && probe.bytes == 300);

    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 0, 1, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_INVALID);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, HOP_XFER_FILE_LIMIT + 1u, 1, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_INVALID);

    hop_xfer_rx_entry_reservations_reset(&entries);
    assert(!entries.reserved[0] && !entries.reserved[1]);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 50, reserve_budget_cb, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.bytes == 350);
}

static void test_file_state(void) {
    hop_xfer_rx_file_state state;
    hop_xfer_rx_file_state_reset(&state);
    assert(state.failure == -1 && state.id == 0);
    static const uint8_t valid_path[] = "folder/file.bin";
    assert(hop_xfer_rx_file_state_prepare_entry(&state, 7, 1234, 0,
        valid_path, (uint32_t)(sizeof(valid_path) - 1)));
    assert(state.id == 7 && state.declared_size == 1234);
    assert(!state.is_image && !state.is_directory && state.failure == -1);

    static const uint8_t directory_path[] = "folder/sub";
    assert(hop_xfer_rx_file_state_prepare_entry(&state, 8, 0, 1,
        directory_path, (uint32_t)(sizeof(directory_path) - 1)));
    assert(state.id == 8 && state.is_directory && state.failure == -1);

    static const uint8_t unsafe_path[] = "../escape";
    assert(!hop_xfer_rx_file_state_prepare_entry(&state, 9, 55, 0,
        unsafe_path, (uint32_t)(sizeof(unsafe_path) - 1)));
    assert(state.id == 9 && state.declared_size == 55);
    assert(state.failure == HOP_FILE_ACK_BADPATH);
    assert(!hop_xfer_rx_file_state_prepare_entry(&state, 0, 1, 0,
        valid_path, (uint32_t)(sizeof(valid_path) - 1)));
    assert(state.id == 9);

    hop_xfer_rx_file_state_begin(&state, 9);
    hop_xfer_rx_file_state_set_size(&state, 9);
    hop_xfer_rx_file_state_set_kind(&state, 1, 0);
    assert(state.is_image && !state.is_directory);
    assert(hop_xfer_rx_file_state_matches(&state, 9));
    assert(!hop_xfer_rx_file_state_matches(&state, 8));

    assert(hop_xfer_rx_file_state_check_chunk(&state, 0, 4) == HOP_RX_FILE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_file_state_record_chunk(&state, 0, 4, 0x12345678));
    uint32_t first_crc = hop_xfer_file_crc_step(0, 0, 0x12345678);
    assert(state.received == 4 && state.crc == first_crc);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 4, 5) == HOP_RX_FILE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 5, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 3, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_check_chunk(&state, UINT64_MAX, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_record_chunk(&state, 4, 5, 0xabcdef01));
    assert(state.received == 9);
    assert(state.crc == hop_xfer_file_crc_step(first_crc, 4, 0xabcdef01));
    assert(hop_xfer_rx_file_state_check_chunk(&state, 9, 1) == HOP_RX_FILE_CHUNK_INVALID);

    hop_xfer_rx_file_end_result end_result =
        hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_OK, 0, state.crc);
    assert(end_result.ack_status == HOP_FILE_ACK_OK);
    assert(end_result.reason == HOP_RX_FILE_END_SUCCESS);
    end_result = hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_IO_ERROR, 0, state.crc);
    assert(end_result.ack_status == HOP_FILE_ACK_DISK);
    assert(end_result.reason == HOP_RX_FILE_END_PEER_REJECTED);
    end_result = hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_OK, 1, state.crc);
    assert(end_result.reason == HOP_RX_FILE_END_WRITER_ERROR);
    end_result = hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_OK, 0, state.crc + 1);
    assert(end_result.reason == HOP_RX_FILE_END_CRC_MISMATCH);
    hop_xfer_rx_file_state_set_size(&state, 10);
    end_result = hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_OK, 0, state.crc);
    assert(end_result.reason == HOP_RX_FILE_END_INCOMPLETE);
    hop_xfer_rx_file_state_set_size(&state, 9);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc) == HOP_FILE_ACK_OK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_IO_ERROR, 0, state.crc) == HOP_FILE_ACK_DISK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 1, state.crc) == HOP_FILE_ACK_DISK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc + 1) == HOP_FILE_ACK_CRC);
    hop_xfer_rx_file_state_set_size(&state, 10);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc) == HOP_FILE_ACK_CRC);
    hop_xfer_rx_file_state_fail(&state, HOP_FILE_ACK_BADPATH);
    end_result = hop_xfer_rx_file_state_evaluate_end(&state, HOP_FILE_IO_ERROR, 1, 0);
    assert(end_result.ack_status == HOP_FILE_ACK_BADPATH);
    assert(end_result.reason == HOP_RX_FILE_END_STORED_FAILURE);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_IO_ERROR, 1, 0) == HOP_FILE_ACK_BADPATH);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 9, 0) == HOP_RX_FILE_CHUNK_ALREADY_FAILED);
    assert(!hop_xfer_rx_file_state_record_chunk(&state, 9, 0, 0));

    hop_xfer_rx_file_state_begin(&state, 10);
    assert(!state.is_image && !state.is_directory);
    assert(state.id == 10 && state.failure == -1 && state.received == 0 &&
           state.declared_size == 0 && state.crc == 0);
}

static void put_file_begin(uint8_t *frame, const hop_session_id *session,
                           uint32_t batch, const uint8_t *body,
                           uint32_t body_length) {
    hop_data_prefix(frame, session, batch);
    hop_put_be32(frame + HOP_DATA_PREFIX, batch);
    memcpy(frame + HOP_DATA_PREFIX + 4, body, body_length);
}

static void test_frame_gate(void) {
    hop_session_id session = {{1, 2, 3}};
    hop_session_id other = {{9, 8, 7}};
    char token[] = "shared-secret";
    hop_session_publish(&session);

    hop_rx_frame_gate_input input;
    input.expected_session = session;
    input.active_batch = 42;

    uint8_t hello[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    uint32_t hello_length = hop_xfer_hello_encode(hello, token, &session);
    assert(hello_length > 0);

    hop_rx_frame_gate_result result =
        hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO, hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_ACCEPT);
    assert(!result.body && !result.body_length);

    result = hop_xfer_rx_gate_frame(&input, "wrong-token", HOP_MSG_XFER_HELLO, hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO, hello, hello_length - 1);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO, NULL, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);

    input.expected_session = other;
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO, hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    input.expected_session = session;

    uint8_t chunk[HOP_DATA_PREFIX + 8];
    hop_data_prefix(chunk, &session, 42);
    memset(chunk + HOP_DATA_PREFIX, 0x5a, 8);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DISPATCH && result.batch == 42 &&
           result.body == chunk + HOP_DATA_PREFIX && result.body_length == 8);

    hop_data_prefix(chunk, &session, 41);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DROP && !result.body && !result.body_length);

    input.active_batch = 0;
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_END, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DROP);

    input.active_batch = 42;
    hop_data_prefix(chunk, &other, 42);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    hop_data_prefix(chunk, &session, 42);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK, chunk, HOP_DATA_PREFIX - 1);
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    uint8_t begin[HOP_DATA_PREFIX + 4 + 4];
    uint8_t body[4] = {1, 2, 3, 4};
    put_file_begin(begin, &session, 43, body, sizeof(body));
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_BEGIN, begin, sizeof(begin));
    assert(result.action == HOP_RX_FRAME_DISPATCH && result.batch == 43 &&
           result.body == begin + HOP_DATA_PREFIX && result.body_length == 4 + sizeof(body));

    hop_data_prefix(begin, &session, 43);
    hop_put_be32(begin + HOP_DATA_PREFIX, 42);
    memcpy(begin + HOP_DATA_PREFIX + 4, body, sizeof(body));
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_BEGIN, begin, sizeof(begin));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    result = hop_xfer_rx_gate_frame(&input, token, 0xff, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(NULL, token, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(&input, NULL, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    hop_session_clear(&session);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO, hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
}

static void test_image_state_and_buffer(void) {
    hop_xfer_rx_image_buffer buffer = {0};
    assert(!hop_xfer_rx_image_buffer_ensure(&buffer, 0));
    assert(hop_xfer_rx_image_buffer_ensure(&buffer, 16));
    assert(buffer.data && buffer.capacity == 16);
    uint8_t *original = buffer.data;
    assert(hop_xfer_rx_image_buffer_ensure(&buffer, 8));
    assert(buffer.data == original && buffer.capacity == 16);
    assert(!hop_xfer_rx_image_buffer_ensure(&buffer, 32));
    assert(buffer.data == original && buffer.capacity == 16);
    hop_xfer_rx_image_buffer_dispose(&buffer);
    assert(!buffer.data && !buffer.capacity);
    hop_xfer_rx_image_buffer_dispose(&buffer);
    hop_xfer_rx_image_buffer_dispose(NULL);

    hop_xfer_rx_image_state s = {0};
    assert(!hop_xfer_rx_image_state_begin(&s,0,2,8,16));
    assert(hop_xfer_rx_image_state_begin(&s,2,2,8,16));
    assert(s.width==2 && s.height==2 && s.stride==8 && s.expected==16 && s.received==0);
    assert(hop_xfer_rx_image_state_check_chunk(&s,0,8)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,0,8));
    assert(s.received==8);
    assert(hop_xfer_rx_image_state_check_chunk(&s,0,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,9,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,9)==HOP_RX_IMAGE_CHUNK_INVALID);
    hop_xfer_rx_image_state_set_capacity(&s,12);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,5)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,4)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,8,4));
    assert(s.received==12);
    assert(hop_xfer_rx_image_state_check_chunk(&s,12,4)==HOP_RX_IMAGE_CHUNK_INVALID);
    hop_xfer_rx_image_state_set_capacity(&s,16);
    assert(hop_xfer_rx_image_state_check_chunk(&s,12,4)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,12,4));
    assert(s.received==16);
    assert(hop_xfer_rx_image_state_check_chunk(&s,16,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    hop_xfer_rx_image_state_reset(&s);
    assert(!s.expected && !s.received && !s.capacity && !s.width);
}

static void test_lifecycle_and_receipt(void) {
    hop_xfer_rx_lifecycle lifecycle;
    hop_xfer_rx_lifecycle_init(&lifecycle);
    hop_session_id a = {{1}}, b = {{2}};
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &a));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 77, HOP_ABORT_CANCEL);
    uint8_t reason = 0;
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(reason == HOP_ABORT_CANCEL);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 78, NULL));

    assert(!hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &b));
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, NULL));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &b));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 88, HOP_ABORT_DISK);
    hop_xfer_rx_lifecycle_init(&lifecycle);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 88, NULL));

    hop_xfer_rx_done_receipt receipt;
    hop_xfer_rx_done_receipt_clear(&receipt);
    hop_xfer_done_msg done = {42, 3, 4, 987654};
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &a, 42));

    hop_xfer_rx_done_receipt_record(&receipt, &a, &done, HOP_DONE_ACK_COMPLETE);
    const hop_xfer_rx_done_receipt *found = hop_xfer_rx_done_receipt_find(&receipt, &a, 42);
    assert(found && found->status == HOP_DONE_ACK_COMPLETE);
    assert(hop_xfer_rx_done_receipt_matches_payload(found, &done));

    hop_xfer_done_msg changed = done;
    changed.bytes++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));
    changed = done; changed.ok++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));
    changed = done; changed.total++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));

    assert(!hop_xfer_rx_done_receipt_find(&receipt, &b, 42));
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &a, 43));
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &a, 0));

    hop_xfer_rx_done_receipt_clear(&receipt);
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &a, 42));
}

int main(void) {
    test_ack_handler();
    test_batch_rules();
    test_batch_state();
    test_dispatch();
    test_entry_reservations();
    test_file_state();
    test_frame_gate();
    test_image_state_and_buffer();
    test_lifecycle_and_receipt();
    puts("PASS: consolidated Core RX state, rules, gate, dispatch, ACK, image and lifecycle tests");
    return 0;
}
