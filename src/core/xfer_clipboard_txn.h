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
  int pending_paste_staged;
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

/* Caller serializes state transitions with its platform lock/queue. */
void hop_clipboard_txn_init(hop_clipboard_txn *txn);
void hop_clipboard_txn_set_stopping(hop_clipboard_txn *txn, int stopping);
void hop_clipboard_txn_cancel_pending_paste(hop_clipboard_txn *txn,
                                            uint32_t request_id);
int hop_clipboard_txn_get_pending_paste(const hop_clipboard_txn *txn,
                                        uint32_t request_id,
                                        uint64_t *expected_version);
int hop_clipboard_txn_get_staged_paste(const hop_clipboard_txn *txn,
                                       uint32_t request_id,
                                       uint64_t *expected_version);
void hop_clipboard_txn_finish_paste_worker(hop_clipboard_txn *txn);

/* Clipboard transition results: values describe business state, not adapter
 * actions. */
typedef enum {
  HOP_CLIP_PASTE_APPLY_COMPLETE = 0,
  HOP_CLIP_PASTE_APPLY_REARM = 1,
  HOP_CLIP_PASTE_APPLY_CANCEL = 2
} hop_clip_paste_apply_result;

typedef enum {
  HOP_CLIP_NOTIFY_NONE = 0,
  HOP_CLIP_NOTIFY_LOCAL_ONLY,
  HOP_CLIP_NOTIFY_BOTH
} hop_clip_notify;

typedef struct {
  hop_clipboard_offer_key remote;
  uint64_t local_generation;
  hop_clip_notify notify;
} hop_clip_retired_offers;

/* Each operation mutates the single state owner and returns only the data its
 * caller needs to perform native clipboard or wire I/O. Caller serializes. */
int hop_clipboard_remote_offer(hop_clipboard_txn *txn, uint32_t local_device,
                               uint32_t origin, uint64_t generation,
                               uint64_t *cleared_local_generation);
int hop_clipboard_offer_clear(hop_clipboard_txn *txn, uint32_t local_device,
                              uint32_t origin, uint64_t generation);
hop_clipboard_request_result
hop_clipboard_paste_request(hop_clipboard_txn *txn, uint32_t request_id,
                            uint32_t origin, uint64_t generation,
                            uint32_t local_device, int worker_available,
                            int (*snapshot)(void *), void *snapshot_ctx);
int hop_clipboard_begin_paste(hop_clipboard_txn *txn, uint64_t current_version,
                              uint64_t observed_version, uint32_t request_flag,
                              uint32_t *request_id, uint32_t *origin,
                              uint64_t *generation, uint64_t *expected_version);
int hop_clipboard_complete_paste(hop_clipboard_txn *txn, uint32_t request_id,
                                 uint64_t current_version, int success,
                                 uint64_t *expected_version);
hop_clip_paste_apply_result hop_clipboard_paste_apply(hop_clipboard_txn *txn,
                                                      uint32_t request_id,
                                                      uint64_t armed_version,
                                                      uint64_t native_after,
                                                      int success);
int hop_clipboard_link_state(hop_clipboard_txn *txn, int connected,
                             int local_offer_count,
                             uint64_t *replay_generation);
hop_clip_retired_offers hop_clipboard_retire_offers(hop_clipboard_txn *txn);
hop_clip_retired_offers hop_clipboard_consume_offers(hop_clipboard_txn *txn);
hop_clip_retired_offers
hop_clipboard_publish_local_offer(hop_clipboard_txn *txn,
                                  uint64_t *new_generation);

#endif
