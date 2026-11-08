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
    void (*send_file_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*send_done_ack)(void *context, uint32_t batch,
                          const uint8_t *payload, uint32_t length);
    void (*deliver_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*terminate_batch)(void *context, uint32_t batch, uint8_t reason, int notify_peer);
    void (*begin_batch)(void *context, uint32_t batch);
    int (*finish_batch)(void *context, uint32_t batch, uint32_t ok, uint32_t total);
    void (*reset_file_error)(void *context);
    int (*open_file)(void *context, const void *native_path);
    int (*write_file_chunk)(void *context, hop_xfer_task *task,
                            uint64_t offset, const uint8_t *data, uint32_t length);
    void (*close_file)(void *context);
    int (*file_error)(void *context);
    int (*add_top_path)(void *context, const void *native_path);
    void (*remove_top_path)(void *context, const void *native_path);
    void (*finish_clipboard)(void *context, uint32_t batch,
                             const void *image_data, uint32_t width,
                             uint32_t height, uint32_t stride, int image_valid,
                             const void *const *top_paths, int top_count, int allow_publish);
    void (*cancel_clipboard)(void *context, uint32_t batch);
} hop_xfer_rx_services;

#ifdef __cplusplus
}
#endif
#endif
