#include "xfer_sender_retry.h"

#include <stddef.h>

static uint64_t retry_add_saturating(uint64_t value, uint64_t amount) {
    return amount > UINT64_MAX - value ? UINT64_MAX : value + amount;
}

hop_xfer_sender_retry_result hop_xfer_sender_plan_retry(
    uint64_t now_ms,
    uint64_t current_give_up_at_ms,
    uint32_t current_attempt,
    uint32_t max_attempts,
    uint64_t reconnect_budget_ms,
    unsigned retry_delay_ms,
    hop_xfer_sender_retry_plan *out) {
    if (!out || !max_attempts) return HOP_XFER_SENDER_RETRY_INVALID;

    out->attempt = current_attempt == UINT32_MAX
        ? UINT32_MAX : current_attempt + 1u;
    out->give_up_at_ms = current_give_up_at_ms
        ? current_give_up_at_ms
        : retry_add_saturating(now_ms, reconnect_budget_ms);
    out->retry_at_ms = 0;

    if (out->attempt < max_attempts && now_ms < out->give_up_at_ms) {
        out->retry_at_ms = retry_add_saturating(now_ms, retry_delay_ms);
        return HOP_XFER_SENDER_RETRY_SCHEDULED;
    }
    return HOP_XFER_SENDER_RETRY_EXHAUSTED;
}
