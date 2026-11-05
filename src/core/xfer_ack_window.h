#ifndef HOP_XFER_ACK_WINDOW_H
#define HOP_XFER_ACK_WINDOW_H

#include <stdint.h>
#include <string.h>

#define HOP_XFER_ACK_WINDOW_CAPACITY 4u

typedef struct {
    uint32_t batch;
    uint32_t id;
    int status;
    uint64_t sent_ns;
    uint64_t rtt_ns;
    uint8_t state; /* 0 free, 1 registered, 2 sent, 3 delivered */
} hop_xfer_ack_slot;

typedef struct {
    hop_xfer_ack_slot slots[HOP_XFER_ACK_WINDOW_CAPACITY];
} hop_xfer_ack_window;

/* The caller serializes access with the platform ACK lock. */
static inline void hop_xfer_ack_window_reset(hop_xfer_ack_window *window) {
    if (window) memset(window, 0, sizeof(*window));
}

static inline uint32_t hop_xfer_ack_window_count(const hop_xfer_ack_window *window) {
    if (!window) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i)
        if (window->slots[i].state) ++count;
    return count;
}

static inline int hop_xfer_ack_window_register(hop_xfer_ack_window *window,
                                                uint32_t batch, uint32_t id) {
    if (!window || !batch || !id) return 0;
    hop_xfer_ack_slot *free_slot = NULL;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->state && slot->batch == batch && slot->id == id) return 0;
        if (!slot->state && !free_slot) free_slot = slot;
    }
    if (!free_slot) return 0;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->batch = batch;
    free_slot->id = id;
    free_slot->state = 1;
    return 1;
}

static inline int hop_xfer_ack_window_mark_sent(hop_xfer_ack_window *window,
                                                 uint32_t batch, uint32_t id,
                                                 uint64_t sent_ns) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->state == 1 && slot->batch == batch && slot->id == id) {
            slot->sent_ns = sent_ns;
            slot->state = 2;
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
        if (slot->state == 2 && slot->batch == batch && slot->id == id) {
            slot->status = status;
            slot->rtt_ns = received_ns >= slot->sent_ns ?
                received_ns - slot->sent_ns : 0;
            slot->state = 3;
            return 1;
        }
    }
    return 0;
}

static inline int hop_xfer_ack_window_take(hop_xfer_ack_window *window,
                                            uint32_t batch, uint32_t id,
                                            int *status, uint64_t *rtt_ns) {
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->state == 3 && slot->batch == batch && slot->id == id) {
            if (status) *status = slot->status;
            if (rtt_ns) *rtt_ns = slot->rtt_ns;
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
        if (slot->state && slot->batch == batch && slot->id == id)
            memset(slot, 0, sizeof(*slot));
    }
}

static inline void hop_xfer_ack_window_unregister_batch(hop_xfer_ack_window *window,
                                                         uint32_t batch) {
    if (!window || !batch) return;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->state && slot->batch == batch)
            memset(slot, 0, sizeof(*slot));
    }
}

#endif
