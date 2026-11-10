#include "xfer_ack_coordinator.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct hop_xfer_ack_coordinator {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    hop_xfer_ack_window window;
};

hop_xfer_ack_coordinator *hop_xfer_ack_coordinator_create(void) {
    hop_xfer_ack_coordinator *c = (hop_xfer_ack_coordinator *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (pthread_mutex_init(&c->lock, NULL) != 0) { free(c); return NULL; }
    if (pthread_cond_init(&c->cond, NULL) != 0) {
        pthread_mutex_destroy(&c->lock);
        free(c);
        return NULL;
    }
    hop_xfer_ack_window_reset(&c->window);
    return c;
}

void hop_xfer_ack_coordinator_destroy(hop_xfer_ack_coordinator *c) {
    if (!c) return;
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

int hop_xfer_ack_coordinator_register(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int ok = hop_xfer_ack_window_register(&c->window, b, id);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_mark_sent(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, uint64_t ns) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int ok = hop_xfer_ack_window_mark_sent(&c->window, b, id, ns);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_set_deadline(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, uint64_t ns) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int ok = hop_xfer_ack_window_set_deadline(&c->window, b, id, ns);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_deliver(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id, int status, uint64_t ns) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int ok = hop_xfer_ack_window_deliver(&c->window, b, id, status, ns);
    if (ok) pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_take(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id,
                                  int *status, uint64_t *rtt, int *expired) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int ok = hop_xfer_ack_window_take_ex(&c->window, b, id, status, rtt, expired);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int hop_xfer_ack_coordinator_wait_take(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id,
                                       uint32_t timeout_ms, int *status, uint64_t *rtt, int *expired) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    int hit = hop_xfer_ack_window_take_ex(&c->window, b, id, status, rtt, expired);
    if (!hit && timeout_ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout_ms / 1000u);
        ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        (void)pthread_cond_timedwait(&c->cond, &c->lock, &ts);
        hit = hop_xfer_ack_window_take_ex(&c->window, b, id, status, rtt, expired);
    }
    pthread_mutex_unlock(&c->lock);
    return hit;
}

void hop_xfer_ack_coordinator_unregister(hop_xfer_ack_coordinator *c, uint32_t b, uint32_t id) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    hop_xfer_ack_window_unregister(&c->window, b, id);
    pthread_mutex_unlock(&c->lock);
}

void hop_xfer_ack_coordinator_unregister_batch(hop_xfer_ack_coordinator *c, uint32_t b) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    hop_xfer_ack_window_unregister_batch(&c->window, b);
    pthread_mutex_unlock(&c->lock);
}

void hop_xfer_ack_coordinator_wake(hop_xfer_ack_coordinator *c) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}
