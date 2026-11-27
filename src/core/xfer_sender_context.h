#ifndef HOP_XFER_SENDER_CONTEXT_H
#define HOP_XFER_SENDER_CONTEXT_H

/*
 * Platform-neutral dependencies, forwarding helpers, and chunk sender for
 * Sender execution.
 *
 * The context is borrowed for one sender operation. The platform adapter owns
 * its storage and must keep it alive until the operation and all ACK waits
 * have returned. No callback may retain the context or its user pointer.
 *
 * This header deliberately contains no HANDLE, SOCKET, pthread, HWND or
 * Objective-C types so Core and both platform adapters share one contract.
 */

#include <stddef.h>
#include <stdint.h>
#include "xfer_rate.h"
#include "xfer_task.h"
#include "xfer_ack_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t size;
} hop_xfer_sender_frame_part;

typedef struct hop_xfer_sender_context {
    void *user;
    /* Borrowed platform-owned coordinator; synchronization stays platform-side. */
    hop_xfer_ack_coordinator *ack_coordinator;

    /* Monotonic time and interruptible pacing/wait. */
    uint64_t (*now_ns)(void *user);
    /* Returns 1 when the requested wait elapsed, 0 when interrupted/stopping. */
    int (*wait_ns)(void *user, uint64_t duration_ns);
    int (*is_stopping)(void *user);

    /* Socket identity/session validation and serialized complete-frame write. */
    int (*session_current)(void *user, intptr_t socket_handle, uint64_t epoch);
    int (*snapshot_session)(void *user, intptr_t socket_handle,
                            uint8_t session[HOP_TASK_SESSION_BYTES], uint64_t *epoch);
    int (*write_frame)(void *user, intptr_t socket_handle, uint64_t epoch,
                       const hop_xfer_sender_frame_part *parts,
                       size_t part_count);

    /* Batch/task cancellation and receiver-originated ABORT. */
    int (*is_cancelled)(void *user, uint32_t batch, uint64_t task_id);
    int (*is_batch_cancelled)(void *user, uint32_t batch);
    int (*peer_aborted)(void *user, uint32_t batch);
    void (*request_reconnect)(void *user);

    /* Application side effects are callbacks; Sender owns their ordering. */
    void (*status_start_job)(void *user, const void *job);
    void (*status_end)(void *user, uint32_t batch, int phase,
                       const char *detail);
    void (*status_file_phase)(void *user, uint32_t batch, int index,
                              int phase, uint32_t attempts);
    void (*status_stage_time)(void *user, uint32_t batch, int stage,
                              uint64_t elapsed_ns);
    void (*status_chunk_timed)(void *user, uint32_t batch, uint32_t id,
                               uint64_t offset, uint32_t length,
                               uint64_t read_ns, uint64_t crc_ns,
                               uint64_t send_ns, uint64_t receive_ns);
    void (*log_message)(void *user, int level, const char *message);

    /* Task state remains owned by the existing Task service. */
    int (*task_bind_session)(void *user, const hop_xfer_task *task,
                             const uint8_t session[HOP_TASK_SESSION_BYTES]);
    int (*task_set_state)(void *user, hop_xfer_task *task, int state);
    int (*task_begin_completion)(void *user, const hop_xfer_task *task);
    void (*task_finish)(void *user, hop_xfer_task *task, int outcome);
    void (*task_dispose)(void *user, hop_xfer_task *task);
} hop_xfer_sender_context;

static inline int hop_xfer_sender_context_clock_valid(const hop_xfer_sender_context *ctx) {
    return ctx && ctx->now_ns && ctx->wait_ns && ctx->is_stopping;
}

static inline int hop_xfer_sender_context_valid(const hop_xfer_sender_context *ctx) {
    return ctx && ctx->now_ns && ctx->wait_ns && ctx->is_stopping &&
           ctx->session_current && ctx->snapshot_session && ctx->write_frame && ctx->is_cancelled &&
           ctx->is_batch_cancelled && ctx->peer_aborted && ctx->request_reconnect &&
           ctx->ack_coordinator && ctx->task_bind_session &&
           ctx->task_set_state && ctx->task_begin_completion &&
           ctx->task_finish && ctx->task_dispose;
}

/* ---- Context Forwarding Helpers ---- */

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

/* ---- Chunk Pacing & Framing ---- */

typedef struct {
    const hop_xfer_sender_context *context;
    hop_xfer_rate_limiter *rate;
    intptr_t socket_handle;
    uint8_t *tx_buffer;
    uint32_t tx_capacity;
    void (*rate_feedback)(uint64_t ack_rtt_us);
} hop_xfer_sender_chunk_ops;

int hop_xfer_sender_send_chunk(const hop_xfer_sender_chunk_ops *ops,
                               uint32_t batch, uint32_t id, uint64_t offset,
                               const uint8_t *data, uint32_t length,
                               uint32_t crc, uint64_t read_ns,
                               uint64_t crc_ns);

#ifdef __cplusplus
}
#endif
#endif
