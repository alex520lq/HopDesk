#include "xfer_sender_ack_adapter.h"
#include "xfer_ack_coordinator.h"

#include <stdlib.h>

struct hop_xfer_sender_ack_adapter {
    hop_xfer_ack_coordinator *coordinator;
    hop_xfer_sender_ack_clock_fn now_ns;
    void *clock_user;
};

hop_xfer_sender_ack_adapter *hop_xfer_sender_ack_adapter_create(
    hop_xfer_sender_ack_clock_fn now_ns, void *clock_user) {
    if (!now_ns) return NULL;
    hop_xfer_sender_ack_adapter *adapter =
        (hop_xfer_sender_ack_adapter *)calloc(1, sizeof(*adapter));
    if (!adapter) return NULL;
    adapter->coordinator = hop_xfer_ack_coordinator_create();
    if (!adapter->coordinator) {
        free(adapter);
        return NULL;
    }
    adapter->now_ns = now_ns;
    adapter->clock_user = clock_user;
    return adapter;
}

void hop_xfer_sender_ack_adapter_destroy(hop_xfer_sender_ack_adapter *adapter) {
    if (!adapter) return;
    hop_xfer_ack_coordinator_destroy(adapter->coordinator);
    adapter->coordinator = NULL;
    free(adapter);
}

int hop_xfer_sender_ack_adapter_register(void *user, uint32_t batch, uint32_t id) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    return adapter && adapter->coordinator &&
           hop_xfer_ack_coordinator_register(adapter->coordinator, batch, id);
}

int hop_xfer_sender_ack_adapter_mark_sent(void *user, uint32_t batch, uint32_t id) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    return adapter && adapter->coordinator && adapter->now_ns &&
           hop_xfer_ack_coordinator_mark_sent(adapter->coordinator, batch, id,
                                               adapter->now_ns(adapter->clock_user));
}

int hop_xfer_sender_ack_adapter_set_deadline(void *user, uint32_t batch,
                                              uint32_t id, uint64_t deadline_ns) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    return adapter && adapter->coordinator &&
           hop_xfer_ack_coordinator_set_deadline(adapter->coordinator, batch, id,
                                                  deadline_ns);
}

int hop_xfer_sender_ack_adapter_wait_take(void *user, uint32_t batch, uint32_t id,
                                           uint32_t timeout_ms, int *status,
                                           uint64_t *rtt_ns, int *deadline_expired) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    return adapter && adapter->coordinator &&
           hop_xfer_ack_coordinator_wait_take(adapter->coordinator, batch, id,
                                               timeout_ms, status, rtt_ns,
                                               deadline_expired);
}

void hop_xfer_sender_ack_adapter_unregister(void *user, uint32_t batch, uint32_t id) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    if (adapter && adapter->coordinator)
        hop_xfer_ack_coordinator_unregister(adapter->coordinator, batch, id);
}

void hop_xfer_sender_ack_adapter_unregister_batch(void *user, uint32_t batch) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    if (adapter && adapter->coordinator)
        hop_xfer_ack_coordinator_unregister_batch(adapter->coordinator, batch);
}

int hop_xfer_sender_ack_adapter_deliver(void *user, uint32_t batch,
                                         uint32_t id, int status) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    return adapter && adapter->coordinator && adapter->now_ns &&
           hop_xfer_ack_coordinator_deliver(adapter->coordinator, batch, id,
                                             status,
                                             adapter->now_ns(adapter->clock_user));
}

void hop_xfer_sender_ack_adapter_wake(void *user) {
    hop_xfer_sender_ack_adapter *adapter = (hop_xfer_sender_ack_adapter *)user;
    if (adapter && adapter->coordinator)
        hop_xfer_ack_coordinator_wake(adapter->coordinator);
}
