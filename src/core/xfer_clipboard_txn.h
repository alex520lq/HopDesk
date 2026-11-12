#ifndef HOP_XFER_CLIPBOARD_TXN_H
#define HOP_XFER_CLIPBOARD_TXN_H

#include <stdint.h>

#define HOP_CLIPBOARD_TXN_SEEN_REQUESTS 32u

typedef struct {
    uint64_t local_offer_generation;
    uint64_t local_offer_sequence;
    uint32_t request_sequence;
    uint32_t remote_offer_origin;
    uint64_t remote_offer_generation;
    uint32_t pending_request_id;
    uint64_t pending_clipboard_version;
    uint32_t seen_requests[HOP_CLIPBOARD_TXN_SEEN_REQUESTS];
    unsigned seen_next;
    int paste_scan_active;
    int stopping;
} hop_clipboard_txn;

typedef struct {
    uint32_t origin;
    uint64_t generation;
} hop_clipboard_offer_key;

typedef enum {
    HOP_CLIPBOARD_REQUEST_ACCEPTED = 1,
    HOP_CLIPBOARD_REQUEST_DUPLICATE = 2,
    HOP_CLIPBOARD_REQUEST_BUSY = 3,
    HOP_CLIPBOARD_REQUEST_STALE = 4
} hop_clipboard_request_result;

/* The caller serializes every operation with its platform lock/queue. */
void hop_clipboard_txn_init(hop_clipboard_txn *txn);
void hop_clipboard_txn_set_stopping(hop_clipboard_txn *txn, int stopping);
uint64_t hop_clipboard_txn_publish_local_offer(hop_clipboard_txn *txn);
uint64_t hop_clipboard_txn_clear_local_offer(hop_clipboard_txn *txn);
void hop_clipboard_txn_set_remote_offer(hop_clipboard_txn *txn, uint32_t origin, uint64_t generation);
hop_clipboard_offer_key hop_clipboard_txn_clear_remote_offer(hop_clipboard_txn *txn);
int hop_clipboard_txn_clear_offer_if_match(hop_clipboard_txn *txn, uint32_t local_origin,
                                            uint32_t origin, uint64_t generation);
int hop_clipboard_txn_begin_paste(hop_clipboard_txn *txn, uint64_t current_clipboard_version,
                                  uint64_t observed_clipboard_version, uint32_t request_flag,
                                  uint32_t *request_id, uint32_t *origin,
                                  uint64_t *generation, uint64_t *expected_version);
void hop_clipboard_txn_cancel_pending_paste(hop_clipboard_txn *txn, uint32_t request_id);
int hop_clipboard_txn_complete_paste(hop_clipboard_txn *txn, uint32_t request_id,
                                     uint64_t current_clipboard_version, int success,
                                     uint64_t *expected_version);
int hop_clipboard_txn_get_pending_paste(const hop_clipboard_txn *txn,
                                        uint32_t request_id, uint64_t *expected_version);
/* Under the same lock, snapshot the native paths before recording admission.
 * The callback must not reenter the transaction. A failed snapshot leaves it unchanged. */
hop_clipboard_request_result hop_clipboard_txn_accept_request(
    hop_clipboard_txn *txn, uint32_t request_id, uint64_t offer_generation,
    int worker_available, int (*snapshot)(void *), void *context);
void hop_clipboard_txn_finish_paste_worker(hop_clipboard_txn *txn);

#endif
