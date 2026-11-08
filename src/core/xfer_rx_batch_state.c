#include "xfer_rx_batch_state.h"
#include <string.h>

void hop_xfer_rx_batch_state_init(hop_xfer_rx_batch_state *state) {
    if (state) memset(state, 0, sizeof(*state));
}

int hop_xfer_rx_batch_state_begin(hop_xfer_rx_batch_state *state, uint32_t batch) {
    if (!state || !batch || state->active) return 0;
    state->generation++;
    if (!state->generation) state->generation++;
    state->batch = batch;
    state->active = 1;
    return 1;
}

int hop_xfer_rx_batch_state_end(hop_xfer_rx_batch_state *state, uint32_t batch) {
    if (!state || !batch || !state->active || state->batch != batch) return 0;
    state->active = 0;
    state->batch = 0;
    return 1;
}

void hop_xfer_rx_batch_state_reset(hop_xfer_rx_batch_state *state) {
    if (!state) return;
    state->active = 0;
    state->batch = 0;
}

int hop_xfer_rx_batch_state_matches(const hop_xfer_rx_batch_state *state, uint32_t batch) {
    return state && batch != 0 && state->active && state->batch == batch;
}
