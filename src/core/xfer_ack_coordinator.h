#ifndef HOP_XFER_ACK_COORDINATOR_H
#define HOP_XFER_ACK_COORDINATOR_H

#include <stdint.h>

#include "xfer_ack_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hop_xfer_ack_coordinator hop_xfer_ack_coordinator;

/* Owns the platform synchronization primitive and the ACK window lifecycle. */
hop_xfer_ack_coordinator *hop_xfer_ack_coordinator_create(void);
void hop_xfer_ack_coordinator_destroy(hop_xfer_ack_coordinator *coordinator);

int hop_xfer_ack_coordinator_register(hop_xfer_ack_coordinator *coordinator,
                                      uint32_t batch, uint32_t id);
int hop_xfer_ack_coordinator_mark_sent(hop_xfer_ack_coordinator *coordinator,
                                       uint32_t batch, uint32_t id,
                                       uint64_t sent_ns);
int hop_xfer_ack_coordinator_set_deadline(hop_xfer_ack_coordinator *coordinator,
                                          uint32_t batch, uint32_t id,
                                          uint64_t deadline_ns);
int hop_xfer_ack_coordinator_deliver(hop_xfer_ack_coordinator *coordinator,
                                     uint32_t batch, uint32_t id, int status,
                                     uint64_t received_ns);
int hop_xfer_ack_coordinator_take(hop_xfer_ack_coordinator *coordinator,
                                  uint32_t batch, uint32_t id, int *status,
                                  uint64_t *rtt_ns, int *deadline_expired);

/* Atomically checks the requested slot and waits for a notification.
 * Returns 1 when that ACK was taken, 0 on timeout/unrelated notification. */
int hop_xfer_ack_coordinator_wait_take(hop_xfer_ack_coordinator *coordinator,
                                       uint32_t batch, uint32_t id,
                                       uint32_t timeout_ms, int *status,
                                       uint64_t *rtt_ns, int *deadline_expired);

void hop_xfer_ack_coordinator_unregister(hop_xfer_ack_coordinator *coordinator,
                                         uint32_t batch, uint32_t id);
void hop_xfer_ack_coordinator_unregister_batch(hop_xfer_ack_coordinator *coordinator,
                                               uint32_t batch);
void hop_xfer_ack_coordinator_wake(hop_xfer_ack_coordinator *coordinator);

#ifdef __cplusplus
}
#endif
#endif
