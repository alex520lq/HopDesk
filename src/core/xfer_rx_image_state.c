#include "xfer_rx_image_state.h"
#include <string.h>

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
