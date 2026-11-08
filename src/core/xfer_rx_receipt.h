#ifndef HOP_XFER_RX_RECEIPT_H
#define HOP_XFER_RX_RECEIPT_H

#include "hop.h"

/* Receiver-thread-owned idempotency receipt for one completed batch.
 * A receipt is valid only for the control session that committed it. */
typedef struct {
    int valid;
    hop_session_id session;
    hop_xfer_done_msg done;
    uint8_t status;
} hop_xfer_rx_done_receipt;

void hop_xfer_rx_done_receipt_clear(hop_xfer_rx_done_receipt *receipt);
const hop_xfer_rx_done_receipt *hop_xfer_rx_done_receipt_find(
    const hop_xfer_rx_done_receipt *receipt,
    const hop_session_id *session, uint32_t batch);
int hop_xfer_rx_done_receipt_matches_payload(
    const hop_xfer_rx_done_receipt *receipt, const hop_xfer_done_msg *done);
void hop_xfer_rx_done_receipt_record(
    hop_xfer_rx_done_receipt *receipt, const hop_session_id *session,
    const hop_xfer_done_msg *done, uint8_t status);

#endif
