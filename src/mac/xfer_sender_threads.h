#ifndef HOP_MAC_XFER_SENDER_THREADS_H
#define HOP_MAC_XFER_SENDER_THREADS_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_t connection_thread;
    pthread_t sender_thread;
    int connection_started;
    int sender_started;
} hop_mac_sender_threads;

/* Starts sender first, then connection thread. If the second start fails,
 * sender_started remains set and the caller must signal stop before join. */
int hop_mac_sender_threads_start(
    hop_mac_sender_threads *threads,
    void *(*sender_entry)(void *),
    void *(*connection_entry)(void *),
    void *argument);

/* Joins connection first, then sender. A failed join leaves its started flag
 * set so the caller can avoid destroying state still potentially in use. */
int hop_mac_sender_threads_join(hop_mac_sender_threads *threads);

#ifdef __cplusplus
}
#endif
#endif
