#include "xfer_rx_file_state.h"
#include "xfer_status.h"
#include <string.h>

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
