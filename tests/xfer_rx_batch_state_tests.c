#include "xfer_rx_batch_state.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    hop_xfer_rx_batch_state state;
    hop_xfer_rx_batch_state_init(&state);
    assert(!state.active && !state.batch && !state.generation);
    assert(!hop_xfer_rx_batch_state_begin(&state, 0));
    assert(hop_xfer_rx_batch_state_begin(&state, 17));
    assert(state.active && state.batch == 17 && state.generation == 1);
    assert(hop_xfer_rx_batch_state_matches(&state, 17));
    assert(!hop_xfer_rx_batch_state_matches(&state, 18));
    assert(!hop_xfer_rx_batch_state_begin(&state, 18));
    assert(!hop_xfer_rx_batch_state_end(&state, 18));
    assert(hop_xfer_rx_batch_state_end(&state, 17));
    assert(!state.active && !state.batch && state.generation == 1);
    assert(!hop_xfer_rx_batch_state_matches(&state, 17));
    assert(hop_xfer_rx_batch_state_begin(&state, 17));
    assert(state.generation == 2);
    hop_xfer_rx_batch_state_reset(&state);
    assert(!state.active && !state.batch && state.generation == 2);
    puts("PASS: receiver batch identity lifecycle and generation");
    return 0;
}
