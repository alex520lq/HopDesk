#include "xfer_rx_queue.h"
#include <string.h>

void hop_xfer_rx_queue_init(hop_xfer_rx_queue *q, uint32_t slots, uint64_t bytes) {
    if (!q) return;
    memset(q, 0, sizeof(*q));
    q->capacity_slots = slots;
    q->capacity_bytes = bytes;
}

int hop_xfer_rx_queue_can_push(const hop_xfer_rx_queue *q, uint64_t bytes) {
    if (!q || bytes > q->capacity_bytes) return 0;
    return q->queued_slots + q->busy_slots + q->reserved_slots < q->capacity_slots &&
           q->queued_bytes <= q->capacity_bytes - bytes;
}

int hop_xfer_rx_queue_reserve(hop_xfer_rx_queue *q, uint64_t bytes) {
    if (!hop_xfer_rx_queue_can_push(q, bytes)) return 0;
    q->reserved_slots++;
    q->queued_bytes += bytes;
    return 1;
}

void hop_xfer_rx_queue_rollback(hop_xfer_rx_queue *q, uint64_t bytes) {
    if (!q || !q->reserved_slots) return;
    q->reserved_slots--;
    q->queued_bytes = bytes > q->queued_bytes ? 0 : q->queued_bytes - bytes;
}

int hop_xfer_rx_queue_commit_reserved(hop_xfer_rx_queue *q) {
    if (!q || !q->reserved_slots || q->queued_slots >= q->capacity_slots) return 0;
    q->reserved_slots--;
    q->queued_slots++;
    return 1;
}

int hop_xfer_rx_queue_push(hop_xfer_rx_queue *q, uint64_t bytes) {
    if (!hop_xfer_rx_queue_can_push(q, bytes)) return 0;
    q->queued_slots++;
    q->queued_bytes += bytes;
    return 1;
}

int hop_xfer_rx_queue_dequeue(hop_xfer_rx_queue *q) {
    if (!q || !q->queued_slots || q->busy_slots) return 0;
    q->queued_slots--;
    q->busy_slots++;
    return 1;
}

void hop_xfer_rx_queue_complete(hop_xfer_rx_queue *q, uint64_t bytes) {
    if (!q || !q->busy_slots) return;
    q->busy_slots--;
    q->queued_bytes = bytes > q->queued_bytes ? 0 : q->queued_bytes - bytes;
}

int hop_xfer_rx_queue_drained(const hop_xfer_rx_queue *q) {
    return q && q->queued_slots == 0 && q->busy_slots == 0 && q->reserved_slots == 0;
}
