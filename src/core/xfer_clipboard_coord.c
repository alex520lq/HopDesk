#include "xfer_clipboard_coord.h"

#include <string.h>

static hop_clip_outcome outcome_none(void) {
    hop_clip_outcome out;
    memset(&out, 0, sizeof(out));
    out.action = HOP_CLIP_ACT_NONE;
    return out;
}

hop_clip_outcome hop_clip_coord_remote_offer(hop_clipboard_txn *txn,
    uint32_t local_device, uint32_t origin, uint64_t generation) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;
    /* An offer echoed from our own device is not a peer offer. */
    if (origin == local_device) return out;
    /* Shutdown already retired clipboard state; do not resurrect it. */
    if (txn->stopping) return out;

    out.cleared_generation = hop_clipboard_txn_clear_local_offer(txn);
    out.notify_peer = out.cleared_generation != 0;
    hop_clipboard_txn_set_remote_offer(txn, origin, generation);
    out.action = HOP_CLIP_ACT_CLEAR_LOCAL_OFFER;
    return out;
}

hop_clip_outcome hop_clip_coord_offer_clear(hop_clipboard_txn *txn,
    uint32_t local_device, uint32_t origin, uint64_t generation) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;
    if (hop_clipboard_txn_clear_offer_if_match(txn, local_device, origin, generation) == 1) {
        /* The transaction already retired the local identity; the adapter owns
         * the native path array and must drop it. No peer notification: the
         * clear came from the peer. */
        out.action = HOP_CLIP_ACT_CLEAR_LOCAL_OFFER;
    }
    return out;
}

hop_clip_outcome hop_clip_coord_paste_request(hop_clipboard_txn *txn,
    uint32_t request_id, uint32_t origin, uint64_t generation,
    uint32_t local_device, int worker_available,
    int (*snapshot)(void *), void *snapshot_ctx) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;
    out.request_id = request_id;

    if (origin != local_device) {
        out.action = HOP_CLIP_ACT_SEND_PASTE_FAILED;
        out.fail_reason = HOP_CLIP_PASTE_FAIL_STALE;
        return out;
    }

    switch (hop_clipboard_txn_accept_request(txn, request_id, generation,
                                             worker_available, snapshot, snapshot_ctx)) {
    case HOP_CLIPBOARD_REQUEST_ACCEPTED:
        out.action = HOP_CLIP_ACT_SPAWN_PASTE_WORKER;
        out.generation = generation;
        break;
    case HOP_CLIPBOARD_REQUEST_DUPLICATE:
        /* Already accepted once: stay silent so a retried request cannot
         * trigger a second failure reply. */
        out.action = HOP_CLIP_ACT_NONE;
        break;
    case HOP_CLIPBOARD_REQUEST_STALE:
        out.action = HOP_CLIP_ACT_SEND_PASTE_FAILED;
        out.fail_reason = HOP_CLIP_PASTE_FAIL_STALE;
        break;
    case HOP_CLIPBOARD_REQUEST_BUSY:
    default:
        out.action = HOP_CLIP_ACT_SEND_PASTE_FAILED;
        out.fail_reason = HOP_CLIP_PASTE_FAIL_BUSY;
        break;
    }
    return out;
}

hop_clip_outcome hop_clip_coord_begin_paste(hop_clipboard_txn *txn,
    uint64_t current_version, uint64_t observed_version, uint32_t request_flag) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;

    switch (hop_clipboard_txn_begin_paste(txn, current_version, observed_version,
                                          request_flag, &out.request_id, &out.origin,
                                          &out.generation, &out.expected_version)) {
    case 1:
        out.action = HOP_CLIP_ACT_SEND_PASTE_REQUEST;
        break;
    case 2:
        /* The same clipboard version is already being fetched; report success
         * without sending a second request. */
        out.action = HOP_CLIP_ACT_RESEND_PENDING;
        break;
    default:
        out.action = HOP_CLIP_ACT_NONE;
        break;
    }
    return out;
}

hop_clip_outcome hop_clip_coord_complete_paste(hop_clipboard_txn *txn,
    uint32_t request_id, uint64_t current_version, int success) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;

    if (txn->stopping) {
        out.action = HOP_CLIP_ACT_CANCEL_PENDING;
        return out;
    }

    if (hop_clipboard_txn_complete_paste(txn, request_id, current_version, success,
                                         &out.expected_version)) {
        out.action = HOP_CLIP_ACT_APPLY_PASTE;
        out.request_id = request_id;
    }
    return out;
}

hop_clip_outcome hop_clip_coord_link_state(hop_clipboard_txn *txn,
    int connected, int local_offer_count) {
    hop_clip_outcome out = outcome_none();
    if (!txn) return out;
    /* Shutdown retired clipboard state on purpose; a late link transition must
     * not replay an offer or reach for the peer again. */
    if (txn->stopping) return out;
    if (connected) {
        if (txn->local_offer_generation && local_offer_count > 0) {
            out.action = HOP_CLIP_ACT_RESEND_LOCAL_OFFER;
            out.generation = txn->local_offer_generation;
        }
        return out;
    }
    /* The peer's offer is unreachable now, but ours stays valid: the reconnect
     * path replays it at the same generation. */
    (void)hop_clipboard_txn_clear_remote_offer(txn);
    out.action = HOP_CLIP_ACT_FORGET_REMOTE_OFFER;
    return out;
}

static hop_clip_retired_offers retire_offers(hop_clipboard_txn *txn,
    hop_clip_notify notify) {
    hop_clip_retired_offers out;
    memset(&out, 0, sizeof(out));
    out.remote = hop_clipboard_txn_clear_remote_offer(txn);
    out.local_generation = hop_clipboard_txn_clear_local_offer(txn);
    /* Nothing was live, so there is nothing to announce. */
    out.notify = (out.remote.generation || out.local_generation)
        ? notify : HOP_CLIP_NOTIFY_NONE;
    return out;
}

hop_clip_retired_offers hop_clip_coord_retire_offers(hop_clipboard_txn *txn) {
    hop_clip_retired_offers out;
    memset(&out, 0, sizeof(out));
    if (!txn) return out;
    return retire_offers(txn, HOP_CLIP_NOTIFY_BOTH);
}

hop_clip_retired_offers hop_clip_coord_consume_offers(hop_clipboard_txn *txn) {
    hop_clip_retired_offers out;
    memset(&out, 0, sizeof(out));
    if (!txn) return out;
    return retire_offers(txn, HOP_CLIP_NOTIFY_LOCAL_ONLY);
}

hop_clip_retired_offers hop_clip_coord_publish_local_offer(hop_clipboard_txn *txn,
    uint64_t *new_generation) {
    hop_clip_retired_offers out;
    memset(&out, 0, sizeof(out));
    if (new_generation) *new_generation = 0;
    if (!txn) return out;
    out = retire_offers(txn, HOP_CLIP_NOTIFY_BOTH);
    if (new_generation) *new_generation = hop_clipboard_txn_publish_local_offer(txn);
    return out;
}
