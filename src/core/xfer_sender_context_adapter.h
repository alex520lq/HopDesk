#ifndef HOP_XFER_SENDER_CONTEXT_ADAPTER_H
#define HOP_XFER_SENDER_CONTEXT_ADAPTER_H

#include "xfer_sender_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Core-side forwarding helpers for the borrowed Sender context. These helpers
 * centralize null/identity checks and callback argument ordering. Platform
 * socket, lock, status and logging implementations remain in the adapters.
 */
int hop_xfer_sender_context_task_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch,
    const hop_xfer_task *task);
int hop_xfer_sender_context_job_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch, uint64_t task_id);
int hop_xfer_sender_context_task_bind_session(
    const hop_xfer_sender_context *context, const hop_xfer_task *task,
    const uint8_t session[HOP_TASK_SESSION_BYTES]);
int hop_xfer_sender_context_task_set_state(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state);
int hop_xfer_sender_context_task_begin_completion(
    const hop_xfer_sender_context *context, const hop_xfer_task *task);
void hop_xfer_sender_context_task_finish(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state);
int hop_xfer_sender_context_task_dispose(
    const hop_xfer_sender_context *context, hop_xfer_task *task);

void hop_xfer_sender_context_status_start_job(
    const hop_xfer_sender_context *context, const void *job);
void hop_xfer_sender_context_status_end(
    const hop_xfer_sender_context *context, uint32_t batch, int phase,
    const char *detail);
void hop_xfer_sender_context_status_file_phase(
    const hop_xfer_sender_context *context, uint32_t batch, int index,
    int phase, uint32_t attempts);
void hop_xfer_sender_context_status_stage_time(
    const hop_xfer_sender_context *context, uint32_t batch, int stage,
    uint64_t elapsed_ns);
void hop_xfer_sender_context_status_chunk_timed(
    const hop_xfer_sender_context *context, uint32_t batch, uint32_t id,
    uint64_t offset, uint32_t length, uint64_t read_ns, uint64_t crc_ns,
    uint64_t send_ns, uint64_t receive_ns);
void hop_xfer_sender_context_logf(
    const hop_xfer_sender_context *context, int level,
    const char *format, ...);

#ifdef __cplusplus
}
#endif
#endif
