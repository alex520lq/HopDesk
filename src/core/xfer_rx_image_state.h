#ifndef HOP_XFER_RX_IMAGE_STATE_H
#define HOP_XFER_RX_IMAGE_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t expected;
    uint64_t received;
    uint64_t capacity;
} hop_xfer_rx_image_state;

typedef enum {
    HOP_RX_IMAGE_CHUNK_INVALID = 0,
    HOP_RX_IMAGE_CHUNK_ACCEPT
} hop_xfer_rx_image_chunk_check;

void hop_xfer_rx_image_state_reset(hop_xfer_rx_image_state *state);
int hop_xfer_rx_image_state_begin(hop_xfer_rx_image_state *state,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint64_t expected);
void hop_xfer_rx_image_state_set_capacity(hop_xfer_rx_image_state *state,
                                          uint64_t capacity);
hop_xfer_rx_image_chunk_check hop_xfer_rx_image_state_check_chunk(
    const hop_xfer_rx_image_state *state, uint64_t offset, uint32_t length);
int hop_xfer_rx_image_state_record_chunk(hop_xfer_rx_image_state *state,
                                         uint64_t offset, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
