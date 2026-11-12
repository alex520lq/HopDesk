#include "xfer_clipboard_txn.h"
#include <string.h>

void hop_clipboard_txn_init(hop_clipboard_txn *txn) {
    if (txn) memset(txn, 0, sizeof(*txn));
}
void hop_clipboard_txn_set_stopping(hop_clipboard_txn *txn, int stopping) {
    if (txn) txn->stopping = stopping != 0;
}
uint64_t hop_clipboard_txn_publish_local_offer(hop_clipboard_txn *txn) {
    if (!txn) return 0;
    uint64_t generation = ++txn->local_offer_sequence;
    if (!generation) generation = ++txn->local_offer_sequence;
    txn->local_offer_generation = generation;
    return generation;
}
uint64_t hop_clipboard_txn_clear_local_offer(hop_clipboard_txn *txn) {
    if (!txn) return 0;
    uint64_t generation = txn->local_offer_generation;
    txn->local_offer_generation = 0;
    return generation;
}
void hop_clipboard_txn_set_remote_offer(hop_clipboard_txn *txn, uint32_t origin, uint64_t generation) {
    if (!txn) return;
    txn->remote_offer_origin = origin;
    txn->remote_offer_generation = generation;
    hop_clipboard_txn_cancel_pending_paste(txn, 0);
}
hop_clipboard_offer_key hop_clipboard_txn_clear_remote_offer(hop_clipboard_txn *txn) {
    hop_clipboard_offer_key old = {0, 0};
    if (!txn) return old;
    old.origin = txn->remote_offer_origin;
    old.generation = txn->remote_offer_generation;
    txn->remote_offer_origin = 0;
    txn->remote_offer_generation = 0;
    hop_clipboard_txn_cancel_pending_paste(txn, 0);
    return old;
}
int hop_clipboard_txn_clear_offer_if_match(hop_clipboard_txn *txn, uint32_t local_origin,
                                            uint32_t origin, uint64_t generation) {
    if (!txn || !origin || !generation) return 0;
    if (origin == local_origin && txn->local_offer_generation == generation) {
        (void)hop_clipboard_txn_clear_local_offer(txn);
        return 1;
    }
    if (txn->remote_offer_origin == origin && txn->remote_offer_generation == generation) {
        (void)hop_clipboard_txn_clear_remote_offer(txn);
        return 2;
    }
    return 0;
}
int hop_clipboard_txn_begin_paste(hop_clipboard_txn *txn, uint64_t current_version,
                                  uint64_t observed_version, uint32_t request_flag,
                                  uint32_t *request_id, uint32_t *origin,
                                  uint64_t *generation, uint64_t *expected_version) {
    if (!txn || !request_id || !origin || !generation || !expected_version ||
        txn->stopping || !txn->remote_offer_origin || !txn->remote_offer_generation ||
        current_version != observed_version) return 0;
    if (txn->pending_request_id) {
        *request_id = txn->pending_request_id;
        *origin = txn->remote_offer_origin;
        *generation = txn->remote_offer_generation;
        *expected_version = txn->pending_clipboard_version;
        return 2;
    }
    if (++txn->request_sequence == 0 || txn->request_sequence > 0x7fffffffu)
        txn->request_sequence = 1;
    txn->pending_request_id = request_flag | txn->request_sequence;
    txn->pending_clipboard_version = current_version;
    *request_id = txn->pending_request_id;
    *origin = txn->remote_offer_origin;
    *generation = txn->remote_offer_generation;
    *expected_version = txn->pending_clipboard_version;
    return 1;
}
void hop_clipboard_txn_cancel_pending_paste(hop_clipboard_txn *txn, uint32_t request_id) {
    if (!txn) return;
    if (!request_id || txn->pending_request_id == request_id) {
        txn->pending_request_id = 0;
        txn->pending_clipboard_version = 0;
        txn->pending_paste_staged = 0;
    }
}
int hop_clipboard_txn_complete_paste(hop_clipboard_txn *txn, uint32_t request_id,
                                     uint64_t current_version, int success,
                                     uint64_t *expected_version) {
    if (!txn || !request_id || txn->pending_request_id != request_id) return 0;
    if (txn->pending_paste_staged) return 0;
    if (expected_version) *expected_version = txn->pending_clipboard_version;
    if (!success || txn->pending_clipboard_version != current_version) {
        hop_clipboard_txn_cancel_pending_paste(txn, request_id);
        return 0;
    }
    txn->pending_paste_staged = 1;
    return 1;
}
static hop_clipboard_request_result check_request(
    const hop_clipboard_txn *txn, uint32_t request_id, uint64_t offer_generation,
    int worker_available) {
    if (!txn || !request_id) return HOP_CLIPBOARD_REQUEST_STALE;
    if (txn->stopping) return HOP_CLIPBOARD_REQUEST_BUSY;
    /* A BUSY reply to a duplicate would cancel the peer's original request. */
    for (unsigned i = 0; i < HOP_CLIPBOARD_TXN_SEEN_REQUESTS; ++i)
        if (txn->seen_requests[i] == request_id) return HOP_CLIPBOARD_REQUEST_DUPLICATE;
    if (!worker_available || txn->paste_scan_active) return HOP_CLIPBOARD_REQUEST_BUSY;
    if (!offer_generation || offer_generation != txn->local_offer_generation)
        return HOP_CLIPBOARD_REQUEST_STALE;
    return HOP_CLIPBOARD_REQUEST_ACCEPTED;
}
int hop_clipboard_txn_get_pending_paste(const hop_clipboard_txn *txn,
                                        uint32_t request_id, uint64_t *expected_version) {
    if (!txn || !request_id || txn->pending_request_id != request_id) return 0;
    if (expected_version) *expected_version = txn->pending_clipboard_version;
    return 1;
}
int hop_clipboard_txn_get_staged_paste(const hop_clipboard_txn *txn, uint32_t request_id,
                                       uint64_t *expected_version) {
    if (!hop_clipboard_txn_get_pending_paste(txn,request_id,expected_version) ||
        !txn->pending_paste_staged || txn->stopping) return 0;
    return 1;
}
int hop_clipboard_txn_finish_pending_paste(hop_clipboard_txn *txn, uint32_t request_id,
                                          uint64_t expected_version, uint64_t current_version) {
    if (!txn || !request_id || txn->pending_request_id != request_id || !txn->pending_paste_staged ||
        txn->pending_clipboard_version != expected_version || current_version != expected_version)
        return 0;
    hop_clipboard_txn_cancel_pending_paste(txn, request_id);
    return 1;
}
int hop_clipboard_txn_update_pending_paste_version(hop_clipboard_txn *txn, uint32_t request_id,
                                                   uint64_t expected_version, uint64_t current_version,
                                                   uint64_t new_version) {
    if (!txn || !request_id || txn->pending_request_id != request_id || !txn->pending_paste_staged ||
        txn->pending_clipboard_version != expected_version || current_version != expected_version)
        return 0;
    txn->pending_clipboard_version = new_version;
    return 1;
}
int hop_clipboard_txn_finish_pending_paste_and_clear_offer(hop_clipboard_txn *txn, uint32_t request_id,
                                                          uint64_t expected_version, uint64_t current_version) {
    if (!hop_clipboard_txn_finish_pending_paste(txn,request_id,expected_version,current_version)) return 0;
    (void)hop_clipboard_txn_clear_remote_offer(txn);
    return 1;
}
hop_clipboard_request_result hop_clipboard_txn_accept_request(
    hop_clipboard_txn *txn, uint32_t request_id, uint64_t offer_generation,
    int worker_available, int (*snapshot)(void *), void *context) {
    hop_clipboard_request_result result =
        check_request(txn, request_id, offer_generation, worker_available);
    if (result != HOP_CLIPBOARD_REQUEST_ACCEPTED) return result;
    if (!snapshot || !snapshot(context)) return HOP_CLIPBOARD_REQUEST_BUSY;
    txn->seen_requests[txn->seen_next++ % HOP_CLIPBOARD_TXN_SEEN_REQUESTS] = request_id;
    txn->paste_scan_active = 1;
    return HOP_CLIPBOARD_REQUEST_ACCEPTED;
}
void hop_clipboard_txn_finish_paste_worker(hop_clipboard_txn *txn) {
    if (txn) txn->paste_scan_active = 0;
}
