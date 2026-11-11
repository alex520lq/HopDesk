#ifndef HOP_XFER_RX_QUEUE_H
#define HOP_XFER_RX_QUEUE_H

#include <stdint.h>
#include "xfer_budget.h"

/* Platform-neutral bounded receive-queue accounting. The platform adapter
 * owns the ring storage and mutex/condition variables; callers serialize all
 * state transitions with that adapter's queue lock. */
typedef struct {
    uint32_t capacity_slots;
    uint64_t capacity_bytes;
    uint32_t queued_slots;
    uint32_t busy_slots;
    uint32_t reserved_slots;
    uint64_t queued_bytes; /* Includes reserved and currently-writing payloads. */
} hop_xfer_rx_queue;

void hop_xfer_rx_queue_init(hop_xfer_rx_queue *queue,
                            uint32_t capacity_slots, uint64_t capacity_bytes);
int hop_xfer_rx_queue_can_push(const hop_xfer_rx_queue *queue, uint64_t bytes);
int hop_xfer_rx_queue_reserve(hop_xfer_rx_queue *queue, uint64_t bytes);
void hop_xfer_rx_queue_rollback(hop_xfer_rx_queue *queue, uint64_t bytes);
int hop_xfer_rx_queue_commit_reserved(hop_xfer_rx_queue *queue);
int hop_xfer_rx_queue_push(hop_xfer_rx_queue *queue, uint64_t bytes);
int hop_xfer_rx_queue_dequeue(hop_xfer_rx_queue *queue);
void hop_xfer_rx_queue_complete(hop_xfer_rx_queue *queue, uint64_t bytes);
int hop_xfer_rx_queue_drained(const hop_xfer_rx_queue *queue);

#endif
