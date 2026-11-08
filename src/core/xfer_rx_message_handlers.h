#ifndef HOP_XFER_RX_MESSAGE_HANDLERS_H
#define HOP_XFER_RX_MESSAGE_HANDLERS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void hop_xfer_rx_handle_image_begin(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_file_chunk(void *context, const uint8_t *payload, uint32_t length);
#ifdef __cplusplus
}
#endif
#endif
