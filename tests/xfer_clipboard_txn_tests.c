#include "xfer_clipboard_txn.h"
#include <assert.h>
#include <stdio.h>

static int snapshot_paths(void *context) {
    return *(int *)context;
}

int main(void) {
    hop_clipboard_txn t;
    hop_clipboard_txn_init(&t);
    uint64_t local = hop_clipboard_txn_publish_local_offer(&t);
    assert(local == 1 && hop_clipboard_txn_clear_local_offer(&t) == local);
    assert(t.local_offer_generation == 0);
    local = hop_clipboard_txn_publish_local_offer(&t);
    assert(local == 2);

    hop_clipboard_txn_set_remote_offer(&t, 22, 77);
    assert(hop_clipboard_txn_clear_offer_if_match(&t, 11, 22, 76) == 0);
    assert(hop_clipboard_txn_clear_offer_if_match(&t, 11, 22, 77) == 2);
    assert(!t.remote_offer_origin && !t.remote_offer_generation);

    hop_clipboard_txn_set_remote_offer(&t, 22, 88);
    uint32_t id = 0, origin = 0;
    uint64_t generation = 0, expected = 0;
    assert(hop_clipboard_txn_begin_paste(&t, 123, 124, 0x80000000u,
        &id, &origin, &generation, &expected) == 0);
    assert(hop_clipboard_txn_begin_paste(&t, 124, 124, 0x80000000u,
        &id, &origin, &generation, &expected) == 1);
    assert(id == 0x80000001u && origin == 22 && generation == 88 && expected == 124);
    uint32_t same_id = 0, same_origin = 0;
    uint64_t same_expected = 0, same_generation = 0;
    assert(hop_clipboard_txn_begin_paste(&t, 124, 124, 0x80000000u,
        &same_id, &same_origin, &same_generation, &same_expected) == 2);
    assert(same_id == id && same_origin == origin && same_generation == generation && same_expected == expected);
    assert(!hop_clipboard_txn_complete_paste(&t, id + 1, 124, 1, NULL));
    assert(!hop_clipboard_txn_complete_paste(&t, id, 125, 1, NULL));
    assert(!t.pending_request_id);
    assert(hop_clipboard_txn_begin_paste(&t, 125, 125, 0x80000000u,
        &id, &origin, &generation, &expected) == 1);
    assert(hop_clipboard_txn_complete_paste(&t, id, 125, 1, &same_expected));
    assert(same_expected == 125 && t.pending_request_id == id);
    hop_clipboard_txn_cancel_pending_paste(&t, id + 1);
    assert(t.pending_request_id == id);
    hop_clipboard_txn_cancel_pending_paste(&t, id);
    assert(!t.pending_request_id);

    int snapshot_ok = 0;
    assert(hop_clipboard_txn_accept_request(&t, 900, local, 1, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_BUSY);
    assert(!t.paste_scan_active && !t.seen_next);
    snapshot_ok = 1;
    assert(hop_clipboard_txn_accept_request(&t, 900, local, 1, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_ACCEPTED);
    assert(hop_clipboard_txn_accept_request(&t, 900, local, 1, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_BUSY);
    hop_clipboard_txn_finish_paste_worker(&t);
    assert(hop_clipboard_txn_accept_request(&t, 900, local, 1, snapshot_paths, NULL) ==
           HOP_CLIPBOARD_REQUEST_DUPLICATE);
    assert(hop_clipboard_txn_accept_request(&t, 901, local + 1, 1, snapshot_paths, NULL) ==
           HOP_CLIPBOARD_REQUEST_STALE);
    assert(hop_clipboard_txn_accept_request(&t, 901, local, 0, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_BUSY);
    assert(hop_clipboard_txn_accept_request(&t, 901, local, 1, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_ACCEPTED);
    hop_clipboard_txn_finish_paste_worker(&t);
    hop_clipboard_txn_set_stopping(&t, 1);
    assert(hop_clipboard_txn_accept_request(&t, 902, local, 1, snapshot_paths, &snapshot_ok) ==
           HOP_CLIPBOARD_REQUEST_BUSY);
    assert(hop_clipboard_txn_begin_paste(&t, 125, 125, 0x80000000u,
        &id, &origin, &generation, &expected) == 0);
    puts("PASS: clipboard transaction lifecycle");
    return 0;
}
