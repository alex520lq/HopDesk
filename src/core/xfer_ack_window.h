#ifndef HOP_XFER_ACK_WINDOW_H
#define HOP_XFER_ACK_WINDOW_H

#include <stdint.h>

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

#ifdef __cplusplus
extern "C" {
#endif

void hop_xfer_ack_window_reset(hop_xfer_ack_window *window);

uint32_t hop_xfer_ack_window_count(const hop_xfer_ack_window *window);

int hop_xfer_ack_window_advance_checkpoint(int *checkpoint,
                                                          int resolved_index,
                                                          int ack_confirmed);

int hop_xfer_ack_window_register(hop_xfer_ack_window *window,
                                                uint32_t batch, uint32_t id);

int hop_xfer_ack_window_mark_sent(hop_xfer_ack_window *window,
                                                 uint32_t batch, uint32_t id,
                                                 uint64_t sent_ns);

int hop_xfer_ack_window_set_deadline(hop_xfer_ack_window *window,
                                                       uint32_t batch, uint32_t id,
                                                       uint64_t deadline_ns);

int hop_xfer_ack_window_deliver(hop_xfer_ack_window *window,
                                               uint32_t batch, uint32_t id,
                                               int status, uint64_t received_ns);

int hop_xfer_ack_window_take_ex(hop_xfer_ack_window *window,
                                               uint32_t batch, uint32_t id,
                                               int *status, uint64_t *rtt_ns,
                                               int *deadline_expired);

int hop_xfer_ack_window_take(hop_xfer_ack_window *window,
                                            uint32_t batch, uint32_t id,
                                            int *status, uint64_t *rtt_ns);

void hop_xfer_ack_window_unregister(hop_xfer_ack_window *window,
                                                   uint32_t batch, uint32_t id);

void hop_xfer_ack_window_unregister_batch(hop_xfer_ack_window *window,
                                                         uint32_t batch);

#ifdef __cplusplus
}
#endif
#endif
