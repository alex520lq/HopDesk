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
    uint64_t deadline_ns;
    uint64_t received_ns;
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

/* Advance only the next contiguous resolved file. This is the reconnect
 * checkpoint (zero-based); an out-of-order ACK must never skip a gap. */
static inline int hop_xfer_ack_window_advance_checkpoint(int *checkpoint,
                                                          int resolved_index) {
    if (!checkpoint || *checkpoint < 0 || resolved_index < 0 ||
        resolved_index != *checkpoint) return 0;
    ++(*checkpoint);
    return 1;
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

static inline int hop_xfer_ack_window_set_deadline(hop_xfer_ack_window *window,
                                                       uint32_t batch, uint32_t id,
                                                       uint64_t deadline_ns) {
    if (!window || !batch || !id || !deadline_ns) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        /* ACK may arrive while the sender is returning from FILE_END send.
         * State 3 is therefore valid: take() runs only after this deadline is set. */
        if ((slot->state == 2 || slot->state == 3) &&
            slot->batch == batch && slot->id == id) {
            slot->deadline_ns = deadline_ns;
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
            slot->received_ns = received_ns;
            slot->rtt_ns = received_ns >= slot->sent_ns ?
                received_ns - slot->sent_ns : 0;
            slot->state = 3;
            return 1;
        }
    }
    return 0;
}

static inline int hop_xfer_ack_window_take_ex(hop_xfer_ack_window *window,
                                               uint32_t batch, uint32_t id,
                                               int *status, uint64_t *rtt_ns,
                                               int *deadline_expired) {
    if (deadline_expired) *deadline_expired = 0;
    if (!window || !batch || !id) return 0;
    for (uint32_t i = 0; i < HOP_XFER_ACK_WINDOW_CAPACITY; ++i) {
        hop_xfer_ack_slot *slot = &window->slots[i];
        if (slot->state == 3 && slot->batch == batch && slot->id == id) {
            if (status) *status = slot->status;
            if (rtt_ns) *rtt_ns = slot->rtt_ns;
            if (deadline_expired)
                *deadline_expired = slot->deadline_ns &&
                    slot->received_ns >= slot->deadline_ns;
            memset(slot, 0, sizeof(*slot));
            return 1;
        }
    }
    return 0;
}

static inline int hop_xfer_ack_window_take(hop_xfer_ack_window *window,
                                            uint32_t batch, uint32_t id,
                                            int *status, uint64_t *rtt_ns) {
    return hop_xfer_ack_window_take_ex(window,batch,id,status,rtt_ns,NULL);
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
