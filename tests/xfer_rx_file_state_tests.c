#include "xfer_rx_file_state.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    hop_xfer_rx_file_state state;
    hop_xfer_rx_file_state_reset(&state);
    assert(state.failure == -1 && state.id == 0);
    hop_xfer_rx_file_state_begin(&state, 9);
    hop_xfer_rx_file_state_set_size(&state, 9);
    assert(hop_xfer_rx_file_state_matches(&state, 9));
    assert(!hop_xfer_rx_file_state_matches(&state, 8));

    assert(hop_xfer_rx_file_state_check_chunk(&state, 0, 4) == HOP_RX_FILE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_file_state_record_chunk(&state, 0, 4, 0x12345678));
    uint32_t first_crc = hop_xfer_file_crc_step(0, 0, 0x12345678);
    assert(state.received == 4 && state.crc == first_crc);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 4, 5) == HOP_RX_FILE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 5, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 3, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_check_chunk(&state, UINT64_MAX, 1) == HOP_RX_FILE_CHUNK_INVALID);
    assert(hop_xfer_rx_file_state_record_chunk(&state, 4, 5, 0xabcdef01));
    assert(state.received == 9);
    assert(state.crc == hop_xfer_file_crc_step(first_crc, 4, 0xabcdef01));
    assert(hop_xfer_rx_file_state_check_chunk(&state, 9, 1) == HOP_RX_FILE_CHUNK_INVALID);

    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc) == HOP_FILE_ACK_OK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_IO_ERROR, 0, state.crc) == HOP_FILE_ACK_DISK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 1, state.crc) == HOP_FILE_ACK_DISK);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc + 1) == HOP_FILE_ACK_OK);
    /* An incomplete transfer is CRC failure even if the peer CRC happens to match. */
    hop_xfer_rx_file_state_set_size(&state, 10);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_OK, 0, state.crc) == HOP_FILE_ACK_CRC);
    hop_xfer_rx_file_state_fail(&state, HOP_FILE_ACK_BADPATH);
    assert(hop_xfer_rx_file_state_ack(&state, HOP_FILE_IO_ERROR, 1, 0) == HOP_FILE_ACK_BADPATH);
    assert(hop_xfer_rx_file_state_check_chunk(&state, 9, 0) == HOP_RX_FILE_CHUNK_ALREADY_FAILED);
    assert(!hop_xfer_rx_file_state_record_chunk(&state, 9, 0, 0));

    hop_xfer_rx_file_state_begin(&state, 10);
    assert(state.id == 10 && state.failure == -1 && state.received == 0 &&
           state.declared_size == 0 && state.crc == 0);
    puts("PASS: receiver file state contiguous offsets, CRC fold and failure gating");
    return 0;
}
