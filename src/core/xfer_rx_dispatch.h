#ifndef HOP_XFER_RX_DISPATCH_H
#define HOP_XFER_RX_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-independent receiver message classification and callback dispatch.
 * Handlers are platform adapters; Core owns routing only, never side effects. */
typedef enum {
    HOP_RX_DISPATCH_UNKNOWN = 0,
    HOP_RX_DISPATCH_FILE_BEGIN,
    HOP_RX_DISPATCH_IMAGE_BEGIN,
    HOP_RX_DISPATCH_FILE_CHUNK,
    HOP_RX_DISPATCH_FILE_END,
    HOP_RX_DISPATCH_FILE_ACK,
    HOP_RX_DISPATCH_ABORT,
    HOP_RX_DISPATCH_DONE,
    HOP_RX_DISPATCH_DONE_ACK
} hop_xfer_rx_dispatch_action;

typedef void (*hop_xfer_rx_dispatch_handler)(
    void *context, const uint8_t *payload, uint32_t payload_length);

typedef struct {
    hop_xfer_rx_dispatch_handler file_begin;
    hop_xfer_rx_dispatch_handler image_begin;
    hop_xfer_rx_dispatch_handler file_chunk;
    hop_xfer_rx_dispatch_handler file_end;
    hop_xfer_rx_dispatch_handler file_ack;
    hop_xfer_rx_dispatch_handler abort_message;
    hop_xfer_rx_dispatch_handler done;
    hop_xfer_rx_dispatch_handler done_ack;
} hop_xfer_rx_dispatch_handlers;

hop_xfer_rx_dispatch_action hop_xfer_rx_dispatch_classify(uint8_t message_type);

/* Returns 1 when a known message has a registered handler and was dispatched;
 * returns 0 for unknown messages or a missing handler. */
int hop_xfer_rx_dispatch_message(
    uint8_t message_type, const uint8_t *payload, uint32_t payload_length,
    const hop_xfer_rx_dispatch_handlers *handlers, void *context);

#ifdef __cplusplus
}
#endif
#endif
