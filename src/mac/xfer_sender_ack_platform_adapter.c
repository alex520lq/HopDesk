#include "xfer_sender_internal.h"
#include "../core/xfer_sender_ack_adapter.h"

static uint64_t sender_ack_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}

int sender_context_ack_create(void) {
    if (s_sender_platform.ack_adapter) return 0;
    hop_xfer_sender_ack_adapter *adapter =
        hop_xfer_sender_ack_adapter_create(sender_ack_now_ns, NULL);
    if (!adapter) return 0;
    s_sender_platform.ack_adapter = adapter;
    s_sender_context.ack_user = adapter;
    return 1;
}

void sender_context_ack_destroy(void) {
    hop_xfer_sender_ack_adapter_destroy(
        (hop_xfer_sender_ack_adapter *)s_sender_platform.ack_adapter);
    s_sender_platform.ack_adapter = NULL;
    s_sender_context.ack_user = NULL;
}

void sender_context_wake_ack(void) {
    if (s_sender_context.ack_wake && s_sender_context.ack_user)
        s_sender_context.ack_wake(s_sender_context.ack_user);
}

void ack_deliver(uint32_t batch, uint32_t id, int status) {
    if (s_sender_context.ack_user && s_sender_context.ack_deliver)
        (void)s_sender_context.ack_deliver(s_sender_context.ack_user,
                                           batch, id, status);
}
