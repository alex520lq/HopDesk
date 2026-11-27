#include "xfer_rx_core.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_rx_configured_dir[HOP_RX_DIR_MAX];
static uint64_t s_rx_disconnect_budget_ms = HOP_XFER_RX_DISCONNECT_BUDGET_MS;

void hop_xfer_rx_configure(const char *rx_dir, uint64_t disconnect_budget_ms) {
    if (rx_dir) snprintf(s_rx_configured_dir, sizeof(s_rx_configured_dir), "%s", rx_dir);
    else s_rx_configured_dir[0] = '\0';
    if (disconnect_budget_ms >= 5000ull && disconnect_budget_ms <= 600000ull)
        s_rx_disconnect_budget_ms = disconnect_budget_ms;
}

const char *hop_xfer_rx_configured_dir(void) {
    return s_rx_configured_dir;
}

uint64_t hop_xfer_rx_disconnect_budget_ms(void) {
    return s_rx_disconnect_budget_ms;
}

/* ---- DONE Idempotency Receipt ---- */

void hop_xfer_rx_done_receipt_clear(hop_xfer_rx_done_receipt *receipt) {
    if (receipt) memset(receipt, 0, sizeof(*receipt));
}

const hop_xfer_rx_done_receipt *hop_xfer_rx_done_receipt_find(
    const hop_xfer_rx_done_receipt *receipt,
    const hop_session_id *session, uint32_t batch) {
    if (!receipt || !receipt->valid || !session || !batch ||
        receipt->done.batch != batch ||
        !hop_session_id_equal(&receipt->session, session))
        return NULL;
    return receipt;
}

int hop_xfer_rx_done_receipt_matches_payload(
    const hop_xfer_rx_done_receipt *receipt, const hop_xfer_done_msg *done) {
    return receipt && receipt->valid && done &&
           receipt->done.batch == done->batch &&
           receipt->done.ok == done->ok &&
           receipt->done.total == done->total &&
           receipt->done.bytes == done->bytes;
}

void hop_xfer_rx_done_receipt_record(
    hop_xfer_rx_done_receipt *receipt, const hop_session_id *session,
    const hop_xfer_done_msg *done, uint8_t status) {
    if (!receipt || !session || !done || !done->batch) return;
    receipt->valid = 1;
    receipt->session = *session;
    receipt->done = *done;
    receipt->status = status;
}

/* ---- Session & Batch Tombstone Lifecycle ---- */

void hop_xfer_rx_lifecycle_init(hop_xfer_rx_lifecycle *lifecycle) {
    if (lifecycle) memset(lifecycle, 0, sizeof(*lifecycle));
}

const hop_session_id *hop_xfer_rx_lifecycle_session(
    const hop_xfer_rx_lifecycle *lifecycle) {
    return lifecycle ? &lifecycle->session : NULL;
}

int hop_xfer_rx_lifecycle_bind_session(
    hop_xfer_rx_lifecycle *lifecycle, const hop_session_id *session) {
    if (!lifecycle || !session) return 0;
    if (hop_session_id_equal(&lifecycle->session, session)) return 0;
    lifecycle->session = *session;
    lifecycle->dead_batch = 0;
    lifecycle->dead_reason = 0;
    return 1;
}

void hop_xfer_rx_lifecycle_mark_dead(
    hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t reason) {
    if (!lifecycle || !batch) return;
    lifecycle->dead_batch = batch;
    lifecycle->dead_reason = reason;
}

int hop_xfer_rx_lifecycle_dead_reason(
    const hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t *reason) {
    if (!lifecycle || !batch || lifecycle->dead_batch != batch) return 0;
    if (reason) *reason = lifecycle->dead_reason;
    return 1;
}

/* ---- Batch Transition Rules ---- */

