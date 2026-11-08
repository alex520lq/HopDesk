#include "xfer_rx_file_state.h"
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

uint8_t hop_xfer_rx_file_state_ack(const hop_xfer_rx_file_state *state,
                                   uint8_t peer_status, int writer_error,
                                   uint32_t expected_file_crc) {
    if (!state || !state->id) return HOP_FILE_ACK_DISK;
    if (state->failure >= 0) return (uint8_t)state->failure;
    if (writer_error || peer_status != HOP_FILE_OK) return HOP_FILE_ACK_DISK;
    if (state->received != state->declared_size) return HOP_FILE_ACK_CRC;
    if (state->crc != expected_file_crc) return HOP_FILE_ACK_CRC;
    return HOP_FILE_ACK_OK;
}
