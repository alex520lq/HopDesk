#include "xfer_rx_receipt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    hop_xfer_rx_done_receipt receipt;
    hop_xfer_rx_done_receipt_clear(&receipt);
    hop_session_id session_a = {{1}}, session_b = {{2}};
    hop_xfer_done_msg done = {42, 3, 4, 987654};
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &session_a, 42));

    hop_xfer_rx_done_receipt_record(&receipt, &session_a, &done, HOP_DONE_ACK_COMPLETE);
    const hop_xfer_rx_done_receipt *found =
        hop_xfer_rx_done_receipt_find(&receipt, &session_a, 42);
    assert(found && found->status == HOP_DONE_ACK_COMPLETE);
    assert(hop_xfer_rx_done_receipt_matches_payload(found, &done));

    hop_xfer_done_msg changed = done;
    changed.bytes++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));
    changed = done; changed.ok++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));
    changed = done; changed.total++;
    assert(!hop_xfer_rx_done_receipt_matches_payload(found, &changed));

    assert(!hop_xfer_rx_done_receipt_find(&receipt, &session_b, 42));
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &session_a, 43));
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &session_a, 0));

    hop_xfer_rx_done_receipt_clear(&receipt);
    assert(!hop_xfer_rx_done_receipt_find(&receipt, &session_a, 42));
    puts("PASS: receiver DONE receipt session binding, exact replay and invalidation");
    return 0;
}