hop_xfer_rx_batch_begin_action hop_xfer_rx_batch_begin_decide(
    uint32_t batch, int committed, uint8_t dead_reason,
    int active, uint32_t active_batch, uint8_t *reason) {
    if (reason) *reason = 0;
    if (!batch) return HOP_RX_BATCH_BEGIN_INVALID;
    if (committed) return HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED;
    if (dead_reason) {
        if (reason) *reason = dead_reason;
        return HOP_RX_BATCH_BEGIN_REJECT_DEAD;
    }
    if (active && active_batch == batch) return HOP_RX_BATCH_BEGIN_RESUME;
    return HOP_RX_BATCH_BEGIN_START;
}

hop_xfer_rx_batch_abort_action hop_xfer_rx_batch_abort_decide(
    uint32_t batch, int active, uint32_t active_batch, int notify_peer) {
    if (!batch) return HOP_RX_BATCH_ABORT_IGNORE;
    if (hop_xfer_rx_batch_is_active(batch, active, active_batch))
        return HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE;
    return notify_peer ? HOP_RX_BATCH_ABORT_REPLY_ONLY : HOP_RX_BATCH_ABORT_IGNORE;
}

int hop_xfer_rx_batch_is_active(uint32_t batch, int active, uint32_t active_batch) {
    return batch != 0 && active && active_batch == batch;
}

hop_xfer_rx_batch_finish_action hop_xfer_rx_batch_finish_decide(
    uint32_t batch, int active, uint32_t active_batch,
    int completion_claimed, uint32_t ok, uint32_t total,
    int is_image, int image_valid) {
    if (!hop_xfer_rx_batch_is_active(batch, active, active_batch))
        return HOP_RX_BATCH_FINISH_STALE;
    if (!completion_claimed) return HOP_RX_BATCH_FINISH_CANCELLED;
    if (ok == total) {
        if (!is_image || image_valid) return HOP_RX_BATCH_FINISH_COMPLETE;
        return HOP_RX_BATCH_FINISH_IMAGE_FAILED;
    }
    return is_image ? HOP_RX_BATCH_FINISH_IMAGE_FAILED : HOP_RX_BATCH_FINISH_PARTIAL;
}

/* ---- Active Batch Identity ---- */

void hop_xfer_rx_batch_state_init(hop_xfer_rx_batch_state *state) {
    if (state) memset(state, 0, sizeof(*state));
}

int hop_xfer_rx_batch_state_begin(hop_xfer_rx_batch_state *state, uint32_t batch) {
    if (!state || !batch || state->active) return 0;
    state->generation++;
    if (!state->generation) state->generation++;
    state->batch = batch;
    state->active = 1;
    return 1;
}

int hop_xfer_rx_batch_state_end(hop_xfer_rx_batch_state *state, uint32_t batch) {
    if (!state || !batch || !state->active || state->batch != batch) return 0;
    state->active = 0;
    state->batch = 0;
    return 1;
}

void hop_xfer_rx_batch_state_reset(hop_xfer_rx_batch_state *state) {
    if (!state) return;
    state->active = 0;
    state->batch = 0;
}

int hop_xfer_rx_batch_state_matches(const hop_xfer_rx_batch_state *state, uint32_t batch) {
    return state && batch != 0 && state->active && state->batch == batch;
}

/* ---- Entry Budget Reservations ---- */

void hop_xfer_rx_entry_reservations_reset(hop_xfer_rx_entry_reservations *entries) {
    if (entries) memset(entries, 0, sizeof(*entries));
}

hop_xfer_rx_entry_reserve_result hop_xfer_rx_entry_reservations_reserve(
    hop_xfer_rx_entry_reservations *entries, uint32_t id, uint64_t size,
    hop_xfer_rx_entry_budget_reserve_fn reserve_budget, void *context) {
    if (!entries || !id || id > HOP_XFER_FILE_LIMIT)
        return HOP_RX_ENTRY_RESERVE_INVALID;
    uint32_t index = id - 1;
    if (entries->reserved[index])
        return entries->sizes[index] == size
            ? HOP_RX_ENTRY_RESERVE_ALREADY_HELD
            : HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT;
    if (!reserve_budget || !reserve_budget(context, size))
        return HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED;
    entries->sizes[index] = size;
    entries->reserved[index] = 1;
    return HOP_RX_ENTRY_RESERVE_NEW;
}

