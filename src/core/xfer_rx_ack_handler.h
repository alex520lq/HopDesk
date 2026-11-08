#ifndef HOP_XFER_RX_ACK_HANDLER_H
#define HOP_XFER_RX_ACK_HANDLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hop_xfer_rx_file_ack_callback)(
    void *context, uint32_t batch, uint32_t file_id, uint8_t status);

typedef void (*hop_xfer_rx_done_ack_callback)(
    void *context, uint32_t batch, uint32_t status);

/* Decode, validate and deliver sender-side ACK payloads. Platform code owns
 * synchronization and the actual ACK-window delivery callback. */
int hop_xfer_rx_handle_file_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_file_ack_callback callback, void *context);

int hop_xfer_rx_handle_done_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_done_ack_callback callback, void *context);

#ifdef __cplusplus
}
#endif
#endif
