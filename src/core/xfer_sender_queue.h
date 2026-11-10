#ifndef HOP_XFER_SENDER_QUEUE_H
#define HOP_XFER_SENDER_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intrusive FIFO view. The caller owns synchronization and node storage. */
typedef struct {
    void *head;
    void *tail;
    uint32_t count;
} hop_xfer_sender_queue;

typedef struct {
    void *user;
    void *(*next)(void *user, const void *node);
    void (*set_next)(void *user, void *node, void *next);
    int (*is_cancelled)(void *user, const void *node);
    uint32_t (*batch)(void *user, const void *node);
    uint64_t (*retry_at_ms)(void *user, const void *node);
    uint64_t (*give_up_at_ms)(void *user, const void *node);
} hop_xfer_sender_queue_ops;

typedef enum {
    HOP_XFER_SENDER_QUEUE_NONE = 0,
    HOP_XFER_SENDER_QUEUE_READY,
    HOP_XFER_SENDER_QUEUE_EXPIRED
} hop_xfer_sender_queue_take_result;

/* Queue storage and intrusive-link mutation are shared Core responsibilities.
 * The caller owns synchronization and node storage. */
void hop_xfer_sender_queue_init(hop_xfer_sender_queue *queue);
int hop_xfer_sender_queue_push_back(hop_xfer_sender_queue *queue,
                                    const hop_xfer_sender_queue_ops *ops,
                                    void *node);
int hop_xfer_sender_queue_push_front(hop_xfer_sender_queue *queue,
                                     const hop_xfer_sender_queue_ops *ops,
                                     void *node);
int hop_xfer_sender_queue_contains_batch(const hop_xfer_sender_queue *queue,
                                         const hop_xfer_sender_queue_ops *ops,
                                         uint32_t batch);
uint32_t hop_xfer_sender_queue_count(const hop_xfer_sender_queue *queue);
int hop_xfer_sender_queue_empty(const hop_xfer_sender_queue *queue);
void *hop_xfer_sender_queue_detach_all(hop_xfer_sender_queue *queue);

/* Removes the first individually-cancelled task, even when it is not the head.
 * The remaining FIFO order and tail pointer are preserved. */
void *hop_xfer_sender_queue_take_cancelled(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops);

/* Only the FIFO head may be selected; later jobs never overtake a paused head.
 * Cancellation/deadline expiry is processed even while the peer is offline.
 * A ready job is selected only when peer_ready && allow_ready && retry_at<=now.
 * The caller must hold the queue's platform lock for the entire call. */
void *hop_xfer_sender_queue_take_head(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops,
    uint64_t now_ms,
    uint32_t cancel_batch,
    int peer_ready,
    int allow_ready,
    hop_xfer_sender_queue_take_result *result);

#ifdef __cplusplus
}
#endif
#endif