/* ---- Active File State ---- */

void hop_xfer_rx_file_state_reset(hop_xfer_rx_file_state *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->failure = -1;
}

void hop_xfer_rx_file_state_begin(hop_xfer_rx_file_state *state, uint32_t id) {
    if (!state) return;
    hop_xfer_rx_file_state_reset(state);
    state->id = id;
}

void hop_xfer_rx_file_state_set_kind(hop_xfer_rx_file_state *state, int is_image, int is_directory) {
    if (!state) return;
    state->is_image = is_image ? 1 : 0;
    state->is_directory = is_directory ? 1 : 0;
}

void hop_xfer_rx_file_state_set_size(hop_xfer_rx_file_state *state, uint64_t size) {
    if (state) state->declared_size = size;
}

int hop_xfer_rx_file_state_prepare_entry(hop_xfer_rx_file_state *state,
    uint32_t id, uint64_t size, int is_directory,
    const uint8_t *relative_path, uint32_t relative_path_length) {
    if (!state || !id || id > HOP_XFER_FILE_LIMIT) return 0;
    hop_xfer_rx_file_state_begin(state, id);
    hop_xfer_rx_file_state_set_kind(state, 0, is_directory);
    hop_xfer_rx_file_state_set_size(state, size);
    if (!hop_xfer_relpath_bytes_safe(relative_path, relative_path_length)) {
        hop_xfer_rx_file_state_fail(state, HOP_FILE_ACK_BADPATH);
        return 0;
    }
    return 1;
}

void hop_xfer_rx_file_state_fail(hop_xfer_rx_file_state *state, uint8_t ack_status) {
    if (state) state->failure = (int)ack_status;
}

int hop_xfer_rx_file_state_matches(const hop_xfer_rx_file_state *state, uint32_t id) {
    return state && id != 0 && state->id == id;
}

hop_xfer_rx_file_chunk_check hop_xfer_rx_file_state_check_chunk(
    const hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length) {
    if (!state || !state->id) return HOP_RX_FILE_CHUNK_INVALID;
    if (state->failure >= 0) return HOP_RX_FILE_CHUNK_ALREADY_FAILED;
    if (offset != state->received || offset > state->declared_size ||
        (uint64_t)length > state->declared_size - offset)
        return HOP_RX_FILE_CHUNK_INVALID;
    return HOP_RX_FILE_CHUNK_ACCEPT;
}

int hop_xfer_rx_file_state_record_chunk(
    hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length,
    uint32_t chunk_crc) {
    if (hop_xfer_rx_file_state_check_chunk(state, offset, length) != HOP_RX_FILE_CHUNK_ACCEPT)
        return 0;
    state->received += length;
    state->crc = hop_xfer_file_crc_step(state->crc, offset, chunk_crc);
    return 1;
}

hop_xfer_rx_file_end_result hop_xfer_rx_file_state_evaluate_end(
    const hop_xfer_rx_file_state *state, uint8_t peer_status,
    int writer_error, uint32_t expected_file_crc) {
    hop_xfer_rx_file_end_result result;
    result.ack_status = HOP_FILE_ACK_DISK;
    result.reason = HOP_RX_FILE_END_STORED_FAILURE;
    if (!state || !state->id) {
        result.reason = HOP_RX_FILE_END_STORED_FAILURE;
        return result;
    }
    if (state->failure >= 0) {
        result.ack_status = (uint8_t)state->failure;
        result.reason = HOP_RX_FILE_END_STORED_FAILURE;
    } else if (writer_error) {
        result.ack_status = HOP_FILE_ACK_DISK;
        result.reason = HOP_RX_FILE_END_WRITER_ERROR;
    } else if (peer_status != HOP_FILE_OK) {
        result.ack_status = HOP_FILE_ACK_DISK;
        result.reason = HOP_RX_FILE_END_PEER_REJECTED;
    } else if (state->received != state->declared_size) {
        result.ack_status = HOP_FILE_ACK_CRC;
        result.reason = HOP_RX_FILE_END_INCOMPLETE;
    } else if (state->crc != expected_file_crc) {
        result.ack_status = HOP_FILE_ACK_CRC;
        result.reason = HOP_RX_FILE_END_CRC_MISMATCH;
    } else {
        result.ack_status = HOP_FILE_ACK_OK;
        result.reason = HOP_RX_FILE_END_SUCCESS;
    }
    return result;
}

