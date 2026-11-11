#include "xfer_sender_engine_adapter.h"

#include "hop.h"
#include "xfer_sender_ack_wait.h"
#include "xfer_sender_context_adapter.h"

#include <string.h>

typedef struct {
    const hop_xfer_sender_engine_adapter_ops *ops;
} sender_file_adapter_call;

static int adapter_cancel_requested(void *user, uint32_t batch) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && hop_xfer_sender_context_job_cancel_requested(
        ops->context, batch, ops->task_id);
}

static int adapter_peer_aborted(void *user, uint32_t batch) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && ops->context && ops->context->peer_aborted
        ? ops->context->peer_aborted(ops->context->user, batch) : 0;
}

static const void *adapter_entry_at(void *user, int index) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && ops->entry_at ? ops->entry_at(ops->user, index) : NULL;
}

static const char *adapter_entry_path(void *user, const void *entry) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && ops->entry_path ? ops->entry_path(ops->user, entry) : NULL;
}

static int file_cancel_requested(void *user, uint32_t batch) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call ? adapter_cancel_requested((void *)call->ops, batch) : 0;
}

static int file_peer_aborted(void *user, uint32_t batch) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call ? adapter_peer_aborted((void *)call->ops, batch) : 0;
}

static int file_open(void *user, const void *entry, intptr_t *handle,
                     uint32_t *system_error) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call && call->ops->open_file
        ? call->ops->open_file(call->ops->user, entry, handle, system_error) : 0;
}

static hop_xfer_sender_read_result file_read(
    void *user, intptr_t handle, uint8_t *buffer, uint32_t request,
    uint32_t *bytes_read, uint32_t *system_error) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call && call->ops->read_file
        ? call->ops->read_file(call->ops->user, handle, buffer, request,
                               bytes_read, system_error)
        : HOP_XFER_SENDER_READ_ERROR;
}

static void file_close(void *user, intptr_t handle) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    if (call && call->ops->close_file)
        call->ops->close_file(call->ops->user, handle);
}

static int file_send_message(void *user, uint8_t type,
                             const uint8_t *payload, uint32_t length,
                             uint32_t batch) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call && call->ops->send_message
        ? call->ops->send_message(call->ops->user,
            call->ops->socket_handle, type, payload, length, batch) : 0;
}

static int file_send_chunk(void *user, uint32_t batch, uint32_t id,
                           uint64_t offset, const uint8_t *data,
                           uint32_t length, uint32_t crc,
                           uint64_t read_ns, uint64_t crc_ns) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    return call && call->ops->send_chunk
        ? call->ops->send_chunk(call->ops->user,
            call->ops->socket_handle, call->ops->tx_buffer, batch, id,
            offset, data, length, crc, read_ns, crc_ns) : 0;
}

static int file_ack_register(void *user, uint32_t batch, uint32_t id) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    const hop_xfer_sender_context *ctx = call ? call->ops->context : NULL;
    return ctx && ctx->ack_user && ctx->ack_register
        ? ctx->ack_register(ctx->ack_user, batch, id) : 0;
}

static int file_ack_mark_sent(void *user, uint32_t batch, uint32_t id) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    const hop_xfer_sender_context *ctx = call ? call->ops->context : NULL;
    return ctx && ctx->ack_user && ctx->ack_mark_sent
        ? ctx->ack_mark_sent(ctx->ack_user, batch, id) : 0;
}

static int file_ack_set_deadline(void *user, uint32_t batch, uint32_t id,
                                 uint64_t deadline_ns) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    const hop_xfer_sender_context *ctx = call ? call->ops->context : NULL;
    return ctx && ctx->ack_user && ctx->ack_set_deadline
        ? ctx->ack_set_deadline(ctx->ack_user, batch, id, deadline_ns) : 0;
}

static void file_ack_unregister(void *user, uint32_t batch, uint32_t id) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    const hop_xfer_sender_context *ctx = call ? call->ops->context : NULL;
    if (ctx && ctx->ack_user && ctx->ack_unregister)
        ctx->ack_unregister(ctx->ack_user, batch, id);
}

