#include "xfer_clipboard_txn.h"
#include <string.h>

static uint64_t hop_clipboard_txn_publish_local_offer(hop_clipboard_txn *txn);
static uint64_t hop_clipboard_txn_clear_local_offer(hop_clipboard_txn *txn);
static void hop_clipboard_txn_set_remote_offer(hop_clipboard_txn *txn,
                                               uint32_t origin,
                                               uint64_t generation);
static hop_clipboard_offer_key
hop_clipboard_txn_clear_remote_offer(hop_clipboard_txn *txn);
static int hop_clipboard_txn_clear_offer_if_match(hop_clipboard_txn *txn,
                                                  uint32_t local_origin,
                                                  uint32_t origin,
                                                  uint64_t generation);
static int hop_clipboard_txn_begin_paste(hop_clipboard_txn *txn,
                                         uint64_t current, uint64_t observed,
                                         uint32_t flag, uint32_t *id,
                                         uint32_t *origin, uint64_t *generation,
                                         uint64_t *expected);
static int hop_clipboard_txn_complete_paste(hop_clipboard_txn *txn, uint32_t id,
                                            uint64_t version, int success,
                                            uint64_t *expected);
static int hop_clipboard_txn_finish_pending_paste(hop_clipboard_txn *txn,
                                                  uint32_t id,
                                                  uint64_t expected,
                                                  uint64_t current);
static int
hop_clipboard_txn_update_pending_paste_version(hop_clipboard_txn *txn,
                                               uint32_t id, uint64_t expected,
                                               uint64_t current, uint64_t next);
static int hop_clipboard_txn_finish_pending_paste_and_clear_offer(
    hop_clipboard_txn *txn, uint32_t id, uint64_t expected, uint64_t current);
static hop_clipboard_request_result
hop_clipboard_txn_accept_request(hop_clipboard_txn *txn, uint32_t id,
                                 uint64_t generation, int available,
                                 int (*snapshot)(void *), void *context);

