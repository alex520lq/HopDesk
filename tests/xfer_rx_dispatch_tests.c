#include "hop.h"
#include "xfer_rx_dispatch.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_BEGIN) == HOP_RX_DISPATCH_FILE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_IMAGE_BEGIN) == HOP_RX_DISPATCH_IMAGE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_CHUNK) == HOP_RX_DISPATCH_FILE_CHUNK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_END) == HOP_RX_DISPATCH_FILE_END);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_ACK) == HOP_RX_DISPATCH_FILE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_ABORT) == HOP_RX_DISPATCH_ABORT);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE) == HOP_RX_DISPATCH_DONE);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE_ACK) == HOP_RX_DISPATCH_DONE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_HELLO) == HOP_RX_DISPATCH_UNKNOWN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_HELLO) == HOP_RX_DISPATCH_UNKNOWN);
    assert(hop_xfer_rx_dispatch_classify(255) == HOP_RX_DISPATCH_UNKNOWN);
    puts("PASS: receiver protocol dispatch classification");
    return 0;
}
