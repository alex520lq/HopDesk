#include "xfer_rx_lifecycle.h"
#include <string.h>

void hop_xfer_rx_lifecycle_init(hop_xfer_rx_lifecycle *lifecycle) {
    if (lifecycle) memset(lifecycle, 0, sizeof(*lifecycle));
}

const hop_session_id *hop_xfer_rx_lifecycle_session(
    const hop_xfer_rx_lifecycle *lifecycle) {
    return lifecycle ? &lifecycle->session : NULL;
}

int hop_xfer_rx_lifecycle_bind_session(
    hop_xfer_rx_lifecycle *lifecycle, const hop_session_id *session) {
    if (!lifecycle || !session) return 0;
    if (hop_session_id_equal(&lifecycle->session, session)) return 0;
    lifecycle->session = *session;
    lifecycle->dead_batch = 0;
    lifecycle->dead_reason = 0;
    return 1;
}

void hop_xfer_rx_lifecycle_mark_dead(
    hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t reason) {
    if (!lifecycle || !batch) return;
    lifecycle->dead_batch = batch;
    lifecycle->dead_reason = reason;
}

int hop_xfer_rx_lifecycle_dead_reason(
    const hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t *reason) {
    if (!lifecycle || !batch || lifecycle->dead_batch != batch) return 0;
    if (reason) *reason = lifecycle->dead_reason;
    return 1;
}
