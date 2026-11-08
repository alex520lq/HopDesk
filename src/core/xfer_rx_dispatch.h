#ifndef HOP_XFER_RX_DISPATCH_H
#define HOP_XFER_RX_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-independent classification for receiver data-channel messages.
 * This module deliberately contains no socket, task, filesystem or clipboard
 * effects; adapters own those side effects after classification. */
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

hop_xfer_rx_dispatch_action hop_xfer_rx_dispatch_classify(uint8_t message_type);

#ifdef __cplusplus
}
#endif
#endif
