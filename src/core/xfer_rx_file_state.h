#ifndef HOP_XFER_RX_FILE_STATE_H
#define HOP_XFER_RX_FILE_STATE_H

#include "hop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    int failure;              /* -1 while valid; otherwise FILE_ACK status */
    uint64_t declared_size;
    uint64_t received;
    uint32_t crc;
} hop_xfer_rx_file_state;

typedef enum {
    HOP_RX_FILE_CHUNK_INVALID = 0,
    HOP_RX_FILE_CHUNK_ACCEPT,
    HOP_RX_FILE_CHUNK_ALREADY_FAILED
} hop_xfer_rx_file_chunk_check;

void hop_xfer_rx_file_state_reset(hop_xfer_rx_file_state *state);
void hop_xfer_rx_file_state_begin(hop_xfer_rx_file_state *state, uint32_t id);
void hop_xfer_rx_file_state_set_size(hop_xfer_rx_file_state *state, uint64_t size);
void hop_xfer_rx_file_state_fail(hop_xfer_rx_file_state *state, uint8_t ack_status);
int hop_xfer_rx_file_state_matches(const hop_xfer_rx_file_state *state, uint32_t id);
hop_xfer_rx_file_chunk_check hop_xfer_rx_file_state_check_chunk(
    const hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length);
/* Call only after the chunk has been accepted by the writer queue. */
int hop_xfer_rx_file_state_record_chunk(
    hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length,
    uint32_t chunk_crc);

#ifdef __cplusplus
}
#endif
#endif
