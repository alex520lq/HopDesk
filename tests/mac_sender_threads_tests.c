#include "../src/mac/xfer_sender_threads.h"

#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>

static void *count_thread(void *user) {
    atomic_int *count = (atomic_int *)user;
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    return NULL;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int released;
    atomic_int returned;
} gate_context;

static void *count_gate_thread(void *user) {
    gate_context *context = (gate_context *)user;
    atomic_fetch_add_explicit(&context->returned, 1, memory_order_relaxed);
    return NULL;
}

static void *blocked_thread(void *user) {
    gate_context *context = (gate_context *)user;
    pthread_mutex_lock(&context->lock);
    while (!context->released)
        pthread_cond_wait(&context->cv, &context->lock);
    pthread_mutex_unlock(&context->lock);
    atomic_fetch_add_explicit(&context->returned, 1, memory_order_relaxed);
    return NULL;
}

static void test_timeout_preserves_join_state(void) {
    hop_mac_sender_threads threads = {0};
    gate_context context;
    assert(pthread_mutex_init(&context.lock, NULL) == 0);
    assert(pthread_cond_init(&context.cv, NULL) == 0);
    context.released = 0;
    atomic_init(&context.returned, 0);

    assert(hop_mac_sender_threads_start(
        &threads, count_gate_thread, blocked_thread, &context));
    assert(threads.sender_started && threads.connection_started);
    assert(threads.exit_sync_initialized);

    /* The connection thread is deliberately held. The sender may still be
     * joined, but the live connection handle and shared exit state must remain. */
    assert(!hop_mac_sender_threads_join(&threads));
    assert(!threads.sender_started && threads.connection_started);
    assert(threads.exit_sync_initialized);
    assert(atomic_load_explicit(&context.returned, memory_order_relaxed) == 1);

    pthread_mutex_lock(&context.lock);
    context.released = 1;
    pthread_cond_broadcast(&context.cv);
    pthread_mutex_unlock(&context.lock);

    assert(hop_mac_sender_threads_join(&threads));
    assert(!threads.sender_started && !threads.connection_started);
    assert(!threads.exit_sync_initialized);
    assert(atomic_load_explicit(&context.returned, memory_order_relaxed) == 2);
    assert(hop_mac_sender_threads_join(&threads));

    pthread_cond_destroy(&context.cv);
    pthread_mutex_destroy(&context.lock);
}

int main(void) {
    hop_mac_sender_threads threads = {0};
    atomic_int count;
    atomic_init(&count, 0);

    assert(hop_mac_sender_threads_start(
        &threads, count_thread, count_thread, &count));
    assert(threads.sender_started);
    assert(threads.connection_started);
    assert(hop_mac_sender_threads_join(&threads));
    assert(atomic_load_explicit(&count, memory_order_relaxed) == 2);
    assert(!threads.sender_started);
    assert(!threads.connection_started);
    assert(!threads.exit_sync_initialized);

    assert(hop_mac_sender_threads_join(&threads));
    assert(!hop_mac_sender_threads_start(&threads, NULL, count_thread, &count));

    test_timeout_preserves_join_state();
    puts("PASS: macOS Sender Threads bounded join, retry, and restart");
    return 0;
}
