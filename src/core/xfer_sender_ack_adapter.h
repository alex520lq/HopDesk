#ifndef HOP_XFER_SENDER_ACK_ADAPTER_H
#define HOP_XFER_SENDER_ACK_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hop_xfer_sender_ack_adapter hop_xfer_sender_ack_adapter;
typedef uint64_t (*hop_xfer_sender_ack_clock_fn)(void *user);

hop_xfer_sender_ack_adapter *hop_xfer_sender_ack_adapter_create(
    hop_xfer_sender_ack_clock_fn now_ns, void *clock_user);
void hop_xfer_sender_ack_adapter_destroy(hop_xfer_sender_ack_adapter *adapter);

/* Callback signatures match hop_xfer_sender_context ACK hooks; pass the
 * adapter itself as ack_user. */
int hop_xfer_sender_ack_adapter_register(void *user, uint32_t batch, uint32_t id);
int hop_xfer_sender_ack_adapter_mark_sent(void *user, uint32_t batch, uint32_t id);
int hop_xfer_sender_ack_adapter_set_deadline(void *user, uint32_t batch,
                                              uint32_t id, uint64_t deadline_ns);
int hop_xfer_sender_ack_adapter_wait_take(void *user, uint32_t batch, uint32_t id,
                                           uint32_t timeout_ms, int *status,
                                           uint64_t *rtt_ns, int *deadline_expired);
void hop_xfer_sender_ack_adapter_unregister(void *user, uint32_t batch, uint32_t id);
void hop_xfer_sender_ack_adapter_unregister_batch(void *user, uint32_t batch);
int hop_xfer_sender_ack_adapter_deliver(void *user, uint32_t batch,
                                         uint32_t id, int status);
void hop_xfer_sender_ack_adapter_wake(void *user);

#ifdef __cplusplus
}
#endif
#endif
