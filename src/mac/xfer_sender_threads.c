#include "xfer_sender_threads.h"

#include <string.h>

int hop_mac_sender_threads_start(
    hop_mac_sender_threads *threads,
    void *(*sender_entry)(void *),
    void *(*connection_entry)(void *),
    void *argument) {
    if (!threads || !sender_entry || !connection_entry ||
        threads->sender_started || threads->connection_started)
        return 0;

    memset(threads, 0, sizeof(*threads));
    if (pthread_create(&threads->sender_thread, NULL, sender_entry, argument) != 0)
        return 0;
    threads->sender_started = 1;

    if (pthread_create(&threads->connection_thread, NULL,
                       connection_entry, argument) != 0)
        return 0;
    threads->connection_started = 1;
    return 1;
}

int hop_mac_sender_threads_join(hop_mac_sender_threads *threads) {
    if (!threads) return 0;
    int joined = 1;

    if (threads->connection_started) {
        if (pthread_join(threads->connection_thread, NULL) == 0)
            threads->connection_started = 0;
        else
            joined = 0;
    }
    if (threads->sender_started) {
        if (pthread_join(threads->sender_thread, NULL) == 0)
            threads->sender_started = 0;
        else
            joined = 0;
    }
    return joined;
}