static uint64_t file_now_ns(void *user) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    const hop_xfer_sender_context *ctx = call ? call->ops->context : NULL;
    return ctx && ctx->now_ns ? ctx->now_ns(ctx->user) : 0;
}

static void file_status_stage_time(void *user, uint32_t batch,
                                   int stage, uint64_t elapsed_ns) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    hop_xfer_sender_context_status_stage_time(
        call ? call->ops->context : NULL, batch, stage, elapsed_ns);
}

static void file_yield_cpu(void *user) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    if (call && call->ops->yield_cpu)
        call->ops->yield_cpu(call->ops->user);
}

static void file_log_message(void *user, int level, const char *message) {
    const sender_file_adapter_call *call =
        (const sender_file_adapter_call *)user;
    hop_xfer_sender_context_logf(call ? call->ops->context : NULL,
                                  level, "%s", message ? message : "");
}

static hop_xfer_sender_send_result adapter_send_entry(
    void *user, uint32_t batch, uint32_t id, const void *platform_entry,
    uint64_t *ack_deadline_ns) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (!ops || !platform_entry || !ack_deadline_ns) 
        return HOP_XFER_SENDER_SEND_RETRYABLE;

    hop_xfer_sender_file_entry entry;
    entry.platform_entry = platform_entry;
    entry.name = ops->entry_name(ops->user, platform_entry);
    entry.relative_path = ops->entry_path(ops->user, platform_entry);
    entry.size = ops->entry_size(ops->user, platform_entry);
    entry.mtime = ops->entry_mtime(ops->user, platform_entry);
    entry.flags = ops->entry_flags(ops->user, platform_entry);
    if (!entry.name || !entry.relative_path)
        return HOP_XFER_SENDER_SEND_RETRYABLE;

    sender_file_adapter_call call = {ops};
    hop_xfer_sender_file_ops file_ops;
    memset(&file_ops, 0, sizeof(file_ops));
    file_ops.user = &call;
    file_ops.cancel_requested = file_cancel_requested;
    file_ops.peer_aborted = file_peer_aborted;
    file_ops.open_file = file_open;
    file_ops.read_file = file_read;
    file_ops.close_file = file_close;
    file_ops.send_message = file_send_message;
    file_ops.send_chunk = file_send_chunk;
    file_ops.ack_register = file_ack_register;
    file_ops.ack_mark_sent = file_ack_mark_sent;
    file_ops.ack_set_deadline = file_ack_set_deadline;
    file_ops.ack_unregister = file_ack_unregister;
    file_ops.now_ns = file_now_ns;
    file_ops.status_stage_time = file_status_stage_time;
    file_ops.yield_cpu = file_yield_cpu;
    file_ops.log_message = file_log_message;

    uint8_t *buffer = ops->tx_buffer + ops->chunk_header_size;
    return hop_xfer_sender_send_file_entry(
        &file_ops, batch, id, &entry, buffer, ops->chunk_size,
        buffer, ops->chunk_size, ops->ack_timeout_ns,
        ops->directory_flag, ops->file_ok_status, ops->file_io_error_status,
        ops->file_begin_message, ops->file_end_message, ack_deadline_ns);
}

static hop_xfer_sender_ack_result adapter_wait_ack(
    void *user, uint32_t batch, uint32_t id, uint64_t deadline_ns,
    uint64_t *rtt_us) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (!ops) return HOP_XFER_SENDER_ACK_LOCAL_FAILURE;
    int result = hop_xfer_sender_ack_wait(
        ops->context, ops->socket_handle, ops->connection_epoch, batch,
        ops->task_id, id, deadline_ns, ops->ack_poll_interval_ms, rtt_us);
    if (result == HOP_FILE_ACK_OK) return HOP_XFER_SENDER_ACK_OK;
    if (result == HOP_FILE_ACK_BADPATH) return HOP_XFER_SENDER_ACK_BADPATH;
    if (result == HOP_XFER_SENDER_ACK_WAIT_TIMEOUT) return HOP_XFER_SENDER_ACK_TIMEOUT;
    if (result == HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD) return HOP_XFER_SENDER_ACK_SEND_DEAD;
    if (result == HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT) return HOP_XFER_SENDER_ACK_PEER_ABORT;
    if (result == HOP_XFER_SENDER_ACK_WAIT_CANCELLED) return HOP_XFER_SENDER_ACK_CANCELLED;
    return result >= 0 ? HOP_XFER_SENDER_ACK_OTHER_REMOTE
                       : HOP_XFER_SENDER_ACK_LOCAL_FAILURE;
}

