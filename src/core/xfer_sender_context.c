#include "xfer_sender_context.h"

#include "hop.h"
#include "xfer_tracking.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Context Forwarding Helpers ---- */

int hop_xfer_sender_context_task_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch,
    const hop_xfer_task *task) {
    if (!context || !context->is_cancelled || !task || !task->id) return 0;
    return context->is_cancelled(context->user, batch, task->id);
}

int hop_xfer_sender_context_job_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch, uint64_t task_id) {
    if (!context) return 0;
    int batch_cancelled = context->is_batch_cancelled &&
        context->is_batch_cancelled(context->user, batch);
    int task_cancelled = task_id && context->is_cancelled &&
        context->is_cancelled(context->user, batch, task_id);
    return batch_cancelled || task_cancelled;
}

int hop_xfer_sender_context_task_bind_session(
    const hop_xfer_sender_context *context, const hop_xfer_task *task,
    const uint8_t session[HOP_TASK_SESSION_BYTES]) {
    return context && context->task_bind_session && task && session
        ? context->task_bind_session(context->user, task, session) : 0;
}

int hop_xfer_sender_context_task_set_state(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state) {
    return context && context->task_set_state && task
        ? context->task_set_state(context->user, task, (int)state) : 0;
}

int hop_xfer_sender_context_task_begin_completion(
    const hop_xfer_sender_context *context, const hop_xfer_task *task) {
    return context && context->task_begin_completion && task
        ? context->task_begin_completion(context->user, task) : 0;
}

void hop_xfer_sender_context_task_finish(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state) {
    if (context && context->task_finish && task)
        context->task_finish(context->user, task, (int)state);
}

int hop_xfer_sender_context_task_dispose(
    const hop_xfer_sender_context *context, hop_xfer_task *task) {
    if (!task || !context || !context->task_dispose) return 0;
    context->task_dispose(context->user, task);
    return !task->initialized && !task->id;
}

void hop_xfer_sender_context_status_start_job(
    const hop_xfer_sender_context *context, const void *job) {
    if (context && context->status_start_job)
        context->status_start_job(context->user, job);
}

void hop_xfer_sender_context_status_end(
    const hop_xfer_sender_context *context, uint32_t batch, int phase,
    const char *detail) {
    if (context && context->status_end)
        context->status_end(context->user, batch, phase, detail);
}

void hop_xfer_sender_context_status_file_phase(
    const hop_xfer_sender_context *context, uint32_t batch, int index,
    int phase, uint32_t attempts) {
    if (context && context->status_file_phase)
        context->status_file_phase(context->user, batch, index, phase, attempts);
}

void hop_xfer_sender_context_status_stage_time(
    const hop_xfer_sender_context *context, uint32_t batch, int stage,
    uint64_t elapsed_ns) {
    if (context && context->status_stage_time)
        context->status_stage_time(context->user, batch, stage, elapsed_ns);
}

void hop_xfer_sender_context_status_chunk_timed(
    const hop_xfer_sender_context *context, uint32_t batch, uint32_t id,
    uint64_t offset, uint32_t length, uint64_t read_ns, uint64_t crc_ns,
    uint64_t send_ns, uint64_t receive_ns) {
    if (context && context->status_chunk_timed)
        context->status_chunk_timed(context->user, batch, id, offset, length,
                                     read_ns, crc_ns, send_ns, receive_ns);
}

void hop_xfer_sender_context_logf(
    const hop_xfer_sender_context *context, int level,
    const char *format, ...) {
    if (!context || !context->log_message || !format) return;
    char message[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    context->log_message(context->user, level, message);
}

/* ---- Chunk Pacing & Framing ---- */

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
