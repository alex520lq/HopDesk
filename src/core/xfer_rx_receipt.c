#include "xfer_rx_receipt.h"
#include <string.h>

void hop_xfer_rx_done_receipt_clear(hop_xfer_rx_done_receipt *receipt) {
    if (receipt) memset(receipt, 0, sizeof(*receipt));
}

const hop_xfer_rx_done_receipt *hop_xfer_rx_done_receipt_find(
    const hop_xfer_rx_done_receipt *receipt,
    const hop_session_id *session, uint32_t batch) {
    if (!receipt || !receipt->valid || !session || !batch ||
        receipt->done.batch != batch ||
        !hop_session_id_equal(&receipt->session, session))
        return NULL;
    return receipt;
}

int hop_xfer_rx_done_receipt_matches_payload(
    const hop_xfer_rx_done_receipt *receipt, const hop_xfer_done_msg *done) {
    return receipt && receipt->valid && done &&
           receipt->done.batch == done->batch &&
           receipt->done.ok == done->ok &&
           receipt->done.total == done->total &&
           receipt->done.bytes == done->bytes;
}

void hop_xfer_rx_done_receipt_record(
    hop_xfer_rx_done_receipt *receipt, const hop_session_id *session,
    const hop_xfer_done_msg *done, uint8_t status) {
    if (!receipt || !session || !done || !done->batch) return;
    receipt->valid = 1;
    receipt->session = *session;
    receipt->done = *done;
    receipt->status = status;
}
