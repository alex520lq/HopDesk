#include "xfer_rx_frame_gate.h"

static hop_rx_frame_gate_result result(hop_rx_frame_action action) {
    hop_rx_frame_gate_result out;
    out.action = action;
    out.batch = 0;
    out.body = NULL;
    out.body_length = 0;
    return out;
}

hop_rx_frame_gate_result hop_xfer_rx_gate_frame(
    const hop_rx_frame_gate_input *input,
    const char *token,
    uint8_t type,
    const uint8_t *payload,
    uint32_t payload_length) {
    if (!input || !token) return result(HOP_RX_FRAME_REJECT_PEER);

    if (type == HOP_MSG_XFER_HELLO) {
        if (!hop_session_matches(&input->expected_session) ||
            !hop_xfer_hello_bound(payload, payload_length, token,
                                  &input->expected_session))
            return result(HOP_RX_FRAME_HELLO_REJECT);
        return result(HOP_RX_FRAME_HELLO_ACCEPT);
    }

    if (!hop_session_matches(&input->expected_session))
        return result(HOP_RX_FRAME_REJECT_PEER);

    uint32_t batch = 0, body_length = 0;
    const uint8_t *body = NULL;
    if (!hop_data_unwrap(type, payload, payload_length, &input->expected_session,
                         &batch, &body, &body_length))
        return result(HOP_RX_FRAME_REJECT_PEER);

    if ((type == HOP_MSG_FILE_CHUNK || type == HOP_MSG_FILE_END) &&
        (!input->active_batch || batch != input->active_batch))
        return result(HOP_RX_FRAME_DROP);

    hop_rx_frame_gate_result out = result(HOP_RX_FRAME_DISPATCH);
    out.batch = batch;
    out.body = body;
    out.body_length = body_length;
    return out;
}
