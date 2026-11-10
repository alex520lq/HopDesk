#include "xfer_ack_coordinator.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    hop_xfer_ack_coordinator *c = hop_xfer_ack_coordinator_create();
    assert(c != NULL);

    int status = -1, expired = 0;
    uint64_t rtt = 0;

    /* ACK may arrive before the sender installs the post-FILE_END deadline. */
    assert(hop_xfer_ack_coordinator_register(c, 7, 1));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 7, 1, 100));
    assert(hop_xfer_ack_coordinator_deliver(c, 7, 1, 0, 150));
    assert(hop_xfer_ack_coordinator_set_deadline(c, 7, 1, 200));
    assert(hop_xfer_ack_coordinator_wait_take(c, 7, 1, 0, &status, &rtt, &expired));
    assert(status == 0 && rtt == 50 && expired == 0);

    /* The receive timestamp, not the time the sender wakes, decides timeout. */
    assert(hop_xfer_ack_coordinator_register(c, 7, 2));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 7, 2, 300));
    assert(hop_xfer_ack_coordinator_set_deadline(c, 7, 2, 350));
    assert(hop_xfer_ack_coordinator_deliver(c, 7, 2, 0, 360));
    status = -1; expired = 0; rtt = 0;
    assert(hop_xfer_ack_coordinator_wait_take(c, 7, 2, 0, &status, &rtt, &expired));
    assert(status == 0 && rtt == 60 && expired == 1);

    /* Duplicate and stale ACKs cannot overwrite a reused (batch,id) slot. */
    assert(!hop_xfer_ack_coordinator_deliver(c, 7, 2, 1, 400));
    assert(hop_xfer_ack_coordinator_register(c, 8, 3));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 8, 3, 500));
    hop_xfer_ack_coordinator_unregister_batch(c, 8);
    assert(!hop_xfer_ack_coordinator_deliver(c, 8, 3, 0, 550));
    assert(hop_xfer_ack_coordinator_register(c, 8, 3));

    /* All slots are bounded and batch cleanup releases every reservation. */
    assert(hop_xfer_ack_coordinator_register(c, 8, 4));
    assert(hop_xfer_ack_coordinator_register(c, 8, 5));
    assert(hop_xfer_ack_coordinator_register(c, 8, 6));
    assert(!hop_xfer_ack_coordinator_register(c, 8, 7));
    hop_xfer_ack_coordinator_unregister_batch(c, 8);
    assert(hop_xfer_ack_coordinator_register(c, 9, 1));

    hop_xfer_ack_coordinator_destroy(c);
    puts("PASS: platform ACK coordinator lifecycle, early/late ACK, stale ACK and bounded slots");
    return 0;
}
