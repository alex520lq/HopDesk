#ifndef HOP_XFER_RX_FRAME_GATE_H
#define HOP_XFER_RX_FRAME_GATE_H

#include <stdint.h>
#include "hop.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receiver-side transport admission shared by Windows and macOS. The platform
 * still owns sockets, locks and session snapshots; this module owns only the
 * protocol decision about HELLO, session binding and stale batch frames. */
typedef enum {
    HOP_RX_FRAME_DROP = 0,       /* stale or irrelevant; keep the connection */
    HOP_RX_FRAME_REJECT_PEER = 1, /* protocol/session violation; close transport */
    HOP_RX_FRAME_HELLO_ACCEPT = 2,
    HOP_RX_FRAME_HELLO_REJECT = 3,
    HOP_RX_FRAME_DISPATCH = 4
} hop_rx_frame_action;

typedef struct {
    hop_session_id expected_session;
    uint32_t active_batch; /* 0 when no receive batch is active */
} hop_rx_frame_gate_input;

typedef struct {
    hop_rx_frame_action action;
    uint32_t batch;
    const uint8_t *body; /* points into the caller's payload */
    uint32_t body_length;
} hop_rx_frame_gate_result;

/* HELLO must match the configured token and the current control session.
 * Every other frame must carry the current session. CHUNK/END are dropped
 * unless they belong to the active batch; BEGIN/DONE/ACK may start or finish
 * a batch and are therefore not filtered by the current batch id. */
hop_rx_frame_gate_result hop_xfer_rx_gate_frame(
    const hop_rx_frame_gate_input *input,
    const char *token,
    uint8_t type,
    const uint8_t *payload,
    uint32_t payload_length);

#ifdef __cplusplus
}
#endif
#endif
