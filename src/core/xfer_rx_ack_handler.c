#include "hop.h"
#include "xfer_rx_ack_handler.h"
#include "xfer_status.h"

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
