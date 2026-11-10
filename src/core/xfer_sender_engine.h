#ifndef HOP_XFER_SENDER_ENGINE_H
#define HOP_XFER_SENDER_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOP_XFER_SENDER_JOB_DONE = 0,
    HOP_XFER_SENDER_JOB_NET_DOWN = 1,
    HOP_XFER_SENDER_JOB_CANCELLED = 2,
    HOP_XFER_SENDER_JOB_PEER_ABORT = 3
} hop_xfer_sender_job_result;

typedef enum {
    HOP_XFER_SENDER_SEND_PENDING = 0,
    HOP_XFER_SENDER_SEND_DEAD,
    HOP_XFER_SENDER_SEND_PEER_ABORT,
    HOP_XFER_SENDER_SEND_CANCELLED,
    HOP_XFER_SENDER_SEND_RETRYABLE
} hop_xfer_sender_send_result;

typedef enum {
    HOP_XFER_SENDER_ACK_OK = 0,
    HOP_XFER_SENDER_ACK_BADPATH,
    HOP_XFER_SENDER_ACK_TIMEOUT,
    HOP_XFER_SENDER_ACK_SEND_DEAD,
    HOP_XFER_SENDER_ACK_PEER_ABORT,
    HOP_XFER_SENDER_ACK_CANCELLED,
    HOP_XFER_SENDER_ACK_OTHER_REMOTE,
    HOP_XFER_SENDER_ACK_LOCAL_FAILURE
} hop_xfer_sender_ack_result;

/*
 * Shared file-window protocol policy.
 *
 * The engine owns window fill/drain ordering and the durable contiguous
 * checkpoint. Platform adapters own file handles, frame IO, ACK coordinator
 * waits, clocks, logging and status publication.
 *
 * entry_at returns a borrowed platform entry for one index. It must remain
 * valid until this call returns. No callback may retain that pointer.
 */
typedef struct {
    void *user;
    int (*cancel_requested)(void *user, uint32_t batch);
    int (*peer_aborted)(void *user, uint32_t batch);
    const void *(*entry_at)(void *user, int index);
    const char *(*entry_path)(void *user, const void *entry);
    hop_xfer_sender_send_result (*send_entry)(void *user, uint32_t batch,
                                               uint32_t id, const void *entry,
                                               uint64_t *ack_deadline_ns);
    hop_xfer_sender_ack_result (*wait_ack)(void *user, uint32_t batch,
                                            uint32_t id, uint64_t deadline_ns,
                                            uint64_t *rtt_us);
    void (*unregister_batch)(void *user, uint32_t batch);
    void (*rate_feedback)(void *user, uint64_t rtt_us);
    void (*status_file_phase)(void *user, uint32_t batch, int index,
                              int phase, uint32_t attempts);
    unsigned (*retry_delay_ms)(void *user, unsigned attempt);
    int (*wait_ms)(void *user, unsigned duration_ms);
    void (*request_reconnect)(void *user);
    void (*log_message)(void *user, int level, const char *message);
} hop_xfer_sender_engine_ops;

/*
 * Sends a file job through a bounded ACK window.
 *
 * next_index is the only reconnect checkpoint and advances only for a
 * contiguous entry with a received remote ACK. ok_count increases only for
 * HOP_XFER_SENDER_ACK_OK. A BADPATH or other remote ACK resolves the entry
 * without counting it as successful. A local failure or missing ACK never
 * advances the checkpoint.
 */
int hop_xfer_sender_run_files(const hop_xfer_sender_engine_ops *ops,
                              uint32_t batch, int entry_count,
                              int *next_index, int *ok_count,
                              unsigned max_attempts);

#ifdef __cplusplus
}
#endif
#endif
