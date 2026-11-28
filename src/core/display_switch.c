#include "display_switch.h"
#include "hop.h"

static int valid(const hop_display_msg *m) {
    if (!m || m->ok > 1) return 0;
    switch (m->op) {
    case HOP_DISPLAY_REQUEST: return m->id == 0 && m->value <= HOP_DISPLAY_AUTO && m->ok == 0;
    case HOP_DISPLAY_WAKE: return m->id != 0 && m->value >= 1 && m->value <= 3600 && m->ok == 0;
    case HOP_DISPLAY_INPUT: return m->id != 0 && m->value <= 65535 && m->ok == 0;
    case HOP_DISPLAY_LOCAL: return m->id == 0 && m->value <= HOP_DISPLAY_MAC && m->ok == 0;
    case HOP_DISPLAY_COMPLETE: return m->id == 0 && m->value <= HOP_DISPLAY_AUTO;
    case HOP_DISPLAY_RESULT: return m->id != 0 && m->value == 0;
    case HOP_DISPLAY_WAKE_CLEAR: return m->id == 0 && m->value == 0 && m->ok == 0;
    case HOP_DISPLAY_SLEEP: return m->id != 0 && m->value == 0 && m->ok == 0;
    default: return 0;
    }
}
int hop_display_encode(uint8_t out[HOP_DISPLAY_WIRE_SIZE], const hop_display_msg *m) {
    if (!out || !valid(m)) return 0;
    out[0] = m->op; hop_put_be32(out + 1, m->id);
    hop_put_be32(out + 5, m->value); out[9] = m->ok;
    return 1;
}
int hop_display_decode(hop_display_msg *m, const uint8_t *p, size_t len) {
    if (!m || !p || len != HOP_DISPLAY_WIRE_SIZE) return 0;
    hop_display_msg v = {p[0], p[9], hop_get_be32(p + 1), hop_get_be32(p + 5)};
    if (!valid(&v)) return 0;
    *m = v;
    return 1;
}
int hop_display_resolve_target(int requested, int attached, int current_input, uint16_t mac_input) {
    if (requested == HOP_DISPLAY_MAC) return HOP_DISPLAY_MAC;
    if (requested == HOP_DISPLAY_WINDOWS) return HOP_DISPLAY_WINDOWS;
    if (!attached) return HOP_DISPLAY_WINDOWS;
    if (current_input >= 0 &&
        ((uint16_t)current_input == mac_input ||
         (mac_input <= 0xFFu && ((uint16_t)current_input & 0xFFu) == mac_input)))
        return HOP_DISPLAY_WINDOWS;
    return HOP_DISPLAY_MAC;
}
static int rollback(const hop_display_ops *o, int failure) {
    int attached = o->attach(o->ctx, 1);
    int input = attached && o->local_input(o->ctx, HOP_DISPLAY_WINDOWS);
    return input ? failure : HOP_DISPLAY_ROLLBACK_FAILED;
}
int hop_display_switch_run(int target, const hop_display_ops *o) {
    if (!o || !o->attach || !o->local_input || !o->peer_input) return HOP_DISPLAY_INPUT_FAILED;
    if (target != HOP_DISPLAY_MAC && target != HOP_DISPLAY_WINDOWS) return HOP_DISPLAY_INPUT_FAILED;
    if (target == HOP_DISPLAY_MAC) {
        if (!o->wake_peer || !o->wake_peer(o->ctx)) return HOP_DISPLAY_WAKE_FAILED;
        if (!o->attach(o->ctx, 1)) return HOP_DISPLAY_ATTACH_FAILED;
        if (!o->local_input(o->ctx, HOP_DISPLAY_MAC) && !o->peer_input(o->ctx, HOP_DISPLAY_MAC))
            return rollback(o, HOP_DISPLAY_INPUT_FAILED);
        if (!o->attach(o->ctx, 0))
            return rollback(o, HOP_DISPLAY_DETACH_FAILED);
        return HOP_DISPLAY_OK;
    }
    if (!o->attach(o->ctx, 1)) {
        int nudged = o->peer_input(o->ctx, HOP_DISPLAY_WINDOWS) ||
                     (o->peer_sleep && o->peer_sleep(o->ctx));
        if (!nudged || !o->attach(o->ctx, 1)) return HOP_DISPLAY_ATTACH_FAILED;
    }
    if (!o->local_input(o->ctx, HOP_DISPLAY_WINDOWS) &&
        !o->peer_input(o->ctx, HOP_DISPLAY_WINDOWS) &&
        !(o->peer_sleep && o->peer_sleep(o->ctx)))
        return HOP_DISPLAY_INPUT_FAILED;
    if (o->clear_wake) o->clear_wake(o->ctx);
    return HOP_DISPLAY_OK;
}
