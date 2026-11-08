#include "xfer_rx_entry_reservations.h"
#include <assert.h>
#include <stdio.h>

typedef struct { uint64_t bytes; unsigned calls; int allow; } budget_probe;
static int reserve_budget(void *opaque, uint64_t bytes) {
    budget_probe *probe = (budget_probe *)opaque;
    probe->calls++;
    if (!probe->allow) return 0;
    probe->bytes += bytes;
    return 1;
}

int main(void) {
    hop_xfer_rx_entry_reservations entries;
    hop_xfer_rx_entry_reservations_reset(&entries);
    budget_probe probe = {0, 0, 1};

    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 100, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.calls == 1 && probe.bytes == 100);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 100, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_ALREADY_HELD);
    assert(probe.calls == 1 && probe.bytes == 100);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 101, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT);
    assert(probe.calls == 1 && probe.bytes == 100);

    probe.allow = 0;
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 2, 200, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED);
    assert(!entries.reserved[1] && entries.sizes[1] == 0);
    probe.allow = 1;
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 2, 200, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.calls == 3 && probe.bytes == 300);

    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 0, 1, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_INVALID);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, HOP_XFER_FILE_LIMIT + 1u, 1, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_INVALID);

    hop_xfer_rx_entry_reservations_reset(&entries);
    assert(!entries.reserved[0] && !entries.reserved[1]);
    assert(hop_xfer_rx_entry_reservations_reserve(
        &entries, 1, 50, reserve_budget, &probe) == HOP_RX_ENTRY_RESERVE_NEW);
    assert(probe.bytes == 350);
    puts("PASS: receiver entry reservation idempotency, conflict and budget rollback");
    return 0;
}
