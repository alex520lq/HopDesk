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
