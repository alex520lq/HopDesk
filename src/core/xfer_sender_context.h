#ifndef HOP_XFER_SENDER_CONTEXT_H
#define HOP_XFER_SENDER_CONTEXT_H

/*
 * Platform-neutral dependencies for Sender execution.
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
#include "xfer_task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t size;
} hop_xfer_sender_frame_part;

typedef struct hop_xfer_sender_context {
    void *user;

    /* Monotonic time and interruptible pacing/wait. */
    uint64_t (*now_ns)(void *user);
    /* Returns 1 when the requested wait elapsed, 0 when interrupted/stopping. */
    int (*wait_ns)(void *user, uint64_t duration_ns);
    int (*is_stopping)(void *user);

    /* Socket identity/session validation and serialized complete-frame write.
     * socket_handle is an intptr_t-sized platform socket value.
     * write_frame must preserve whole-frame serialization with other writers.
     */
    int (*session_current)(void *user, intptr_t socket_handle, uint64_t epoch);
    int (*write_frame)(void *user, intptr_t socket_handle, uint64_t epoch,
                       const hop_xfer_sender_frame_part *parts,
                       size_t part_count);

    /* Batch/task cancellation and receiver-originated ABORT. */
    int (*is_cancelled)(void *user, uint32_t batch, uint64_t task_id);
    int (*is_batch_cancelled)(void *user, uint32_t batch);
    int (*peer_aborted)(void *user, uint32_t batch);

    /* Application side effects are callbacks; Sender owns their ordering.
     * job is a borrowed opaque platform job and must not be retained. */
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

/* The clock subset can be adopted independently while the remaining Sender
 * dependencies are migrated in later slices. */
static inline int hop_xfer_sender_context_clock_valid(const hop_xfer_sender_context *ctx) {
    return ctx && ctx->now_ns && ctx->wait_ns && ctx->is_stopping;
}

/* Validate the complete execution contract before starting a fully migrated
 * Sender operation; optional observability callbacks may be NULL. */
static inline int hop_xfer_sender_context_valid(const hop_xfer_sender_context *ctx) {
    return ctx && ctx->now_ns && ctx->wait_ns && ctx->is_stopping &&
           ctx->session_current && ctx->write_frame && ctx->is_cancelled &&
           ctx->peer_aborted && ctx->task_bind_session &&
           ctx->task_set_state && ctx->task_begin_completion &&
           ctx->task_finish && ctx->task_dispose;
}

#ifdef __cplusplus
}
#endif
#endif
