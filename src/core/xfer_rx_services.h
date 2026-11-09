#ifndef HOP_XFER_RX_SERVICES_H
#define HOP_XFER_RX_SERVICES_H

#include <stdint.h>
#include "xfer_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform boundary used by receiver protocol handlers. All callbacks are
 * invoked on the receiver dispatch thread unless explicitly documented by
 * the owning adapter. The context remains owned by the platform adapter. */
typedef struct {
    void *context;
    void (*shutdown_data_channel)(void *context);
    void (*mark_sender_aborted)(void *context, uint32_t batch, uint8_t reason);
    void (*send_abort)(void *context, uint32_t batch, uint8_t reason);
    void (*clear_cancel_request)(void *context);
    int (*create_staging_directory)(void *context, const char *session_hex, uint32_t batch);
    void (*discard_staging_directory)(void *context);
    void (*send_file_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*send_done_ack)(void *context, uint32_t batch,
                          const uint8_t *payload, uint32_t length);
    void (*deliver_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*terminate_batch)(void *context, uint32_t batch, uint8_t reason, int notify_peer);
    void (*begin_batch)(void *context, uint32_t batch);
    int (*finish_batch)(void *context, uint32_t batch, uint32_t ok, uint32_t total);
    void (*reset_batch_state)(void *context);
    void (*reset_file_error)(void *context);
    int (*open_file)(void *context, const void *native_path);
    int (*write_file_chunk)(void *context, hop_xfer_task *task,
                            uint64_t offset, const uint8_t *data, uint32_t length);
    void (*close_file)(void *context);
    int (*file_error)(void *context);
    /* Native path operations are platform adapters. build_path returns a
     * malloc-compatible candidate owned by Receiver until context reset. */
    void *(*build_path)(void *context, const char *relative_path, uint32_t length);
    int (*prepare_file_path)(void *context, const void *native_path, int is_directory);
    int (*is_top_level_path)(void *context, const void *native_path);
    int (*remove_file)(void *context, const void *native_path);
    int (*add_top_path)(void *context, const void *native_path);
    void (*remove_top_path)(void *context, const void *native_path);
    void (*finish_clipboard)(void *context, uint32_t batch,
                             const void *image_data, uint32_t width,
                             uint32_t height, uint32_t stride, int image_valid,
                             const void *const *top_paths, int top_count, int allow_publish);
    void (*cancel_clipboard)(void *context, uint32_t batch);
    uint64_t (*now_ns)(void *context);
    int (*status_begin)(void *context, uint32_t batch, uint32_t files,
                        uint64_t bytes, const char *name, int phase);
    void (*status_end)(void *context, uint32_t batch, int phase, const char *detail);
    void (*status_add_received_file)(void *context, uint32_t batch, uint32_t id,
                                     const char *path, uint64_t size, int directory);
    void (*status_image)(void *context, uint32_t batch, uint64_t size);
    void (*status_stage_time)(void *context, uint32_t batch, int stage, uint64_t elapsed_ns);
    void (*status_chunk_received)(void *context, uint32_t batch, uint32_t id,
                                  uint64_t offset, uint32_t length,
                                  uint64_t crc_ns, uint64_t receive_ns);
    void (*status_file_phase)(void *context, uint32_t batch, int index,
                              int phase, uint32_t attempts);
    void (*status_set_files_total)(void *context, uint32_t total);
    void (*status_reset)(void *context);
} hop_xfer_rx_services;

#ifdef __cplusplus
}
#endif
#endif
