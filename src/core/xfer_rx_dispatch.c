#include "hop.h"
#include "xfer_rx_dispatch.h"

hop_xfer_rx_dispatch_action hop_xfer_rx_dispatch_classify(uint8_t message_type) {
    switch (message_type) {
        case HOP_MSG_FILE_BEGIN: return HOP_RX_DISPATCH_FILE_BEGIN;
        case HOP_MSG_IMAGE_BEGIN: return HOP_RX_DISPATCH_IMAGE_BEGIN;
        case HOP_MSG_FILE_CHUNK: return HOP_RX_DISPATCH_FILE_CHUNK;
        case HOP_MSG_FILE_END: return HOP_RX_DISPATCH_FILE_END;
        case HOP_MSG_FILE_ACK: return HOP_RX_DISPATCH_FILE_ACK;
        case HOP_MSG_XFER_ABORT: return HOP_RX_DISPATCH_ABORT;
        case HOP_MSG_XFER_DONE: return HOP_RX_DISPATCH_DONE;
        case HOP_MSG_XFER_DONE_ACK: return HOP_RX_DISPATCH_DONE_ACK;
        default: return HOP_RX_DISPATCH_UNKNOWN;
    }
}

int hop_xfer_rx_dispatch_message(
    uint8_t message_type, const uint8_t *payload, uint32_t payload_length,
    const hop_xfer_rx_dispatch_handlers *handlers, void *context) {
    if (!handlers) return 0;
    hop_xfer_rx_dispatch_handler handler = NULL;
    switch (hop_xfer_rx_dispatch_classify(message_type)) {
        case HOP_RX_DISPATCH_FILE_BEGIN: handler = handlers->file_begin; break;
        case HOP_RX_DISPATCH_IMAGE_BEGIN: handler = handlers->image_begin; break;
        case HOP_RX_DISPATCH_FILE_CHUNK: handler = handlers->file_chunk; break;
        case HOP_RX_DISPATCH_FILE_END: handler = handlers->file_end; break;
        case HOP_RX_DISPATCH_FILE_ACK: handler = handlers->file_ack; break;
        case HOP_RX_DISPATCH_ABORT: handler = handlers->abort_message; break;
        case HOP_RX_DISPATCH_DONE: handler = handlers->done; break;
        case HOP_RX_DISPATCH_DONE_ACK: handler = handlers->done_ack; break;
        case HOP_RX_DISPATCH_UNKNOWN:
        default: return 0;
    }
    if (!handler) return 0;
    handler(context, payload, payload_length);
    return 1;
}
