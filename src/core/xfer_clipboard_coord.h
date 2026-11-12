#ifndef HOP_XFER_CLIPBOARD_COORD_H
#define HOP_XFER_CLIPBOARD_COORD_H

#include <stdint.h>

#include "hop.h"
#include "xfer_clipboard_txn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clipboard orchestration decisions.
 *
 * Every entry point is a pure decision: it mutates the shared transaction and
 * returns the action the platform adapter must perform. The adapter owns the
 * platform lock and must call these with that lock held; actions that touch
 * native state are executed by the adapter, and wire sends happen after the
 * adapter releases the lock. No OS type, thread primitive, socket or native
 * path representation appears in this contract.
 *
 * The point is not to wrap platform calls one to one. It is to keep the
 * decision rules (identity checks, admission, ordering, terminal reason) in a
 * single implementation so Windows and macOS cannot drift apart. */

typedef enum {
    HOP_CLIP_ACT_NONE = 0,           /* nothing to do */
    HOP_CLIP_ACT_SEND_PASTE_FAILED,  /* send PASTE_FAILED using fail_reason */
    HOP_CLIP_ACT_SPAWN_PASTE_WORKER, /* accepted: start the paste worker */
    HOP_CLIP_ACT_SEND_PASTE_REQUEST, /* encode and send the pending request */
    HOP_CLIP_ACT_RESEND_PENDING,     /* request already pending: report success, send nothing */
    HOP_CLIP_ACT_CLEAR_LOCAL_OFFER,  /* drop the adapter path array; notify if cleared_generation */
    HOP_CLIP_ACT_APPLY_PASTE,        /* staged: apply at expected_version */
    HOP_CLIP_ACT_CANCEL_PENDING      /* drop the pending paste without applying */
} hop_clip_action;

typedef struct {
    hop_clip_action action;
    uint8_t fail_reason;       /* HOP_CLIP_ACT_SEND_PASTE_FAILED */
    uint32_t request_id;       /* paste request identity */
    uint32_t origin;
    uint64_t generation;
    uint64_t expected_version; /* HOP_CLIP_ACT_APPLY_PASTE / SEND_PASTE_REQUEST */
    uint64_t cleared_generation; /* CLEAR_LOCAL_OFFER: previous local generation, 0 = no notify */
    int notify_peer;
} hop_clip_outcome;

/* Inbound FILE_OFFER. Rejects self-originated and post-stop offers, then makes
 * the peer offer current and retires the local one. */
hop_clip_outcome hop_clip_coord_remote_offer(hop_clipboard_txn *txn,
    uint32_t local_device, uint32_t origin, uint64_t generation);

/* Inbound FILE_OFFER_CLEAR. Clears whichever identity actually matches; only a
 * local match requires the adapter to drop its path array. */
hop_clip_outcome hop_clip_coord_offer_clear(hop_clipboard_txn *txn,
    uint32_t local_device, uint32_t origin, uint64_t generation);

/* Inbound PASTE_REQUEST. Owns the origin check, admission and the mapping from
 * admission result to terminal failure reason. */
hop_clip_outcome hop_clip_coord_paste_request(hop_clipboard_txn *txn,
    uint32_t request_id, uint32_t origin, uint64_t generation,
    uint32_t local_device, int worker_available,
    int (*snapshot)(void *), void *snapshot_ctx);

/* Outbound PASTE_REQUEST. Owns the tri-state begin result so both platforms
 * report the same value for "no offer", "already pending" and "new request". */
hop_clip_outcome hop_clip_coord_begin_paste(hop_clipboard_txn *txn,
    uint64_t current_version, uint64_t observed_version, uint32_t request_flag);

/* Completion of an inbound transfer that backs a local paste. current_version
 * is the platform clipboard generation sampled at completion time. */
hop_clip_outcome hop_clip_coord_complete_paste(hop_clipboard_txn *txn,
    uint32_t request_id, uint64_t current_version, int success);

/* Who must be told to drop an identity once the local clipboard is rewritten.
 * This is protocol semantics, not a platform choice, so Core decides it: when
 * we consume the peer's offer and write its content ourselves, the peer still
 * owns that offer and must not be told to drop it. */
typedef enum {
    HOP_CLIP_NOTIFY_NONE = 0,    /* nothing to send */
    HOP_CLIP_NOTIFY_LOCAL_ONLY,  /* our local generation is dead, the peer's offer is not */
    HOP_CLIP_NOTIFY_BOTH         /* both identities are dead */
} hop_clip_notify;

/* Identities retired in one critical section, plus what the peer must be told.
 * The adapter owns the wire sends; this only decides what is retired. */
typedef struct {
    hop_clipboard_offer_key remote;
    uint64_t local_generation;
    hop_clip_notify notify;
} hop_clip_retired_offers;

/* Under the adapter lock: the clipboard changed under us, so both identities are
 * dead and the peer must drop both. Both platforms used to run this as two
 * separately locked steps, which left a window where the peer identity was gone
 * but the local one was still live. */
hop_clip_retired_offers hop_clip_coord_retire_offers(hop_clipboard_txn *txn);

/* Under the adapter lock: we are writing the clipboard ourselves (applying a
 * remote paste, or a publish that cannot be described). Both identities are
 * retired, but only our local one is announced: the peer still owns its offer. */
hop_clip_retired_offers hop_clip_coord_consume_offers(hop_clipboard_txn *txn);

/* Under the adapter lock: retire both identities, then publish a new local
 * generation. The adapter installs its native paths before releasing the lock. */
hop_clip_retired_offers hop_clip_coord_publish_local_offer(hop_clipboard_txn *txn,
    uint64_t *new_generation);

#ifdef __cplusplus
}
#endif
#endif
