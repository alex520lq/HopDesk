#include "xfer_clipboard_txn.h"
#include <assert.h>
#include <stdio.h>

static int snapshot_ok(void *ctx) { return *(int *)ctx; }
int main(void) {
  hop_clipboard_txn s;
  hop_clipboard_txn_init(&s);
  uint32_t local = 11, peer = 22, id = 0, origin = 0;
  uint64_t generation = 0, version = 0, retired = 0;
  assert(!hop_clipboard_remote_offer(NULL, local, peer, 1, &retired));
  assert(hop_clipboard_retire_offers(NULL).notify == HOP_CLIP_NOTIFY_NONE);
  uint64_t untouched = 99;
  assert(hop_clipboard_publish_local_offer(NULL, &untouched).notify ==
         HOP_CLIP_NOTIFY_NONE);
  assert(untouched == 0);
  assert(!hop_clipboard_link_state(&s, 1, 2, &version));
  assert(hop_clipboard_publish_local_offer(&s, &generation).notify ==
         HOP_CLIP_NOTIFY_NONE);
  assert(generation == 1);
  assert(!hop_clipboard_remote_offer(&s, local, local, 5, &retired));
  assert(hop_clipboard_remote_offer(&s, local, peer, 5, &retired) &&
         retired == generation);
  assert(s.remote_offer_origin == peer && s.remote_offer_generation == 5 &&
         !s.local_offer_generation);
  assert(!hop_clipboard_offer_clear(&s, local, peer, 4));
  assert(!hop_clipboard_offer_clear(&s, local, peer, 5));
  assert(!s.remote_offer_origin);

  assert(hop_clipboard_publish_local_offer(&s, &generation).notify ==
         HOP_CLIP_NOTIFY_NONE);
  int snapshot = 0;
  assert(hop_clipboard_paste_request(&s, 900, peer, generation, local, 1,
                                     snapshot_ok,
                                     &snapshot) == HOP_CLIPBOARD_REQUEST_STALE);
  assert(hop_clipboard_paste_request(&s, 900, local, generation, local, 1,
                                     snapshot_ok,
                                     &snapshot) == HOP_CLIPBOARD_REQUEST_BUSY);
  snapshot = 1;
  assert(hop_clipboard_paste_request(&s, 900, local, generation, local, 1,
                                     snapshot_ok, &snapshot) ==
         HOP_CLIPBOARD_REQUEST_ACCEPTED);
  assert(hop_clipboard_paste_request(&s, 900, local, generation, local, 1,
                                     snapshot_ok, &snapshot) ==
         HOP_CLIPBOARD_REQUEST_DUPLICATE);
  assert(hop_clipboard_paste_request(&s, 901, local, generation, local, 1,
                                     snapshot_ok,
                                     &snapshot) == HOP_CLIPBOARD_REQUEST_BUSY);
  hop_clipboard_txn_finish_paste_worker(&s);
  assert(hop_clipboard_paste_request(&s, 901, local, generation + 1, local, 1,
                                     snapshot_ok,
                                     &snapshot) == HOP_CLIPBOARD_REQUEST_STALE);

  assert(hop_clipboard_remote_offer(&s, local, peer, 80, &retired));
  assert(hop_clipboard_begin_paste(&s, 123, 124, 0x80000000u, &id, &origin,
                                   &generation, &version) == 0);
  assert(hop_clipboard_begin_paste(&s, 124, 124, 0x80000000u, &id, &origin,
                                   &generation, &version) == 1);
  assert(id == 0x80000001u && origin == peer && generation == 80 &&
         version == 124);
  uint32_t id2 = 0, origin2 = 0;
  uint64_t gen2 = 0, version2 = 0;
  assert(hop_clipboard_begin_paste(&s, 124, 124, 0x80000000u, &id2, &origin2,
                                   &gen2, &version2) == 2);
  assert(id2 == id && gen2 == generation && version2 == version);
  assert(!hop_clipboard_complete_paste(&s, id + 1, 124, 1, NULL));
  assert(!hop_clipboard_complete_paste(&s, id, 125, 1, NULL));
  assert(hop_clipboard_begin_paste(&s, 125, 125, 0x80000000u, &id, &origin,
                                   &generation, &version) == 1);
  assert(hop_clipboard_complete_paste(&s, id, 125, 1, &version2) &&
         version2 == 125);
  assert(!hop_clipboard_complete_paste(&s, id, 125, 1, NULL));
  assert(s.pending_request_id == id && s.pending_paste_staged);
  assert(hop_clipboard_paste_apply(&s, id, 126, 127, 1) ==
         HOP_CLIP_PASTE_APPLY_CANCEL);
  assert(s.pending_request_id == id);
  assert(hop_clipboard_paste_apply(&s, id, 125, 126, 0) ==
         HOP_CLIP_PASTE_APPLY_REARM);
  assert(hop_clipboard_paste_apply(&s, id, 126, 127, 1) ==
         HOP_CLIP_PASTE_APPLY_COMPLETE);
  assert(!s.pending_request_id && !s.remote_offer_origin);

  assert(hop_clipboard_remote_offer(&s, local, peer, 79, &retired));
  assert(hop_clipboard_begin_paste(&s, 200, 200, 0x80000000u, &id, &origin,
                                   &generation, &version) == 1);
  assert(hop_clipboard_remote_offer(&s, local, peer, 78, &retired));
  assert(!s.pending_request_id && s.remote_offer_generation == 78);
  assert(!hop_clipboard_complete_paste(&s, id, version, 1, NULL));

  assert(hop_clipboard_remote_offer(&s, local, peer, 81, &retired));
  hop_clip_retired_offers r = hop_clipboard_retire_offers(&s);
  assert(r.remote.origin == peer && r.remote.generation == 81 &&
         r.local_generation == 0 && r.notify == HOP_CLIP_NOTIFY_BOTH);
  assert(hop_clipboard_remote_offer(&s, local, peer, 82, &retired));
  r = hop_clipboard_consume_offers(&s);
  assert(r.remote.generation == 82 && r.notify == HOP_CLIP_NOTIFY_LOCAL_ONLY);
  assert(hop_clipboard_remote_offer(&s, local, peer, 83, &retired));
  r = hop_clipboard_publish_local_offer(&s, &generation);
  assert(r.remote.origin == peer && r.remote.generation == 83 &&
         r.notify == HOP_CLIP_NOTIFY_BOTH && generation != 0);
  assert(hop_clipboard_link_state(&s, 1, 1, &version) && version == generation);
  assert(hop_clipboard_link_state(&s, 0, 1, &version) == 0 &&
         !s.remote_offer_origin);
  r = hop_clipboard_publish_local_offer(&s, &generation);
  assert(r.local_generation != 0 && r.notify == HOP_CLIP_NOTIFY_BOTH);
  assert(hop_clipboard_link_state(&s, 1, 2, &version) && version == generation);
  hop_clipboard_txn_set_stopping(&s, 1);
  assert(!hop_clipboard_link_state(&s, 1, 2, &version));
  assert(hop_clipboard_paste_request(&s, 902, local, generation, local, 1,
                                     snapshot_ok,
                                     &snapshot) == HOP_CLIPBOARD_REQUEST_BUSY);
  assert(!hop_clipboard_remote_offer(&s, local, peer, 84, &retired));
  assert(s.local_offer_generation == generation);
  assert(hop_clipboard_begin_paste(&s, 300, 300, 0x80000000u, &id, &origin,
                                   &generation, &version) == 0);
  hop_clipboard_txn_set_stopping(&s, 0);
  assert(hop_clipboard_remote_offer(&s, local, peer, 85, &retired));
  assert(hop_clipboard_begin_paste(&s, 301, 301, 0x80000000u, &id, &origin,
                                   &generation, &version) == 1);
  hop_clipboard_txn_set_stopping(&s, 1);
  assert(!hop_clipboard_complete_paste(&s, id, version, 1, NULL));
  assert(!s.pending_request_id);
  puts("PASS: shared clipboard offer identity, admission, idempotency and "
       "paste version lifecycle");
  return 0;
}
