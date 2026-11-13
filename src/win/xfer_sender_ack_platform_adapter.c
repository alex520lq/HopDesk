#include <winsock2.h>
#include <windows.h>
#include "xfer_sender_internal.h"
#include "../core/xfer_ack_coordinator.h"

int sender_context_ack_create(void) {
    if (s_sender_platform.ack_coordinator) return 0;
    hop_xfer_ack_coordinator *coordinator = hop_xfer_ack_coordinator_create();
    if (!coordinator) return 0;
    s_sender_platform.ack_coordinator = coordinator;
    s_sender_context.ack_coordinator = coordinator;
    return 1;
}

void sender_context_ack_destroy(void) {
    hop_xfer_ack_coordinator_destroy(s_sender_platform.ack_coordinator);
    s_sender_platform.ack_coordinator = NULL;
    s_sender_context.ack_coordinator = NULL;
}

void sender_context_wake_ack(void) {
    hop_xfer_ack_coordinator_wake(s_sender_platform.ack_coordinator);
}

void ack_deliver(uint32_t batch, uint32_t id, int status) {
    hop_xfer_ack_coordinator *coordinator = s_sender_platform.ack_coordinator;
    if (coordinator)
        (void)hop_xfer_ack_coordinator_deliver(coordinator, batch, id, status,
                                               rate_now_ns());
}
