#include "xfer_rx_batch_rules.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t reason = 99;
    assert(hop_xfer_rx_batch_begin_decide(0,0,0,0,0,&reason) == HOP_RX_BATCH_BEGIN_INVALID);
    assert(hop_xfer_rx_batch_begin_decide(7,1,0,1,7,&reason) == HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED);
    assert(hop_xfer_rx_batch_begin_decide(7,0,4,1,7,&reason) == HOP_RX_BATCH_BEGIN_REJECT_DEAD);
    assert(reason == 4);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,1,7,&reason) == HOP_RX_BATCH_BEGIN_RESUME);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,1,8,&reason) == HOP_RX_BATCH_BEGIN_START);
    assert(hop_xfer_rx_batch_begin_decide(7,0,0,0,0,&reason) == HOP_RX_BATCH_BEGIN_START);
    assert(reason == 0);

    assert(hop_xfer_rx_batch_abort_decide(0,1,7,1) == HOP_RX_BATCH_ABORT_IGNORE);
    assert(hop_xfer_rx_batch_abort_decide(7,1,7,1) == HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE);
    assert(hop_xfer_rx_batch_abort_decide(8,1,7,1) == HOP_RX_BATCH_ABORT_REPLY_ONLY);
    assert(hop_xfer_rx_batch_abort_decide(8,1,7,0) == HOP_RX_BATCH_ABORT_IGNORE);
    assert(hop_xfer_rx_batch_abort_decide(7,0,0,1) == HOP_RX_BATCH_ABORT_REPLY_ONLY);

    assert(!hop_xfer_rx_batch_is_active(0,1,7));
    assert(!hop_xfer_rx_batch_is_active(7,0,7));
    assert(!hop_xfer_rx_batch_is_active(7,1,8));
    assert(hop_xfer_rx_batch_is_active(7,1,7));

    assert(hop_xfer_rx_batch_finish_decide(7,0,7,1,3,3,0,1) == HOP_RX_BATCH_FINISH_STALE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,0,3,3,0,1) == HOP_RX_BATCH_FINISH_CANCELLED);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,0,1) == HOP_RX_BATCH_FINISH_COMPLETE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,1,1) == HOP_RX_BATCH_FINISH_COMPLETE);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,3,3,1,0) == HOP_RX_BATCH_FINISH_IMAGE_FAILED);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,2,3,0,1) == HOP_RX_BATCH_FINISH_PARTIAL);
    assert(hop_xfer_rx_batch_finish_decide(7,1,7,1,2,3,1,1) == HOP_RX_BATCH_FINISH_IMAGE_FAILED);
    puts("PASS: receiver batch transition policy precedence and outcomes");
    return 0;
}
