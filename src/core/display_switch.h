#ifndef HOP_DISPLAY_SWITCH_H
#define HOP_DISPLAY_SWITCH_H
#include <stdint.h>
#include <stddef.h>
/* Windows owns the shared monitor topology; either UI can request a target. */
enum { HOP_DISPLAY_WINDOWS, HOP_DISPLAY_MAC, HOP_DISPLAY_AUTO };
enum {
    HOP_DISPLAY_REQUEST = 1,
    HOP_DISPLAY_WAKE,
    HOP_DISPLAY_INPUT,
    HOP_DISPLAY_RESULT,
    HOP_DISPLAY_COMPLETE,
    HOP_DISPLAY_LOCAL,
    HOP_DISPLAY_WAKE_CLEAR,
    HOP_DISPLAY_SLEEP
};
#define HOP_DISPLAY_WIRE_SIZE 10

typedef struct { uint8_t op, ok; uint32_t id, value; } hop_display_msg;
int hop_display_encode(uint8_t out[HOP_DISPLAY_WIRE_SIZE], const hop_display_msg *msg);
int hop_display_decode(hop_display_msg *msg, const uint8_t *p, size_t len);

/* Resolves HOP_DISPLAY_AUTO from live Windows topology + VCP 0x60 input code
 * (current_input < 0 if unreadable). Explicit MAC/WINDOWS pass through. */
int hop_display_resolve_target(int requested, int attached, int current_input, uint16_t mac_input);

typedef struct {
    void *ctx;
    int (*wake_peer)(void *ctx);
    void (*clear_wake)(void *ctx); /* optional: release peer keep-awake after reclaiming Windows */
    /* Input callbacks return success only after VCP readback, not queue admission. */
    int (*local_input)(void *ctx, int target);
    int (*peer_input)(void *ctx, int target);
    int (*peer_sleep)(void *ctx); /* optional fallback for Windows target when enabled */
    int (*attach)(void *ctx, int attached); /* includes fresh topology verification */
} hop_display_ops;
enum { HOP_DISPLAY_OK, HOP_DISPLAY_WAKE_FAILED, HOP_DISPLAY_ATTACH_FAILED,
       HOP_DISPLAY_INPUT_FAILED, HOP_DISPLAY_DETACH_FAILED, HOP_DISPLAY_ROLLBACK_FAILED };
/* Synchronous policy, run by a platform worker. Owns no OS objects or locks. */
int hop_display_switch_run(int target, const hop_display_ops *ops);
#endif
