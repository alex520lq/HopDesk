#include "hop.h"
#include "xfer_rx_dispatch.h"
#include <assert.h>
#include <stdio.h>

static int calls;
static uint8_t last_byte;
static void capture(void *context, const uint8_t *payload, uint32_t length) {
    int *seen = (int *)context;
    assert(payload && length == 1);
    last_byte = payload[0];
    (*seen)++;
    calls++;
}

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

    int seen = 0;
    const uint8_t payload[] = { 0x5a };
    const hop_xfer_rx_dispatch_handlers handlers = { capture, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    assert(hop_xfer_rx_dispatch_message(HOP_MSG_FILE_BEGIN, payload, 1, &handlers, &seen));
    assert(seen == 1 && calls == 1 && last_byte == 0x5a);
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_IMAGE_BEGIN, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_XFER_HELLO, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(255, payload, 1, &handlers, &seen));
    assert(!hop_xfer_rx_dispatch_message(HOP_MSG_FILE_BEGIN, payload, 1, NULL, &seen));
    puts("PASS: receiver dispatch classification and injected handler routing");
    return 0;
}
