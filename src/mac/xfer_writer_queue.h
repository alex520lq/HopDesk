#ifndef HOP_MAC_XFER_WRITER_QUEUE_H
#define HOP_MAC_XFER_WRITER_QUEUE_H

#include <stdint.h>

typedef struct hop_writer_queue hop_writer_queue;

typedef struct {
    uint32_t io_deadline_ms;
    int (*reserve_task_rx)(void *context, void *task_owner, uint32_t bytes);
    void (*release_task_rx)(void *context, void *task_owner, uint32_t bytes);
    int (*watchdog_begin)(void *context, const char *label, uint32_t deadline_ms);
    void (*watchdog_end)(void *context, int guard);
    uint64_t (*now_ns)(void *context);
    void (*write_time)(void *context, void *task_owner, uint64_t elapsed_ns);
    void (*open_error)(void *context, const char *path, int error);
    void (*write_error)(void *context, uint64_t offset, int error);
} hop_writer_queue_callbacks;

hop_writer_queue *hop_writer_queue_create(
    void *callback_context, const hop_writer_queue_callbacks *callbacks);
int hop_writer_queue_enqueue_open(hop_writer_queue *queue, const char *path);
int hop_writer_queue_enqueue_write(hop_writer_queue *queue, void *task_owner,
                                   uint64_t offset, const uint8_t *data,
                                   uint32_t length);
int hop_writer_queue_enqueue_close(hop_writer_queue *queue);
void hop_writer_queue_drain(hop_writer_queue *queue);
void hop_writer_queue_error_reset(hop_writer_queue *queue);
int hop_writer_queue_error(const hop_writer_queue *queue);
int hop_writer_queue_stop(hop_writer_queue *queue);
void hop_writer_queue_destroy(hop_writer_queue *queue);

#endif
