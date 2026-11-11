#include "xfer_ack_coordinator.h"

#include <windows.h>
#include <stdlib.h>

struct hop_xfer_ack_coordinator {
    CRITICAL_SECTION lock;
    HANDLE event;
    hop_xfer_ack_window window;
};

hop_xfer_ack_coordinator *hop_xfer_ack_coordinator_create(void) {
    hop_xfer_ack_coordinator *c = (hop_xfer_ack_coordinator *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    InitializeCriticalSection(&c->lock);
    c->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!c->event) {
        DeleteCriticalSection(&c->lock);
        free(c);
        return NULL;
    }
    hop_xfer_ack_window_reset(&c->window);
    return c;
}

void hop_xfer_ack_coordinator_destroy(hop_xfer_ack_coordinator *c) {
    if (!c) return;
    if (c->event) CloseHandle(c->event);
    DeleteCriticalSection(&c->lock);
    free(c);
}

int hop_xfer_ack_coordinator_register(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id) {
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    int ok = hop_xfer_ack_window_register(&c->window, b, id);
    LeaveCriticalSection(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_mark_sent(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, uint64_t ns) {
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    int ok = hop_xfer_ack_window_mark_sent(&c->window, b, id, ns);
    LeaveCriticalSection(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_set_deadline(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, uint64_t ns) {
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    int ok = hop_xfer_ack_window_set_deadline(&c->window, b, id, ns);
    LeaveCriticalSection(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_deliver(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, int status, uint64_t ns) {
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    int ok = hop_xfer_ack_window_deliver(&c->window, b, id, status, ns);
    LeaveCriticalSection(&c->lock);
    if (ok) SetEvent(c->event);
    return ok;
}

int hop_xfer_ack_coordinator_take(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id,
                                  int *status, uint64_t *rtt, int *expired) {
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    int ok = hop_xfer_ack_window_take_ex(&c->window, b, id, status, rtt, expired);
    LeaveCriticalSection(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_wait_take(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id,
                                       uint32_t timeout_ms, int *status, uint64_t *rtt, int *expired) {
    if (!c) return 0;
    if (hop_xfer_ack_coordinator_take(c, b, id, status, rtt, expired)) return 1;
    if (WaitForSingleObject(c->event, timeout_ms) != WAIT_OBJECT_0) return 0;
    return hop_xfer_ack_coordinator_take(c, b, id, status, rtt, expired);
}

void hop_xfer_ack_coordinator_unregister(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id) {
    if (!c) return;
    EnterCriticalSection(&c->lock);
    hop_xfer_ack_window_unregister(&c->window, b, id);
    LeaveCriticalSection(&c->lock);
}

void hop_xfer_ack_coordinator_unregister_batch(hop_xfer_ack_coordinator *c, uint32_t b) {
    if (!c) return;
    EnterCriticalSection(&c->lock);
    hop_xfer_ack_window_unregister_batch(&c->window, b);
    LeaveCriticalSection(&c->lock);
}

void hop_xfer_ack_coordinator_wake(hop_xfer_ack_coordinator *c) {
    if (c) SetEvent(c->event);
}
