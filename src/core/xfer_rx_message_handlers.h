#ifndef HOP_XFER_RX_MESSAGE_HANDLERS_H
#define HOP_XFER_RX_MESSAGE_HANDLERS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int hop_xfer_rx_finish_batch(void *context, uint32_t batch, uint32_t ok, uint32_t total);
void hop_xfer_rx_handle_file_begin(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_file_end(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_image_begin(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_file_chunk(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_dispatch_file_ack_message(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_abort(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_handle_done(void *context, const uint8_t *payload, uint32_t length);
void hop_xfer_rx_dispatch_done_ack_message(void *context, const uint8_t *payload, uint32_t length);
#ifdef __cplusplus
}
#endif
#endif
