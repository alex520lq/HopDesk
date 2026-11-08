#ifndef HOP_XFER_RX_ENTRY_RESERVATIONS_H
#define HOP_XFER_RX_ENTRY_RESERVATIONS_H

#include "hop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t reserved[HOP_XFER_FILE_LIMIT];
    uint64_t sizes[HOP_XFER_FILE_LIMIT];
} hop_xfer_rx_entry_reservations;

typedef enum {
    HOP_RX_ENTRY_RESERVE_INVALID = 0,
    HOP_RX_ENTRY_RESERVE_NEW,
    HOP_RX_ENTRY_RESERVE_ALREADY_HELD,
    HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT,
    HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED
} hop_xfer_rx_entry_reserve_result;

typedef int (*hop_xfer_rx_entry_budget_reserve_fn)(void *context, uint64_t bytes);

void hop_xfer_rx_entry_reservations_reset(hop_xfer_rx_entry_reservations *entries);

/* IDs are one-based. A new slot is committed only after its Task budget
 * callback succeeds; same-ID/same-size retries are idempotent. */
hop_xfer_rx_entry_reserve_result hop_xfer_rx_entry_reservations_reserve(
    hop_xfer_rx_entry_reservations *entries, uint32_t id, uint64_t size,
    hop_xfer_rx_entry_budget_reserve_fn reserve_budget, void *context);

#ifdef __cplusplus
}
#endif
#endif