static void adapter_unregister_batch(void *user, uint32_t batch) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    const hop_xfer_sender_context *ctx = ops ? ops->context : NULL;
    if (ctx && ctx->ack_user && ctx->ack_unregister_batch)
        ctx->ack_unregister_batch(ctx->ack_user, batch);
}

static void adapter_rate_feedback(void *user, uint64_t rtt_us) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (ops && ops->rate_feedback)
        ops->rate_feedback(ops->user, rtt_us);
}

static void adapter_status_file_phase(void *user, uint32_t batch, int index,
                                      int phase, uint32_t attempts) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    hop_xfer_sender_context_status_file_phase(
        ops ? ops->context : NULL, batch, index, phase, attempts);
}

static unsigned adapter_retry_delay_ms(void *user, unsigned attempt) {
    (void)user;
    return hop_xfer_file_retry_delay_ms((int)attempt);
}

static int adapter_wait_ms(void *user, unsigned duration_ms) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    const hop_xfer_sender_context *ctx = ops ? ops->context : NULL;
    return ctx && ctx->wait_ns &&
           ctx->wait_ns(ctx->user, (uint64_t)duration_ms * 1000000ull);
}

static void adapter_request_reconnect(void *user) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    const hop_xfer_sender_context *ctx = ops ? ops->context : NULL;
    if (ctx && ctx->request_reconnect)
        ctx->request_reconnect(ctx->user);
}

static void adapter_log_message(void *user, int level, const char *message) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    hop_xfer_sender_context_logf(ops ? ops->context : NULL,
                                  level, "%s", message ? message : "");
}

static uint64_t adapter_now_ns(void *user) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && ops->context && ops->context->now_ns
        ? ops->context->now_ns(ops->context->user) : 0;
}

static int image_send_begin(void *user, uint32_t batch, uint32_t width,
                            uint32_t height, uint32_t stride) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (!ops || !ops->send_message) return 0;
    uint8_t payload[20];
    hop_put_be32(payload, batch);
    hop_put_be32(payload + 4, 1);
    hop_put_be32(payload + 8, width);
    hop_put_be32(payload + 12, height);
    hop_put_be32(payload + 16, stride);
    return ops->send_message(ops->user, ops->socket_handle,
        ops->image_begin_message, payload, sizeof(payload), batch);
}

static int image_send_chunk(void *user, uint32_t batch, uint32_t id,
                            uint64_t offset, const uint8_t *data,
                            uint32_t length, uint32_t crc,
                            uint64_t crc_elapsed_ns) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    return ops && ops->send_chunk
        ? ops->send_chunk(ops->user, ops->socket_handle, ops->tx_buffer,
            batch, id, offset, data, length, crc, 0, crc_elapsed_ns) : 0;
}

static int image_send_end(void *user, uint32_t batch, uint32_t id,
                          uint8_t status, uint32_t file_crc) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (!ops || !ops->send_message) return 0;
    uint8_t payload[9];
    hop_put_be32(payload, id);
    payload[4] = status;
    hop_put_be32(payload + 5, file_crc);
    return ops->send_message(ops->user, ops->socket_handle,
        ops->file_end_message, payload, sizeof(payload), batch);
}

static void image_yield_cpu(void *user) {
    const hop_xfer_sender_engine_adapter_ops *ops =
        (const hop_xfer_sender_engine_adapter_ops *)user;
    if (ops && ops->yield_cpu) ops->yield_cpu(ops->user);
}

