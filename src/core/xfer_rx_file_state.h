#ifndef HOP_XFER_RX_FILE_STATE_H
#define HOP_XFER_RX_FILE_STATE_H

#include "hop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    int is_image;
    int is_directory;
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
void hop_xfer_rx_file_state_set_kind(hop_xfer_rx_file_state *state, int is_image, int is_directory);
void hop_xfer_rx_file_state_set_size(hop_xfer_rx_file_state *state, uint64_t size);
/* Shared FILE_BEGIN metadata admission: initializes the entry state and applies
 * the same relative-path safety policy on Windows and macOS. A rejected path
 * leaves the entry registered with HOP_FILE_ACK_BADPATH for its FILE_END ACK. */
int hop_xfer_rx_file_state_prepare_entry(hop_xfer_rx_file_state *state,
    uint32_t id, uint64_t size, int is_directory,
    const uint8_t *relative_path, uint32_t relative_path_length);
void hop_xfer_rx_file_state_fail(hop_xfer_rx_file_state *state, uint8_t ack_status);
int hop_xfer_rx_file_state_matches(const hop_xfer_rx_file_state *state, uint32_t id);
hop_xfer_rx_file_chunk_check hop_xfer_rx_file_state_check_chunk(
    const hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length);
/* Call only after the chunk has been accepted by the writer queue. */
int hop_xfer_rx_file_state_record_chunk(
    hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length,
    uint32_t chunk_crc);
typedef enum {
    HOP_RX_FILE_END_STORED_FAILURE = 0,
    HOP_RX_FILE_END_WRITER_ERROR,
    HOP_RX_FILE_END_PEER_REJECTED,
    HOP_RX_FILE_END_INCOMPLETE,
    HOP_RX_FILE_END_CRC_MISMATCH,
    HOP_RX_FILE_END_SUCCESS
} hop_xfer_rx_file_end_reason;

typedef struct {
    uint8_t ack_status;
    hop_xfer_rx_file_end_reason reason;
} hop_xfer_rx_file_end_result;

/* Shared FILE_END outcome: one Core decision supplies both wire ACK status
 * and a stable diagnostic reason to platform adapters. */
hop_xfer_rx_file_end_result hop_xfer_rx_file_state_evaluate_end(
    const hop_xfer_rx_file_state *state, uint8_t peer_status,
    int writer_error, uint32_t expected_file_crc);

/* Compatibility helper for callers that only need the wire ACK status. */
uint8_t hop_xfer_rx_file_state_ack(const hop_xfer_rx_file_state *state,
                                   uint8_t peer_status, int writer_error,
                                   uint32_t expected_file_crc);

#ifdef __cplusplus
}
#endif
#endif
