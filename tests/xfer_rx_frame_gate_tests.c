#include "xfer_rx_frame_gate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_file_begin(uint8_t *frame, const hop_session_id *session,
                           uint32_t batch, const uint8_t *body,
                           uint32_t body_length) {
    hop_data_prefix(frame, session, batch);
    hop_put_be32(frame + HOP_DATA_PREFIX, batch);
    memcpy(frame + HOP_DATA_PREFIX + 4, body, body_length);
}

int main(void) {
    hop_session_id session = {{1, 2, 3}};
    hop_session_id other = {{9, 8, 7}};
    char token[] = "shared-secret";
    hop_session_publish(&session);

    hop_rx_frame_gate_input input;
    input.expected_session = session;
    input.active_batch = 42;

    uint8_t hello[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    uint32_t hello_length = hop_xfer_hello_encode(hello, token, &session);
    assert(hello_length > 0);

    hop_rx_frame_gate_result result =
        hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO,
                               hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_ACCEPT);
    assert(!result.body && !result.body_length);

    result = hop_xfer_rx_gate_frame(&input, "wrong-token", HOP_MSG_XFER_HELLO,
                                    hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO,
                                    hello, hello_length - 1);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO,
                                    NULL, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);

    input.expected_session = other;
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO,
                                    hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);
    input.expected_session = session;

    uint8_t chunk[HOP_DATA_PREFIX + 8];
    hop_data_prefix(chunk, &session, 42);
    memset(chunk + HOP_DATA_PREFIX, 0x5a, 8);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DISPATCH);
    assert(result.batch == 42);
    assert(result.body == chunk + HOP_DATA_PREFIX);
    assert(result.body_length == 8);

    hop_data_prefix(chunk, &session, 41);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DROP);
    assert(!result.body && !result.body_length);

    input.active_batch = 0;
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_END,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_DROP);

    input.active_batch = 42;
    hop_data_prefix(chunk, &other, 42);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    hop_data_prefix(chunk, &session, 42);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK,
                                    chunk, HOP_DATA_PREFIX - 1);
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    uint8_t begin[HOP_DATA_PREFIX + 4 + 4];
    uint8_t body[4] = {1, 2, 3, 4};
    put_file_begin(begin, &session, 43, body, sizeof(body));
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_BEGIN,
                                    begin, sizeof(begin));
    assert(result.action == HOP_RX_FRAME_DISPATCH);
    assert(result.batch == 43);
    assert(result.body == begin + HOP_DATA_PREFIX);
    assert(result.body_length == 4 + sizeof(body));

    hop_data_prefix(begin, &session, 43);
    hop_put_be32(begin + HOP_DATA_PREFIX, 42);
    memcpy(begin + HOP_DATA_PREFIX + 4, body, sizeof(body));
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_BEGIN,
                                    begin, sizeof(begin));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    result = hop_xfer_rx_gate_frame(&input, token, 0xff, chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(NULL, token, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(&input, NULL, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);

    hop_session_clear(&session);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_FILE_CHUNK,
                                    chunk, sizeof(chunk));
    assert(result.action == HOP_RX_FRAME_REJECT_PEER);
    result = hop_xfer_rx_gate_frame(&input, token, HOP_MSG_XFER_HELLO,
                                    hello, hello_length);
    assert(result.action == HOP_RX_FRAME_HELLO_REJECT);

    puts("PASS: shared receiver frame gate validates HELLO, session binding and stale batch frames");
    return 0;
}
