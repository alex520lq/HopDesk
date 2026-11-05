#ifndef HOP_XFER_ACK_WINDOW_H
#define HOP_XFER_ACK_WINDOW_H

#include <stdint.h>
#include <string.h>

/* A small, fixed ACK window. Callers serialize access with their platform lock. */
#define HOP_XFER_ACK_WINDOW_CAPACITY 4u

typedef struct {
    uint32_t batch;
    uint32_t id;
    uint64_t sent_ns;
    uint64_t received_ns;
    int status;
    uint8_t used;
    uint8_t ready;
} hop_xfer_ack_slot;

typedef struct {
    hop_xfer_ack_slot slots[HOP_XFER_ACK_WINDOW_CAPACITY];
} hop_xfer_ack_window;

static inline void hop_xfer_ack_window_reset(hop_xfer_ack_window *window) {
    if (window) memset(window, 0, sizeof(*window));
}

static inline int hop_xfer_ack_window_register(hop_xfer_ack_window *window,
                                                uint32_t batch, uint32_t id) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch && slot->id == id) {
            slot->sent_ns = 0;
            slot->received_ns = 0;
            slot->status = 0;
            slot->ready = 0;
            return 1;
        }
    }
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->batch = batch;
            slot->id = id;
            slot->used = 1;
            return 1;
        }
    }
    return 0;
}

static inline int hop_xfer_ack_window_mark_sent(hop_xfer_ack_window *window,
                                                 uint32_t batch, uint32_t id,
                                                 uint64_t sent_ns) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch && slot->id == id && !slot->ready) {
            slot->sent_ns = sent_ns;
            return 1;
        }
    }
    return 0;
}

static inline int hop_xfer_ack_window_deliver(hop_xfer_ack_window *window,
                                               uint32_t batch, uint32_t id,
                                               int status, uint64_t received_ns) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch && slot->id == id) {
            if (slot->ready) return 0; /* duplicate ACK: preserve the first result */
            slot->status = status;
            slot->received_ns = received_ns;
            slot->ready = 1;
            return 1;
        }
    }
    return 0; /* stale or unsolicited ACK */
}

static inline int hop_xfer_ack_window_take(hop_xfer_ack_window *window,
                                            uint32_t batch, uint32_t id,
                                            int *status, uint64_t *rtt_ns) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch && slot->id == id && slot->ready) {
            if (status) *status = slot->status;
            if (rtt_ns) *rtt_ns = slot->sent_ns && slot->received_ns >= slot->sent_ns ?
                                  slot->received_ns - slot->sent_ns : 0;
            memset(slot, 0, sizeof(*slot));
            return 1;
        }
    }
    return 0;
}

static inline void hop_xfer_ack_window_unregister(hop_xfer_ack_window *window,
                                                   uint32_t batch, uint32_t id) {
    if (!window || !batch || !id) return;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch && slot->id == id)
            memset(slot, 0, sizeof(*slot));
    }
}

static inline void hop_xfer_ack_window_unregister_batch(hop_xfer_ack_window *window,
                                                         uint32_t batch) {
    if (!window || !batch) return;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->used && slot->batch == batch)
            memset(slot, 0, sizeof(*slot));
    }
}

static inline uint32_t hop_xfer_ack_window_count(const hop_xfer_ack_window *window) {
    uint32_t count = 0;
    if (!window) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i)
        if (window->slots[i].used) count++;
    return count;
}

#endif