void hop_clipboard_txn_init(hop_clipboard_txn *txn) {
  if (txn)
    memset(txn, 0, sizeof(*txn));
}
void hop_clipboard_txn_set_stopping(hop_clipboard_txn *txn, int stopping) {
  if (txn)
    txn->stopping = stopping != 0;
}
static uint64_t hop_clipboard_txn_publish_local_offer(hop_clipboard_txn *txn) {
  if (!txn)
    return 0;
  uint64_t generation = ++txn->local_offer_sequence;
  if (!generation)
    generation = ++txn->local_offer_sequence;
  txn->local_offer_generation = generation;
  return generation;
}
static uint64_t hop_clipboard_txn_clear_local_offer(hop_clipboard_txn *txn) {
  if (!txn)
    return 0;
  uint64_t generation = txn->local_offer_generation;
  txn->local_offer_generation = 0;
  return generation;
}
static void hop_clipboard_txn_set_remote_offer(hop_clipboard_txn *txn,
                                               uint32_t origin,
                                               uint64_t generation) {
  if (!txn)
    return;
  txn->remote_offer_origin = origin;
  txn->remote_offer_generation = generation;
  hop_clipboard_txn_cancel_pending_paste(txn, 0);
}
static hop_clipboard_offer_key
hop_clipboard_txn_clear_remote_offer(hop_clipboard_txn *txn) {
  hop_clipboard_offer_key old = {0, 0};
  if (!txn)
    return old;
  old.origin = txn->remote_offer_origin;
  old.generation = txn->remote_offer_generation;
  txn->remote_offer_origin = 0;
  txn->remote_offer_generation = 0;
  hop_clipboard_txn_cancel_pending_paste(txn, 0);
  return old;
}
static int hop_clipboard_txn_clear_offer_if_match(hop_clipboard_txn *txn,
                                                  uint32_t local_origin,
                                                  uint32_t origin,
                                                  uint64_t generation) {
  if (!txn || !origin || !generation)
    return 0;
  if (origin == local_origin && txn->local_offer_generation == generation) {
    (void)hop_clipboard_txn_clear_local_offer(txn);
    return 1;
  }
  if (txn->remote_offer_origin == origin &&
      txn->remote_offer_generation == generation) {
    (void)hop_clipboard_txn_clear_remote_offer(txn);
    return 2;
  }
  return 0;
}
static int hop_clipboard_txn_begin_paste(
    hop_clipboard_txn *txn, uint64_t current_version, uint64_t observed_version,
    uint32_t request_flag, uint32_t *request_id, uint32_t *origin,
    uint64_t *generation, uint64_t *expected_version) {
  if (!txn || !request_id || !origin || !generation || !expected_version ||
      txn->stopping || !txn->remote_offer_origin ||
      !txn->remote_offer_generation || current_version != observed_version)
    return 0;
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
void hop_clipboard_txn_cancel_pending_paste(hop_clipboard_txn *txn,
                                            uint32_t request_id) {
  if (!txn)
    return;
  if (!request_id || txn->pending_request_id == request_id) {
    txn->pending_request_id = 0;
    txn->pending_clipboard_version = 0;
    txn->pending_paste_staged = 0;
  }
}
static int hop_clipboard_txn_complete_paste(hop_clipboard_txn *txn,
                                            uint32_t request_id,
                                            uint64_t current_version,
                                            int success,
                                            uint64_t *expected_version) {
  if (!txn || !request_id || txn->pending_request_id != request_id)
    return 0;
  if (txn->pending_paste_staged)
    return 0;
  if (expected_version)
    *expected_version = txn->pending_clipboard_version;
  if (!success || txn->pending_clipboard_version != current_version) {
    hop_clipboard_txn_cancel_pending_paste(txn, request_id);
    return 0;
  }
  txn->pending_paste_staged = 1;
  return 1;
}
static hop_clipboard_request_result check_request(const hop_clipboard_txn *txn,
                                                  uint32_t request_id,
                                                  uint64_t offer_generation,
                                                  int worker_available) {
  if (!txn || !request_id)
    return HOP_CLIPBOARD_REQUEST_STALE;
  if (txn->stopping)
    return HOP_CLIPBOARD_REQUEST_BUSY;
  /* A BUSY reply to a duplicate would cancel the peer's original request. */
  for (unsigned i = 0; i < HOP_CLIPBOARD_TXN_SEEN_REQUESTS; ++i)
    if (txn->seen_requests[i] == request_id)
      return HOP_CLIPBOARD_REQUEST_DUPLICATE;
  if (!worker_available || txn->paste_scan_active)
    return HOP_CLIPBOARD_REQUEST_BUSY;
  if (!offer_generation || offer_generation != txn->local_offer_generation)
    return HOP_CLIPBOARD_REQUEST_STALE;
  return HOP_CLIPBOARD_REQUEST_ACCEPTED;
}
int hop_clipboard_txn_get_pending_paste(const hop_clipboard_txn *txn,
                                        uint32_t request_id,
                                        uint64_t *expected_version) {
  if (!txn || !request_id || txn->pending_request_id != request_id)
    return 0;
  if (expected_version)
    *expected_version = txn->pending_clipboard_version;
  return 1;
}
int hop_clipboard_txn_get_staged_paste(const hop_clipboard_txn *txn,
                                       uint32_t request_id,
                                       uint64_t *expected_version) {
  if (!hop_clipboard_txn_get_pending_paste(txn, request_id, expected_version) ||
      !txn->pending_paste_staged || txn->stopping)
    return 0;
  return 1;
}
static int hop_clipboard_txn_finish_pending_paste(hop_clipboard_txn *txn,
                                                  uint32_t request_id,
                                                  uint64_t expected_version,
                                                  uint64_t current_version) {
  if (!txn || !request_id || txn->pending_request_id != request_id ||
      !txn->pending_paste_staged ||
      txn->pending_clipboard_version != expected_version ||
      current_version != expected_version)
    return 0;
  hop_clipboard_txn_cancel_pending_paste(txn, request_id);
  return 1;
}
static int hop_clipboard_txn_update_pending_paste_version(
    hop_clipboard_txn *txn, uint32_t request_id, uint64_t expected_version,
    uint64_t current_version, uint64_t new_version) {
  if (!txn || !request_id || txn->pending_request_id != request_id ||
      !txn->pending_paste_staged ||
      txn->pending_clipboard_version != expected_version ||
      current_version != expected_version)
    return 0;
  txn->pending_clipboard_version = new_version;
  return 1;
}
static int hop_clipboard_txn_finish_pending_paste_and_clear_offer(
    hop_clipboard_txn *txn, uint32_t request_id, uint64_t expected_version,
    uint64_t current_version) {
  if (!hop_clipboard_txn_finish_pending_paste(txn, request_id, expected_version,
                                              current_version))
    return 0;
  (void)hop_clipboard_txn_clear_remote_offer(txn);
  return 1;
}
static hop_clipboard_request_result hop_clipboard_txn_accept_request(
    hop_clipboard_txn *txn, uint32_t request_id, uint64_t offer_generation,
    int worker_available, int (*snapshot)(void *), void *context) {
  hop_clipboard_request_result result =
      check_request(txn, request_id, offer_generation, worker_available);
  if (result != HOP_CLIPBOARD_REQUEST_ACCEPTED)
    return result;
  if (!snapshot || !snapshot(context))
    return HOP_CLIPBOARD_REQUEST_BUSY;
  txn->seen_requests[txn->seen_next++ % HOP_CLIPBOARD_TXN_SEEN_REQUESTS] =
      request_id;
  txn->paste_scan_active = 1;
  return HOP_CLIPBOARD_REQUEST_ACCEPTED;
}
void hop_clipboard_txn_finish_paste_worker(hop_clipboard_txn *txn) {
  if (txn)
    txn->paste_scan_active = 0;
}

/* Clipboard business rules and state transitions share one owner. */
int hop_clipboard_remote_offer(hop_clipboard_txn *txn, uint32_t local_device,
                               uint32_t origin, uint64_t generation,
                               uint64_t *cleared_local_generation) {
  if (cleared_local_generation)
    *cleared_local_generation = 0;
  if (!txn || origin == local_device || txn->stopping)
    return 0;
  uint64_t old = hop_clipboard_txn_clear_local_offer(txn);
  if (cleared_local_generation)
    *cleared_local_generation = old;
  hop_clipboard_txn_set_remote_offer(txn, origin, generation);
  return 1;
}
int hop_clipboard_offer_clear(hop_clipboard_txn *txn, uint32_t local_device,
                              uint32_t origin, uint64_t generation) {
  return hop_clipboard_txn_clear_offer_if_match(txn, local_device, origin,
                                                generation) == 1;
}
hop_clipboard_request_result
hop_clipboard_paste_request(hop_clipboard_txn *txn, uint32_t request_id,
                            uint32_t origin, uint64_t generation,
                            uint32_t local_device, int worker_available,
                            int (*snapshot)(void *), void *snapshot_ctx) {
  if (origin != local_device)
    return HOP_CLIPBOARD_REQUEST_STALE;
  return hop_clipboard_txn_accept_request(
      txn, request_id, generation, worker_available, snapshot, snapshot_ctx);
}
int hop_clipboard_begin_paste(hop_clipboard_txn *txn, uint64_t current_version,
                              uint64_t observed_version, uint32_t request_flag,
                              uint32_t *request_id, uint32_t *origin,
                              uint64_t *generation,
                              uint64_t *expected_version) {
  return hop_clipboard_txn_begin_paste(txn, current_version, observed_version,
                                       request_flag, request_id, origin,
                                       generation, expected_version);
}
int hop_clipboard_complete_paste(hop_clipboard_txn *txn, uint32_t request_id,
                                 uint64_t current_version, int success,
                                 uint64_t *expected_version) {
  if (txn && txn->stopping) {
    hop_clipboard_txn_cancel_pending_paste(txn, 0);
    return 0;
  }
  return hop_clipboard_txn_complete_paste(txn, request_id, current_version,
                                          success, expected_version);
}
hop_clip_paste_apply_result hop_clipboard_paste_apply(hop_clipboard_txn *txn,
                                                      uint32_t request_id,
                                                      uint64_t armed_version,
                                                      uint64_t native_after,
                                                      int success) {
  if (!txn || !request_id || txn->stopping)
    return HOP_CLIP_PASTE_APPLY_CANCEL;
  if (success)
    return hop_clipboard_txn_finish_pending_paste_and_clear_offer(
               txn, request_id, armed_version, armed_version)
               ? HOP_CLIP_PASTE_APPLY_COMPLETE
               : HOP_CLIP_PASTE_APPLY_CANCEL;
  return hop_clipboard_txn_update_pending_paste_version(
             txn, request_id, armed_version, armed_version, native_after)
             ? HOP_CLIP_PASTE_APPLY_REARM
             : HOP_CLIP_PASTE_APPLY_CANCEL;
}
int hop_clipboard_link_state(hop_clipboard_txn *txn, int connected,
                             int local_offer_count,
                             uint64_t *replay_generation) {
  if (replay_generation)
    *replay_generation = 0;
  if (!txn || txn->stopping)
    return 0;
  if (!connected) {
    (void)hop_clipboard_txn_clear_remote_offer(txn);
    return 0;
  }
  if (!txn->local_offer_generation || local_offer_count <= 0)
    return 0;
  if (replay_generation)
    *replay_generation = txn->local_offer_generation;
  return 1;
}
static hop_clip_retired_offers retire_offers(hop_clipboard_txn *txn,
                                             hop_clip_notify notify) {
  hop_clip_retired_offers out = {0};
  if (!txn)
    return out;
  out.remote = hop_clipboard_txn_clear_remote_offer(txn);
  out.local_generation = hop_clipboard_txn_clear_local_offer(txn);
  out.notify = (out.remote.generation || out.local_generation)
                   ? notify
                   : HOP_CLIP_NOTIFY_NONE;
  return out;
}
hop_clip_retired_offers hop_clipboard_retire_offers(hop_clipboard_txn *txn) {
  return retire_offers(txn, HOP_CLIP_NOTIFY_BOTH);
}
hop_clip_retired_offers hop_clipboard_consume_offers(hop_clipboard_txn *txn) {
  return retire_offers(txn, HOP_CLIP_NOTIFY_LOCAL_ONLY);
}
hop_clip_retired_offers
hop_clipboard_publish_local_offer(hop_clipboard_txn *txn,
                                  uint64_t *new_generation) {
  if (new_generation)
    *new_generation = 0;
  hop_clip_retired_offers out = retire_offers(txn, HOP_CLIP_NOTIFY_BOTH);
  if (txn && new_generation)
    *new_generation = hop_clipboard_txn_publish_local_offer(txn);
  return out;
}
