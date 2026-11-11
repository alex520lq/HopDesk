#ifndef HOP_XFER_SENDER_ENGINE_ADAPTER_H
#define HOP_XFER_SENDER_ENGINE_ADAPTER_H

#include <stdint.h>
#include "xfer_sender_engine.h"
#include "xfer_sender_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared orchestration for the file/image Sender engine adapters. Native file
 * handles, entry layout, socket writes, rate state and CPU yielding stay in
 * the platform callback table; ACK, status, retry and engine wiring stay here. */
typedef struct {
    void *user;
    const hop_xfer_sender_context *context;
    intptr_t socket_handle;
    uint64_t connection_epoch;
    uint32_t batch;
    uint64_t task_id;
    int is_image;
    int entry_count;
    int *next_index;
    int *ok_count;
    const uint8_t *image_pixels;
    uint32_t image_width, image_height, image_stride;
    uint8_t *tx_buffer;
    uint32_t chunk_size;
    uint32_t chunk_header_size;
    uint64_t ack_timeout_ns;
    uint32_t ack_poll_interval_ms;
    unsigned max_attempts;
    uint32_t directory_flag;
    uint8_t file_ok_status, file_io_error_status;
    uint8_t file_begin_message, file_end_message, image_begin_message;

    const void *(*entry_at)(void *user, int index);
    const char *(*entry_name)(void *user, const void *entry);
    const char *(*entry_path)(void *user, const void *entry);
    uint64_t (*entry_size)(void *user, const void *entry);
    uint64_t (*entry_mtime)(void *user, const void *entry);
    uint32_t (*entry_flags)(void *user, const void *entry);

    int (*open_file)(void *user, const void *entry,
                     intptr_t *file_handle, uint32_t *system_error);
    hop_xfer_sender_read_result (*read_file)(
        void *user, intptr_t file_handle, uint8_t *buffer, uint32_t request,
        uint32_t *bytes_read, uint32_t *system_error);
    void (*close_file)(void *user, intptr_t file_handle);
    int (*send_message)(void *user, intptr_t socket_handle, uint8_t type,
                        const uint8_t *payload, uint32_t length, uint32_t batch);
    int (*send_chunk)(void *user, intptr_t socket_handle, uint8_t *tx_buffer,
                      uint32_t batch, uint32_t id, uint64_t offset,
                      const uint8_t *data, uint32_t length, uint32_t crc,
                      uint64_t read_ns, uint64_t crc_ns);
    void (*yield_cpu)(void *user);
    void (*rate_feedback)(void *user, uint64_t rtt_us);
} hop_xfer_sender_engine_adapter_ops;

int hop_xfer_sender_engine_adapter_run(
    const hop_xfer_sender_engine_adapter_ops *ops);

#ifdef __cplusplus
}
#endif
#endif
