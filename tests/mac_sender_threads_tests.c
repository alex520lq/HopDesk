#include "../src/mac/xfer_sender_threads.h"

#include <assert.h>
#include <stdatomic.h>

static void *count_thread(void *user) {
    atomic_int *count = (atomic_int *)user;
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    return NULL;
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

    assert(hop_mac_sender_threads_join(&threads));
    assert(!hop_mac_sender_threads_start(&threads, NULL, count_thread, &count));
    return 0;
}
