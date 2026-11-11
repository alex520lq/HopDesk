#ifndef HOP_XFER_RX_BATCH_STATE_H
#define HOP_XFER_RX_BATCH_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receiver-thread-owned active batch identity. The generation advances for
 * each accepted batch and is intentionally retained across reset/end so late
 * callbacks can distinguish a new batch from a reused wire batch number. */
typedef struct {
    uint32_t batch;
    uint32_t generation;
    int active;
} hop_xfer_rx_batch_state;

void hop_xfer_rx_batch_state_init(hop_xfer_rx_batch_state *state);
int hop_xfer_rx_batch_state_begin(hop_xfer_rx_batch_state *state, uint32_t batch);
int hop_xfer_rx_batch_state_end(hop_xfer_rx_batch_state *state, uint32_t batch);
void hop_xfer_rx_batch_state_reset(hop_xfer_rx_batch_state *state);
int hop_xfer_rx_batch_state_matches(const hop_xfer_rx_batch_state *state, uint32_t batch);

#ifdef __cplusplus
}
#endif
#endif
