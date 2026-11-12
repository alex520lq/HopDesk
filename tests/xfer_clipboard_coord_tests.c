#include "xfer_clipboard_coord.h"

#include <assert.h>
#include <stdio.h>

static int snapshot_ok(void *context) {
    return *(int *)context;
}

static int snapshot_fail(void *context) {
    (void)context;
    return 0;
}

int main(void) {
    const uint32_t local = 11u;
    const uint32_t peer = 22u;

    /* ---- inbound FILE_OFFER ---- */
    hop_clipboard_txn t;
    hop_clipboard_txn_init(&t);

    assert(hop_clip_coord_remote_offer(NULL, local, peer, 5).action == HOP_CLIP_ACT_NONE);

    /* Self-originated offers are ignored and leave both identities untouched. */
    uint64_t local_gen = hop_clipboard_txn_publish_local_offer(&t);
    hop_clip_outcome out = hop_clip_coord_remote_offer(&t, local, local, 5);
    assert(out.action == HOP_CLIP_ACT_NONE);
    assert(t.local_offer_generation == local_gen && !t.remote_offer_origin);

    /* A post-stop offer must not resurrect clipboard state. */
    hop_clipboard_txn_set_stopping(&t, 1);
    assert(hop_clip_coord_remote_offer(&t, local, peer, 5).action == HOP_CLIP_ACT_NONE);
    assert(!t.remote_offer_origin);
    hop_clipboard_txn_set_stopping(&t, 0);

    /* A real peer offer retires the local one and becomes current. */
    out = hop_clip_coord_remote_offer(&t, local, peer, 5);
    assert(out.action == HOP_CLIP_ACT_CLEAR_LOCAL_OFFER);
    assert(out.cleared_generation == local_gen && out.notify_peer == 1);
    assert(!t.local_offer_generation);
    assert(t.remote_offer_origin == peer && t.remote_offer_generation == 5);

    /* ---- inbound FILE_OFFER_CLEAR ---- */
    /* A mismatched generation is inert. */
    assert(hop_clip_coord_offer_clear(&t, local, peer, 4).action == HOP_CLIP_ACT_NONE);
    assert(t.remote_offer_origin == peer && t.remote_offer_generation == 5);

    /* A matching peer identity clears the remote offer without touching paths. */
    out = hop_clip_coord_offer_clear(&t, local, peer, 5);
    assert(out.action == HOP_CLIP_ACT_NONE);
    assert(!t.remote_offer_origin && !t.remote_offer_generation);

    /* A matching local identity asks the adapter to drop its path array. */
    local_gen = hop_clipboard_txn_publish_local_offer(&t);
    out = hop_clip_coord_offer_clear(&t, local, local, local_gen);
    assert(out.action == HOP_CLIP_ACT_CLEAR_LOCAL_OFFER);
    assert(out.notify_peer == 0 && out.cleared_generation == 0);
    assert(!t.local_offer_generation);

    /* ---- inbound PASTE_REQUEST ---- */
    local_gen = hop_clipboard_txn_publish_local_offer(&t);

    /* Foreign origin is stale before any admission work happens. */
    out = hop_clip_coord_paste_request(&t, 900, peer, local_gen, local, 1,
                                       snapshot_fail, NULL);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_FAILED);
    assert(out.fail_reason == HOP_CLIP_PASTE_FAIL_STALE && out.request_id == 900);
    assert(!t.seen_next && !t.paste_scan_active);

    /* A generation that does not match the current offer is stale. */
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen + 1, local, 1,
                                       snapshot_fail, NULL);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_FAILED);
    assert(out.fail_reason == HOP_CLIP_PASTE_FAIL_STALE);

    /* A failed native snapshot is reported as busy and leaves no history. */
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen, local, 1,
                                       snapshot_fail, NULL);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_FAILED);
    assert(out.fail_reason == HOP_CLIP_PASTE_FAIL_BUSY);
    assert(!t.seen_next && !t.paste_scan_active);

    /* No worker slot is also busy. */
    int snapshot = 1;
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen, local, 0,
                                       snapshot_ok, &snapshot);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_FAILED);
    assert(out.fail_reason == HOP_CLIP_PASTE_FAIL_BUSY);

    /* Accepted: the adapter must start a worker. */
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen, local, 1,
                                       snapshot_ok, &snapshot);
    assert(out.action == HOP_CLIP_ACT_SPAWN_PASTE_WORKER);
    assert(out.request_id == 900 && out.generation == local_gen);
    assert(t.paste_scan_active);

    /* A retried request while the worker runs is busy, not a duplicate. */
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen, local, 1,
                                       snapshot_ok, &snapshot);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_FAILED);
    assert(out.fail_reason == HOP_CLIP_PASTE_FAIL_BUSY);

    /* Once the worker retires, the same request id is a silent duplicate. */
    hop_clipboard_txn_finish_paste_worker(&t);
    out = hop_clip_coord_paste_request(&t, 900, local, local_gen, local, 1,
                                       snapshot_ok, &snapshot);
    assert(out.action == HOP_CLIP_ACT_NONE);
    hop_clipboard_txn_finish_paste_worker(&t);

    /* ---- outbound PASTE_REQUEST ---- */
    hop_clipboard_txn_init(&t);
    /* No peer offer: nothing to request. */
    out = hop_clip_coord_begin_paste(&t, 123, 123, 0x80000000u);
    assert(out.action == HOP_CLIP_ACT_NONE);

    hop_clipboard_txn_set_remote_offer(&t, peer, 88);
    /* Clipboard changed since the offer was observed: refuse. */
    out = hop_clip_coord_begin_paste(&t, 124, 123, 0x80000000u);
    assert(out.action == HOP_CLIP_ACT_NONE);

    out = hop_clip_coord_begin_paste(&t, 124, 124, 0x80000000u);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_REQUEST);
    assert(out.request_id == 0x80000001u);
    assert(out.origin == peer && out.generation == 88 && out.expected_version == 124);

    /* The same version is already in flight: report success, send nothing. */
    hop_clip_outcome again = hop_clip_coord_begin_paste(&t, 124, 124, 0x80000000u);
    assert(again.action == HOP_CLIP_ACT_RESEND_PENDING);
    assert(again.request_id == out.request_id && again.expected_version == out.expected_version);

    /* ---- completion ---- */
    /* Unknown request id is inert. */
    assert(hop_clip_coord_complete_paste(&t, out.request_id + 1, 124, 1).action ==
           HOP_CLIP_ACT_NONE);
    /* A clipboard generation that no longer matches refuses the paste. */
    assert(hop_clip_coord_complete_paste(&t, out.request_id, 125, 1).action ==
           HOP_CLIP_ACT_NONE);
    assert(!t.pending_request_id);

    out = hop_clip_coord_begin_paste(&t, 126, 126, 0x80000000u);
    assert(out.action == HOP_CLIP_ACT_SEND_PASTE_REQUEST);
    hop_clip_outcome done = hop_clip_coord_complete_paste(&t, out.request_id, 126, 1);
    assert(done.action == HOP_CLIP_ACT_APPLY_PASTE);
    assert(done.expected_version == 126);

    /* Stopping cancels instead of applying. */
    hop_clipboard_txn_set_stopping(&t, 1);
    assert(hop_clip_coord_complete_paste(&t, out.request_id, 126, 1).action ==
           HOP_CLIP_ACT_CANCEL_PENDING);
    hop_clipboard_txn_set_stopping(&t, 0);

    /* ---- retiring and publishing the local identity ---- */
    hop_clipboard_txn_init(&t);
    local_gen = hop_clipboard_txn_publish_local_offer(&t);
    hop_clipboard_txn_set_remote_offer(&t, peer, 70);
    hop_clip_retired_offers retired = hop_clip_coord_retire_offers(&t);
    assert(retired.remote.origin == peer && retired.remote.generation == 70);
    assert(retired.local_generation == local_gen);
    assert(!t.remote_offer_origin && !t.remote_offer_generation && !t.local_offer_generation);

    /* Publishing retires both identities and hands back a fresh generation. */
    hop_clipboard_txn_init(&t);
    local_gen = hop_clipboard_txn_publish_local_offer(&t);
    hop_clipboard_txn_set_remote_offer(&t, peer, 71);
    uint64_t fresh = 0;
    retired = hop_clip_coord_publish_local_offer(&t, &fresh);
    assert(retired.remote.origin == peer && retired.remote.generation == 71);
    assert(retired.local_generation == local_gen);
    assert(fresh == local_gen + 1 && t.local_offer_generation == fresh);
    assert(!t.remote_offer_origin);

    /* Null transaction is inert and never writes a generation. */
    assert(hop_clip_coord_retire_offers(NULL).local_generation == 0);
    uint64_t untouched = 7;
    assert(hop_clip_coord_publish_local_offer(NULL, &untouched).local_generation == 0);
    assert(untouched == 0);
    assert(hop_clip_coord_consume_offers(NULL).notify == HOP_CLIP_NOTIFY_NONE);

    /* A clipboard change announces both identities to the peer. */
    hop_clipboard_txn_init(&t);
    local_gen = hop_clipboard_txn_publish_local_offer(&t);
    hop_clipboard_txn_set_remote_offer(&t, peer, 72);
    retired = hop_clip_coord_retire_offers(&t);
    assert(retired.notify == HOP_CLIP_NOTIFY_BOTH);
    assert(retired.remote.generation == 72 && retired.local_generation == local_gen);

    /* Consuming a remote paste retires both but only announces our own
     * generation: the peer still owns the offer it published. */
    hop_clipboard_txn_init(&t);
    local_gen = hop_clipboard_txn_publish_local_offer(&t);
    hop_clipboard_txn_set_remote_offer(&t, peer, 73);
    retired = hop_clip_coord_consume_offers(&t);
    assert(retired.notify == HOP_CLIP_NOTIFY_LOCAL_ONLY);
    assert(retired.remote.origin == peer && retired.remote.generation == 73);
    assert(retired.local_generation == local_gen);
    assert(!t.remote_offer_origin && !t.local_offer_generation);

    /* Publishing also announces both, so the peer drops the offer it held. */
    hop_clipboard_txn_init(&t);
    hop_clipboard_txn_publish_local_offer(&t);
    hop_clipboard_txn_set_remote_offer(&t, peer, 74);
    retired = hop_clip_coord_publish_local_offer(&t, &fresh);
    assert(retired.notify == HOP_CLIP_NOTIFY_BOTH);

    /* Nothing live means nothing to announce, whichever intent is used. */
    hop_clipboard_txn_init(&t);
    assert(hop_clip_coord_retire_offers(&t).notify == HOP_CLIP_NOTIFY_NONE);
    assert(hop_clip_coord_consume_offers(&t).notify == HOP_CLIP_NOTIFY_NONE);
    assert(hop_clip_coord_publish_local_offer(&t, &fresh).notify == HOP_CLIP_NOTIFY_NONE);

    puts("PASS: clipboard orchestration decisions");
    return 0;
}
