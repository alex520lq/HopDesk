#include "../src/core/xfer_sender_ack_adapter.h"

#include <assert.h>
#include <stdio.h>

typedef struct { uint64_t now_ns; } fake_clock;
static uint64_t fake_now_ns(void *user) {
    return ((fake_clock *)user)->now_ns;
}

int main(void) {
    fake_clock clock = {1000};
    hop_xfer_sender_ack_adapter *adapter =
        hop_xfer_sender_ack_adapter_create(fake_now_ns, &clock);
    assert(adapter != NULL);

    int status = -1, expired = 0;
    uint64_t rtt_ns = 0;

    /* Normal delivery: RTT is measured from mark_sent to receiver delivery. */
    assert(hop_xfer_sender_ack_adapter_register(adapter, 10, 1));
    assert(hop_xfer_sender_ack_adapter_mark_sent(adapter, 10, 1));
    assert(hop_xfer_sender_ack_adapter_set_deadline(adapter, 10, 1, 1200));
    clock.now_ns = 1040;
    assert(hop_xfer_sender_ack_adapter_deliver(adapter, 10, 1, 0));
    assert(hop_xfer_sender_ack_adapter_wait_take(adapter, 10, 1, 0,
                                                  &status, &rtt_ns, &expired));
    assert(status == 0 && rtt_ns == 40 && !expired);

    /* Early ACK remains valid when the sender installs its deadline later. */
    clock.now_ns = 1100;
    assert(hop_xfer_sender_ack_adapter_register(adapter, 10, 2));
    assert(hop_xfer_sender_ack_adapter_mark_sent(adapter, 10, 2));
    clock.now_ns = 1110;
    assert(hop_xfer_sender_ack_adapter_deliver(adapter, 10, 2, 7));
    assert(hop_xfer_sender_ack_adapter_set_deadline(adapter, 10, 2, 1300));
    status = -1; expired = 0; rtt_ns = 0;
    assert(hop_xfer_sender_ack_adapter_wait_take(adapter, 10, 2, 0,
                                                  &status, &rtt_ns, &expired));
    assert(status == 7 && rtt_ns == 10 && !expired);

    /* A late ACK is classified by its receive timestamp. */
    clock.now_ns = 1200;
    assert(hop_xfer_sender_ack_adapter_register(adapter, 11, 1));
    assert(hop_xfer_sender_ack_adapter_mark_sent(adapter, 11, 1));
    assert(hop_xfer_sender_ack_adapter_set_deadline(adapter, 11, 1, 1250));
    clock.now_ns = 1260;
    assert(hop_xfer_sender_ack_adapter_deliver(adapter, 11, 1, 0));
    status = -1; expired = 0; rtt_ns = 0;
    assert(hop_xfer_sender_ack_adapter_wait_take(adapter, 11, 1, 0,
                                                  &status, &rtt_ns, &expired));
    assert(status == 0 && rtt_ns == 60 && expired);

    /* Wrong ids and unregistered batches cannot receive a stale ACK. */
    assert(hop_xfer_sender_ack_adapter_register(adapter, 12, 3));
    assert(!hop_xfer_sender_ack_adapter_deliver(adapter, 12, 4, 0));
    hop_xfer_sender_ack_adapter_unregister_batch(adapter, 12);
    assert(!hop_xfer_sender_ack_adapter_deliver(adapter, 12, 3, 0));

    hop_xfer_sender_ack_adapter_wake(adapter);
    hop_xfer_sender_ack_adapter_destroy(adapter);
    puts("PASS: sender ACK adapter owns coordinator and preserves ACK identity/timing");
    return 0;
}