uint8_t hop_xfer_rx_file_state_ack(const hop_xfer_rx_file_state *state,
                                   uint8_t peer_status, int writer_error,
                                   uint32_t expected_file_crc) {
    return hop_xfer_rx_file_state_evaluate_end(
        state, peer_status, writer_error, expected_file_crc).ack_status;
}

/* ---- Active Image State & Buffer ---- */

void hop_xfer_rx_image_state_reset(hop_xfer_rx_image_state *state) {
    if (state) memset(state, 0, sizeof(*state));
}

int hop_xfer_rx_image_state_begin(hop_xfer_rx_image_state *state,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint64_t expected) {
    if (!state || !width || !height || !stride || !expected) return 0;
    hop_xfer_rx_image_state_reset(state);
    state->width = width;
    state->height = height;
    state->stride = stride;
    state->expected = expected;
    return 1;
}

void hop_xfer_rx_image_state_set_capacity(hop_xfer_rx_image_state *state,
                                          uint64_t capacity) {
    if (state) state->capacity = capacity;
}

hop_xfer_rx_image_chunk_check hop_xfer_rx_image_state_check_chunk(
    const hop_xfer_rx_image_state *state, uint64_t offset, uint32_t length) {
    if (!state || !state->expected || offset != state->received ||
        offset > state->expected || (uint64_t)length > state->expected - offset)
        return HOP_RX_IMAGE_CHUNK_INVALID;
    if (state->capacity && (offset > state->capacity ||
        (uint64_t)length > state->capacity - offset))
        return HOP_RX_IMAGE_CHUNK_INVALID;
    return HOP_RX_IMAGE_CHUNK_ACCEPT;
}

int hop_xfer_rx_image_state_record_chunk(hop_xfer_rx_image_state *state,
                                         uint64_t offset, uint32_t length) {
    if (hop_xfer_rx_image_state_check_chunk(state, offset, length) != HOP_RX_IMAGE_CHUNK_ACCEPT)
        return 0;
    state->received += length;
    return 1;
}

int hop_xfer_rx_image_buffer_ensure(hop_xfer_rx_image_buffer *buffer, uint64_t expected) {
    if (!buffer || !expected || expected > (uint64_t)SIZE_MAX) return 0;
    if (buffer->data) return buffer->capacity >= expected;
    uint8_t *data = (uint8_t *)malloc((size_t)expected);
    if (!data) return 0;
    buffer->data = data;
    buffer->capacity = expected;
    return 1;
}

