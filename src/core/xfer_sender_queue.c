#include "xfer_sender_queue.h"

#include <stddef.h>

static int queue_ops_valid(const hop_xfer_sender_queue_ops *ops) {
    return ops && ops->next && ops->set_next && ops->is_cancelled &&
           ops->batch && ops->retry_at_ms && ops->give_up_at_ms;
}

static void queue_remove(hop_xfer_sender_queue *queue,
                         const hop_xfer_sender_queue_ops *ops,
                         void *previous, void *node, void *next) {
    if (previous) ops->set_next(ops->user, previous, next);
    else queue->head = next;

    if (queue->tail == node) queue->tail = previous;
    ops->set_next(ops->user, node, NULL);
}

void hop_xfer_sender_queue_init(hop_xfer_sender_queue *queue) {
    if (queue) {
        queue->head = NULL;
        queue->tail = NULL;
        queue->count = 0;
    }
}

int hop_xfer_sender_queue_push_back(hop_xfer_sender_queue *queue,
                                    const hop_xfer_sender_queue_ops *ops,
                                    void *node) {
    if (!queue || !node || !queue_ops_valid(ops) || queue->count == UINT32_MAX)
        return 0;
    ops->set_next(ops->user, node, NULL);
    if (queue->tail) ops->set_next(ops->user, queue->tail, node);
    else queue->head = node;
    queue->tail = node;
    queue->count++;
    return 1;
}

int hop_xfer_sender_queue_push_front(hop_xfer_sender_queue *queue,
                                     const hop_xfer_sender_queue_ops *ops,
                                     void *node) {
    if (!queue || !node || !queue_ops_valid(ops) || queue->count == UINT32_MAX)
        return 0;
    ops->set_next(ops->user, node, queue->head);
    queue->head = node;
    if (!queue->tail) queue->tail = node;
    queue->count++;
    return 1;
}

int hop_xfer_sender_queue_contains_batch(const hop_xfer_sender_queue *queue,
                                         const hop_xfer_sender_queue_ops *ops,
                                         uint32_t batch) {
    if (!queue || !batch || !queue_ops_valid(ops)) return 0;
    for (void *node = queue->head; node; node = ops->next(ops->user, node))
        if (ops->batch(ops->user, node) == batch) return 1;
    return 0;
}

uint32_t hop_xfer_sender_queue_count(const hop_xfer_sender_queue *queue) {
    return queue ? queue->count : 0;
}

int hop_xfer_sender_queue_empty(const hop_xfer_sender_queue *queue) {
    return !queue || queue->head == NULL;
}

void *hop_xfer_sender_queue_detach_all(hop_xfer_sender_queue *queue) {
    if (!queue) return NULL;
    void *head = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    return head;
}

void *hop_xfer_sender_queue_take_cancelled(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops) {
    if (!queue || !queue_ops_valid(ops)) return NULL;

    void *previous = NULL;
    void *node = queue->head;
    while (node) {
        void *next = ops->next(ops->user, node);
        if (ops->is_cancelled(ops->user, node)) {
            queue_remove(queue, ops, previous, node, next);
            if (queue->count) queue->count--;
            return node;
        }
        previous = node;
        node = next;
    }
    return NULL;
}

void *hop_xfer_sender_queue_take_head(
    hop_xfer_sender_queue *queue,
    const hop_xfer_sender_queue_ops *ops,
    uint64_t now_ms,
    uint32_t cancel_batch,
    int peer_ready,
    int allow_ready,
    hop_xfer_sender_queue_take_result *result) {
    if (result) *result = HOP_XFER_SENDER_QUEUE_NONE;
    if (!queue || !result || !queue_ops_valid(ops)) return NULL;

    void *node = queue->head;
    if (!node) return NULL;

    uint64_t give_up_at = ops->give_up_at_ms(ops->user, node);
    int expired = (cancel_batch != 0 &&
                   ops->batch(ops->user, node) == cancel_batch) ||
                  (give_up_at != 0 && now_ms >= give_up_at);
    if (expired) {
        void *next = ops->next(ops->user, node);
        queue_remove(queue, ops, NULL, node, next);
        if (queue->count) queue->count--;
        *result = HOP_XFER_SENDER_QUEUE_EXPIRED;
        return node;
    }

    if (allow_ready && peer_ready &&
        ops->retry_at_ms(ops->user, node) <= now_ms) {
        void *next = ops->next(ops->user, node);
        queue_remove(queue, ops, NULL, node, next);
        if (queue->count) queue->count--;
        *result = HOP_XFER_SENDER_QUEUE_READY;
        return node;
    }

    return NULL;
}
