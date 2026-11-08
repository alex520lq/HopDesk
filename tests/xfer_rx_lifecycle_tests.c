#include "xfer_rx_lifecycle.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    hop_xfer_rx_lifecycle lifecycle;
    hop_xfer_rx_lifecycle_init(&lifecycle);
    hop_session_id a = {{1}}, b = {{2}};
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &a));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 77, HOP_ABORT_CANCEL);
    uint8_t reason = 0;
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(reason == HOP_ABORT_CANCEL);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 78, NULL));

    assert(!hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &b));
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, NULL));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &b));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 88, HOP_ABORT_DISK);
    hop_xfer_rx_lifecycle_init(&lifecycle);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 88, NULL));
    puts("PASS: receiver session binding invalidates batch tombstones");
    return 0;
}
