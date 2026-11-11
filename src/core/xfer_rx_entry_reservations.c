#include "xfer_rx_entry_reservations.h"
#include <string.h>

void hop_xfer_rx_entry_reservations_reset(hop_xfer_rx_entry_reservations *entries) {
    if (entries) memset(entries, 0, sizeof(*entries));
}

hop_xfer_rx_entry_reserve_result hop_xfer_rx_entry_reservations_reserve(
    hop_xfer_rx_entry_reservations *entries, uint32_t id, uint64_t size,
    hop_xfer_rx_entry_budget_reserve_fn reserve_budget, void *context) {
    if (!entries || !id || id > HOP_XFER_FILE_LIMIT)
        return HOP_RX_ENTRY_RESERVE_INVALID;
    uint32_t index = id - 1;
    if (entries->reserved[index])
        return entries->sizes[index] == size
            ? HOP_RX_ENTRY_RESERVE_ALREADY_HELD
            : HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT;
    if (!reserve_budget || !reserve_budget(context, size))
        return HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED;
    entries->sizes[index] = size;
    entries->reserved[index] = 1;
    return HOP_RX_ENTRY_RESERVE_NEW;
}
