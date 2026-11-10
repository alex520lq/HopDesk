#ifndef HOP_MAC_XFER_SENDER_THREADS_H
#define HOP_MAC_XFER_SENDER_THREADS_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hop_mac_sender_threads hop_mac_sender_threads;

typedef struct {
    hop_mac_sender_threads *owner;
    void *(*entry)(void *);
    void *argument;
    int exited;
} hop_mac_sender_thread_control;

struct hop_mac_sender_threads {
    pthread_t connection_thread;
    pthread_t sender_thread;
    int connection_started;
    int sender_started;
    pthread_mutex_t exit_lock;
    pthread_cond_t exit_cv;
    int exit_sync_initialized;
    hop_mac_sender_thread_control connection_control;
    hop_mac_sender_thread_control sender_control;
};

/* Starts sender first, then connection thread. If the second start fails,
 * sender_started remains set and the caller must signal stop before join. */
int hop_mac_sender_threads_start(
    hop_mac_sender_threads *threads,
    void *(*sender_entry)(void *),
    void *(*connection_entry)(void *),
    void *argument);

/* Waits up to three seconds total. A timeout preserves live thread handles and
 * exit synchronization so Shutdown can retry. Only successfully joined
 * threads have their started flags cleared; synchronization is destroyed only
 * after both threads have been joined. */
int hop_mac_sender_threads_join(hop_mac_sender_threads *threads);

#ifdef __cplusplus
}
#endif
#endif