void hop_xfer_rx_image_buffer_dispose(hop_xfer_rx_image_buffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

/* ---- Frame Gate ---- */

static hop_rx_frame_gate_result frame_gate_result(hop_rx_frame_action action) {
    hop_rx_frame_gate_result out;
    out.action = action;
    out.batch = 0;
    out.body = NULL;
    out.body_length = 0;
    return out;
}

hop_rx_frame_gate_result hop_xfer_rx_gate_frame(
    const hop_rx_frame_gate_input *input,
    const char *token,
    uint8_t type,
    const uint8_t *payload,
    uint32_t payload_length) {
    if (!input || !token) return frame_gate_result(HOP_RX_FRAME_REJECT_PEER);

    if (type == HOP_MSG_XFER_HELLO) {
        if (!hop_session_matches(&input->expected_session) ||
            !hop_xfer_hello_bound(payload, payload_length, token,
                                  &input->expected_session))
            return frame_gate_result(HOP_RX_FRAME_HELLO_REJECT);
        return frame_gate_result(HOP_RX_FRAME_HELLO_ACCEPT);
    }

    if (!hop_session_matches(&input->expected_session))
        return frame_gate_result(HOP_RX_FRAME_REJECT_PEER);

    uint32_t batch = 0, body_length = 0;
    const uint8_t *body = NULL;
    if (!hop_data_unwrap(type, payload, payload_length, &input->expected_session,
                         &batch, &body, &body_length))
        return frame_gate_result(HOP_RX_FRAME_REJECT_PEER);

    if ((type == HOP_MSG_FILE_CHUNK || type == HOP_MSG_FILE_END) &&
        (!input->active_batch || batch != input->active_batch))
        return frame_gate_result(HOP_RX_FRAME_DROP);

    hop_rx_frame_gate_result out = frame_gate_result(HOP_RX_FRAME_DISPATCH);
    out.batch = batch;
    out.body = body;
    out.body_length = body_length;
    return out;
}

/* ---- Message Dispatch ---- */

hop_xfer_rx_dispatch_action hop_xfer_rx_dispatch_classify(uint8_t message_type) {
    switch (message_type) {
        case HOP_MSG_FILE_BEGIN: return HOP_RX_DISPATCH_FILE_BEGIN;
        case HOP_MSG_IMAGE_BEGIN: return HOP_RX_DISPATCH_IMAGE_BEGIN;
        case HOP_MSG_FILE_CHUNK: return HOP_RX_DISPATCH_FILE_CHUNK;
        case HOP_MSG_FILE_END: return HOP_RX_DISPATCH_FILE_END;
        case HOP_MSG_FILE_ACK: return HOP_RX_DISPATCH_FILE_ACK;
        case HOP_MSG_XFER_ABORT: return HOP_RX_DISPATCH_ABORT;
        case HOP_MSG_XFER_DONE: return HOP_RX_DISPATCH_DONE;
        case HOP_MSG_XFER_DONE_ACK: return HOP_RX_DISPATCH_DONE_ACK;
        default: return HOP_RX_DISPATCH_UNKNOWN;
    }
}

int hop_xfer_rx_dispatch_message(
    uint8_t message_type, const uint8_t *payload, uint32_t payload_length,
    const hop_xfer_rx_dispatch_handlers *handlers, void *context) {
    if (!handlers) return 0;
    hop_xfer_rx_dispatch_handler handler = NULL;
    switch (hop_xfer_rx_dispatch_classify(message_type)) {
        case HOP_RX_DISPATCH_FILE_BEGIN: handler = handlers->file_begin; break;
        case HOP_RX_DISPATCH_IMAGE_BEGIN: handler = handlers->image_begin; break;
        case HOP_RX_DISPATCH_FILE_CHUNK: handler = handlers->file_chunk; break;
        case HOP_RX_DISPATCH_FILE_END: handler = handlers->file_end; break;
        case HOP_RX_DISPATCH_FILE_ACK: handler = handlers->file_ack; break;
        case HOP_RX_DISPATCH_ABORT: handler = handlers->abort_message; break;
        case HOP_RX_DISPATCH_DONE: handler = handlers->done; break;
        case HOP_RX_DISPATCH_DONE_ACK: handler = handlers->done_ack; break;
        case HOP_RX_DISPATCH_UNKNOWN:
        default: return 0;
    }
    if (!handler) return 0;
    handler(context, payload, payload_length);
    return 1;
}

/* ---- Inbound ACK Payload Validation ---- */

int hop_xfer_rx_handle_file_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_file_ack_callback callback, void *context) {
    hop_xfer_file_ack_msg ack;
    if (!callback || !hop_xfer_decode_file_ack(payload, payload_length, &ack) ||
        ack.id < 1u || ack.id > HOP_XFER_FILE_LIMIT) return 0;
    callback(context, ack.batch, ack.id, ack.status);
    return 1;
}

int hop_xfer_rx_handle_done_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_done_ack_callback callback, void *context) {
    if (!payload || payload_length != 5u || !callback) return 0;
    uint32_t batch = hop_get_be32(payload);
    uint8_t status = payload[4];
    if (!batch || status > HOP_DONE_ACK_FAILED) return 0;
    callback(context, batch, status);
    return 1;
}
