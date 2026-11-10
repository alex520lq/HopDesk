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

/* Shared image sequencing. The platform adapter still owns frame/socket IO
 * and rate pacing; Core owns chunk order, CRC calculation and cancellation. */
typedef enum {
    HOP_XFER_SENDER_READ_OK = 0,
    HOP_XFER_SENDER_READ_EOF,
    HOP_XFER_SENDER_READ_ERROR
} hop_xfer_sender_read_result;

typedef struct {
    const void *platform_entry;
    const char *name;
    const char *relative_path;
    uint64_t size;
    uint64_t mtime;
    uint32_t flags;
} hop_xfer_sender_file_entry;

typedef struct {
    void *user;
    int (*cancel_requested)(void *user, uint32_t batch);
    int (*peer_aborted)(void *user, uint32_t batch);
    int (*open_file)(void *user, const void *platform_entry,
                     intptr_t *file_handle, uint32_t *system_error);
    hop_xfer_sender_read_result (*read_file)(void *user, intptr_t file_handle,
                                              uint8_t *buffer, uint32_t request,
                                              uint32_t *bytes_read,
                                              uint32_t *system_error);
    void (*close_file)(void *user, intptr_t file_handle);
    int (*send_message)(void *user, uint8_t type, const uint8_t *payload,
                        uint32_t length, uint32_t batch);
    int (*send_chunk)(void *user, uint32_t batch, uint32_t id,
                      uint64_t offset, const uint8_t *data, uint32_t length,
                      uint32_t crc, uint64_t read_ns, uint64_t crc_ns);
    int (*ack_register)(void *user, uint32_t batch, uint32_t id);
    int (*ack_mark_sent)(void *user, uint32_t batch, uint32_t id);
    int (*ack_set_deadline)(void *user, uint32_t batch, uint32_t id,
                            uint64_t deadline_ns);
    void (*ack_unregister)(void *user, uint32_t batch, uint32_t id);
    uint64_t (*now_ns)(void *user);
    void (*status_stage_time)(void *user, uint32_t batch, int stage,
                              uint64_t elapsed_ns);
    void (*yield_cpu)(void *user);
    void (*log_message)(void *user, int level, const char *message);
} hop_xfer_sender_file_ops;

hop_xfer_sender_send_result hop_xfer_sender_send_file_entry(
    const hop_xfer_sender_file_ops *ops, uint32_t batch, uint32_t id,
    const hop_xfer_sender_file_entry *entry, uint8_t *begin_buffer,
    uint8_t *chunk_buffer, uint32_t chunk_size, uint64_t ack_timeout_ns,
    uint32_t directory_flag, uint8_t file_ok_status,
    uint8_t file_io_error_status, uint8_t file_begin_message,
    uint8_t file_end_message,
    uint64_t *ack_deadline_ns);

typedef struct {
    void *user;
    int (*cancel_requested)(void *user, uint32_t batch);
    int (*peer_aborted)(void *user, uint32_t batch);
    uint64_t (*now_ns)(void *user);
    int (*send_image_begin)(void *user, uint32_t batch, uint32_t width,
                            uint32_t height, uint32_t stride);
    int (*send_image_chunk)(void *user, uint32_t batch, uint32_t id,
                            uint64_t offset, const uint8_t *data,
                            uint32_t length, uint32_t crc,
                            uint64_t crc_elapsed_ns);
    int (*send_image_end)(void *user, uint32_t batch, uint32_t id,
                          uint8_t status, uint32_t file_crc);
    void (*yield_cpu)(void *user);
    void (*rate_feedback)(void *user, uint64_t rtt_us);
    void (*status_file_phase)(void *user, uint32_t batch, int index,
                              int phase, uint32_t attempts);
} hop_xfer_sender_image_ops;

int hop_xfer_sender_run_image(const hop_xfer_sender_image_ops *ops,
                              uint32_t batch, const uint8_t *pixels,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t chunk_size,
                              uint8_t file_ok_status, int entry_count,
                              int *next_index, int *ok_count);

#ifdef __cplusplus
}
#endif
#endif