static int adapter_ops_valid(const hop_xfer_sender_engine_adapter_ops *ops) {
    const hop_xfer_sender_context *ctx = ops ? ops->context : NULL;
    if (!ops || !ctx || !ops->batch || !ops->task_id || !ops->next_index ||
        !ops->ok_count || !ops->tx_buffer || !ops->chunk_size ||
        !ops->chunk_header_size || !ops->send_message || !ops->send_chunk ||
        !ops->yield_cpu || !ctx->now_ns || !ctx->is_cancelled ||
        !ctx->is_batch_cancelled || !ctx->peer_aborted ||
        !ctx->ack_user || !ctx->ack_unregister_batch)
        return 0;
    if (ops->is_image)
        return ops->image_pixels && ops->image_width && ops->image_height &&
               ((uint64_t)ops->image_stride >= (uint64_t)ops->image_width * 4ull);
    return ops->entry_count >= 0 && ops->max_attempts &&
        ops->ack_timeout_ns && ops->entry_at && ops->entry_name &&
        ops->entry_path && ops->entry_size && ops->entry_mtime &&
        ops->entry_flags && ops->open_file && ops->read_file &&
        ops->close_file && ctx->ack_register && ctx->ack_mark_sent &&
        ctx->ack_set_deadline && ctx->ack_wait_take && ctx->ack_unregister &&
        ctx->session_current && ctx->wait_ns;
}

int hop_xfer_sender_engine_adapter_run(
    const hop_xfer_sender_engine_adapter_ops *ops) {
    if (!adapter_ops_valid(ops)) return HOP_XFER_SENDER_JOB_NET_DOWN;

    adapter_unregister_batch((void *)ops, ops->batch);
    if (adapter_cancel_requested((void *)ops, ops->batch))
        return HOP_XFER_SENDER_JOB_CANCELLED;
    if (adapter_peer_aborted((void *)ops, ops->batch))
        return HOP_XFER_SENDER_JOB_PEER_ABORT;

    if (ops->is_image) {
        hop_xfer_sender_image_ops image_ops;
        memset(&image_ops, 0, sizeof(image_ops));
        image_ops.user = (void *)ops;
        image_ops.cancel_requested = adapter_cancel_requested;
        image_ops.peer_aborted = adapter_peer_aborted;
        image_ops.now_ns = adapter_now_ns;
        image_ops.send_image_begin = image_send_begin;
        image_ops.send_image_chunk = image_send_chunk;
        image_ops.send_image_end = image_send_end;
        image_ops.yield_cpu = image_yield_cpu;
        image_ops.rate_feedback = adapter_rate_feedback;
        image_ops.status_file_phase = adapter_status_file_phase;
        return hop_xfer_sender_run_image(
            &image_ops, ops->batch, ops->image_pixels, ops->image_width,
            ops->image_height, ops->image_stride, ops->chunk_size,
            ops->file_ok_status, ops->entry_count, ops->next_index,
            ops->ok_count);
    }

    hop_xfer_sender_engine_ops engine_ops;
    memset(&engine_ops, 0, sizeof(engine_ops));
    engine_ops.user = (void *)ops;
    engine_ops.cancel_requested = adapter_cancel_requested;
    engine_ops.peer_aborted = adapter_peer_aborted;
    engine_ops.entry_at = adapter_entry_at;
    engine_ops.entry_path = adapter_entry_path;
    engine_ops.send_entry = adapter_send_entry;
    engine_ops.wait_ack = adapter_wait_ack;
    engine_ops.unregister_batch = adapter_unregister_batch;
    engine_ops.rate_feedback = adapter_rate_feedback;
    engine_ops.status_file_phase = adapter_status_file_phase;
    engine_ops.retry_delay_ms = adapter_retry_delay_ms;
    engine_ops.wait_ms = adapter_wait_ms;
    engine_ops.request_reconnect = adapter_request_reconnect;
    engine_ops.log_message = adapter_log_message;
    return hop_xfer_sender_run_files(&engine_ops, ops->batch,
        ops->entry_count, ops->next_index, ops->ok_count, ops->max_attempts);
}
