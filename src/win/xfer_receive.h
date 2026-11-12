#ifndef HOP_WIN_XFER_RECEIVE_H
#define HOP_WIN_XFER_RECEIVE_H

#include <windows.h>

#include "../core/xfer_rx_frame_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receiver-owned lifecycle boundaries. xfer.c starts/stops the writer queue
 * and data-channel thread, then uses the audit predicate before destroying
 * shared synchronization. Receiver context and wire dispatch stay private. */
int xfer_receive_start(void);
int xfer_receive_stop(void);
int xfer_receive_owner_clean(void);

/* Connection-owner callbacks; no sender state or native socket is exposed. */
void xfer_receive_bind_session(const hop_session_id *session);
void xfer_receive_poll_cancel(int connected);
void xfer_receive_disconnected(void);
hop_rx_frame_action xfer_receive_process_frame(const hop_session_id *session,
    const char *token, int authenticated, uint8_t type,
    const uint8_t *payload, uint32_t length);
void xfer_receive_cancel_request(void);

void xfer_data_connection_shutdown(void);
void xfer_data_send_abort(uint32_t batch, uint8_t reason);
void xfer_data_send_file_ack(uint32_t batch, uint32_t id, int status);
void xfer_data_send_done_ack(uint32_t batch, const uint8_t *payload, uint32_t length);
void xfer_data_record_peer_abort(uint32_t batch, uint8_t reason);

/* Narrow Receiver reporting/ACK services shared with the sender owner. */
uint64_t rate_now_ns(void);
void ack_deliver(uint32_t batch, uint32_t id, int status);
int status_begin(uint32_t batch, int direction, uint32_t files, uint64_t bytes, const char *name, int phase);
void status_end(int direction, uint32_t batch, int phase, const char *detail);
void status_add_received_file(uint32_t batch, uint32_t id, const char *path, uint64_t size, int directory);
void status_image(uint32_t batch, int direction, uint64_t size);
void status_stage_time(int direction, uint32_t batch, int stage, uint64_t elapsed_ns);
void status_chunk_timed(int direction, uint32_t batch, uint32_t id, uint64_t offset, uint32_t length, uint64_t read_ns, uint64_t crc_ns, uint64_t send_ns, uint64_t receive_ns);
void status_file_phase(int direction, uint32_t batch, int index, int phase, uint32_t attempts);
void status_receive_set_total(uint32_t total);
void status_receive_reset(void);

#ifdef __cplusplus
}
#endif
#endif
