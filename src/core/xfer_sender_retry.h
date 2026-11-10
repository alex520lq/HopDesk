#ifndef HOP_XFER_SENDER_RETRY_H
#define HOP_XFER_SENDER_RETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOP_XFER_SENDER_RETRY_INVALID = -1,
    HOP_XFER_SENDER_RETRY_EXHAUSTED = 0,
    HOP_XFER_SENDER_RETRY_SCHEDULED = 1
} hop_xfer_sender_retry_result;

typedef struct {
    uint32_t attempt;
    uint64_t give_up_at_ms;
    uint64_t retry_at_ms;
} hop_xfer_sender_retry_plan;

/* Pure reconnect retry policy shared by Windows/macOS workers.
 * current_attempt is incremented for every link failure. A zero give-up
 * deadline is initialized from now_ms + reconnect_budget_ms. The retry is
 * scheduled only while attempt < max_attempts and now_ms < give_up_at_ms.
 * All additions saturate rather than wrapping. */
hop_xfer_sender_retry_result hop_xfer_sender_plan_retry(
    uint64_t now_ms,
    uint64_t current_give_up_at_ms,
    uint32_t current_attempt,
    uint32_t max_attempts,
    uint64_t reconnect_budget_ms,
    unsigned retry_delay_ms,
    hop_xfer_sender_retry_plan *out);

#ifdef __cplusplus
}
#endif
#endif
