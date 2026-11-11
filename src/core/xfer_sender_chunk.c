#include "xfer_sender_chunk.h"

#include "hop.h"
#include "xfer_tracking.h"
#include "xfer_sender_context_adapter.h"

#include <string.h>

#define SENDER_CHUNK_HEADER_BYTES (HOP_HEADER_SIZE + HOP_DATA_PREFIX + 20u)

static int sender_chunk_wait_rate(const hop_xfer_sender_chunk_ops *ops,
                                  uint32_t bytes) {
    const hop_xfer_sender_context *context = ops->context;
    if (!hop_xfer_sender_context_clock_valid(context)) return 0;
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(
            ops->rate, bytes, context->now_ns(context->user));
        if (delay == UINT64_MAX) {
            hop_xfer_sender_context_logf(context, HOP_LOG_ERROR,
                "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return !context->is_stopping(context->user);
        if (context->is_stopping(context->user)) return 0;
        if (!context->wait_ns(context->user, delay)) return 0;
    }
}

int hop_xfer_sender_send_chunk(const hop_xfer_sender_chunk_ops *ops,
                               uint32_t batch, uint32_t id, uint64_t offset,
                               const uint8_t *data, uint32_t length,
                               uint32_t crc, uint64_t read_ns,
                               uint64_t crc_ns) {
    if (!ops || !ops->context || !ops->rate || !ops->tx_buffer ||
        ops->tx_capacity < SENDER_CHUNK_HEADER_BYTES ||
        !ops->context->snapshot_session || !ops->context->write_frame ||
        (!data && length) ||
        length > UINT32_MAX - 20u - HOP_DATA_PREFIX)
        return 0;

    const hop_xfer_sender_context *context = ops->context;
    uint8_t *tx = ops->tx_buffer;
    if (!sender_chunk_wait_rate(ops, length)) {
        hop_xfer_sender_context_status_stage_time(context, batch,
                                                   HOP_XFER_TIME_READ, read_ns);
        hop_xfer_sender_context_status_stage_time(context, batch,
                                                   HOP_XFER_TIME_CRC, crc_ns);
        return 0;
    }
    if (data == tx + SENDER_CHUNK_HEADER_BYTES &&
        length > ops->tx_capacity - SENDER_CHUNK_HEADER_BYTES) {
        hop_xfer_sender_context_status_stage_time(context, batch,
                                                   HOP_XFER_TIME_READ, read_ns);
        hop_xfer_sender_context_status_stage_time(context, batch,
                                                   HOP_XFER_TIME_CRC, crc_ns);
        return 0;
    }

    int send_guard = hop_watchdog_begin("data frame send", 10000);
    uint8_t session_bytes[HOP_TASK_SESSION_BYTES];
    memset(session_bytes, 0, sizeof(session_bytes));
    uint64_t epoch = 0;
    int current = context->snapshot_session(context->user, ops->socket_handle,
                                             session_bytes, &epoch);
    hop_session_id session;
    memset(&session, 0, sizeof(session));
    memcpy(session.bytes, session_bytes, HOP_SESSION_BYTES);
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK,
                      20u + HOP_DATA_PREFIX + length);
    hop_data_prefix(tx + HOP_HEADER_SIZE, &session, batch);
    hop_put_be32(tx + HOP_HEADER_SIZE + HOP_DATA_PREFIX, id);
    hop_put_be64(tx + HOP_HEADER_SIZE + HOP_DATA_PREFIX + 4u, offset);
    hop_put_be32(tx + HOP_HEADER_SIZE + HOP_DATA_PREFIX + 12u, length);
    hop_put_be32(tx + HOP_HEADER_SIZE + HOP_DATA_PREFIX + 16u, crc);

    hop_xfer_sender_frame_part parts[2];
    size_t part_count = 1;
    parts[0] = (hop_xfer_sender_frame_part){tx, SENDER_CHUNK_HEADER_BYTES};
    if (data == tx + SENDER_CHUNK_HEADER_BYTES) {
        parts[0].size += length;
    } else {
        parts[1] = (hop_xfer_sender_frame_part){data, length};
        part_count = 2;
    }

    uint64_t send_started = context->now_ns(context->user);
    int ok = current && context->write_frame(context->user,
        ops->socket_handle, epoch, parts, part_count);
    uint64_t send_finished = context->now_ns(context->user);
    uint64_t send_us = send_finished >= send_started
        ? (send_finished - send_started) / 1000ull : 0;
    hop_xfer_rate_note_send_time(ops->rate, length, send_us);
    if ((ops->rate->send_samples & 7u) == 0 && ops->rate_feedback)
        ops->rate_feedback(0);
    uint64_t send_ns = send_us * 1000ull;
    if (ok) {
        hop_xfer_sender_context_status_chunk_timed(context, batch, id, offset,
            length, read_ns, crc_ns, send_ns, 0);
    } else {
        hop_xfer_sender_context_status_stage_time(context, batch,
            HOP_XFER_TIME_READ, read_ns);
        hop_xfer_sender_context_status_stage_time(context, batch,
            HOP_XFER_TIME_CRC, crc_ns);
        hop_xfer_sender_context_status_stage_time(context, batch,
            HOP_XFER_TIME_SEND, send_ns);
    }
    hop_watchdog_end(send_guard);
    return ok;
}
