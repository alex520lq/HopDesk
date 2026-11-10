#include "xfer_sender_threads.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#define HOP_MAC_SENDER_JOIN_TIMEOUT_SECONDS 3

static void *sender_thread_trampoline(void *opaque) {
    hop_mac_sender_thread_control *control =
        (hop_mac_sender_thread_control *)opaque;
    void *result = control->entry(control->argument);
    hop_mac_sender_threads *threads = control->owner;
    pthread_mutex_lock(&threads->exit_lock);
    control->exited = 1;
    pthread_cond_broadcast(&threads->exit_cv);
    pthread_mutex_unlock(&threads->exit_lock);
    return result;
}

static int sender_thread_wait_exited(
    hop_mac_sender_threads *threads,
    hop_mac_sender_thread_control *control,
    const struct timespec *deadline) {
    if (pthread_mutex_lock(&threads->exit_lock) != 0) return 0;
    while (!control->exited) {
        int result = pthread_cond_timedwait(
            &threads->exit_cv, &threads->exit_lock, deadline);
        if (result == ETIMEDOUT || result != 0) {
            pthread_mutex_unlock(&threads->exit_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&threads->exit_lock);
    return 1;
}

int hop_mac_sender_threads_start(
    hop_mac_sender_threads *threads,
    void *(*sender_entry)(void *),
    void *(*connection_entry)(void *),
    void *argument) {
    if (!threads || !sender_entry || !connection_entry ||
        threads->sender_started || threads->connection_started ||
        threads->exit_sync_initialized)
        return 0;

    memset(threads, 0, sizeof(*threads));
    if (pthread_mutex_init(&threads->exit_lock, NULL) != 0) return 0;
    if (pthread_cond_init(&threads->exit_cv, NULL) != 0) {
        pthread_mutex_destroy(&threads->exit_lock);
        memset(threads, 0, sizeof(*threads));
        return 0;
    }
    threads->exit_sync_initialized = 1;

    threads->sender_control.owner = threads;
    threads->sender_control.entry = sender_entry;
    threads->sender_control.argument = argument;
    threads->connection_control.owner = threads;
    threads->connection_control.entry = connection_entry;
    threads->connection_control.argument = argument;

    if (pthread_create(&threads->sender_thread, NULL,
                       sender_thread_trampoline,
                       &threads->sender_control) != 0) {
        pthread_cond_destroy(&threads->exit_cv);
        pthread_mutex_destroy(&threads->exit_lock);
        memset(threads, 0, sizeof(*threads));
        return 0;
    }
    threads->sender_started = 1;

    if (pthread_create(&threads->connection_thread, NULL,
                       sender_thread_trampoline,
                       &threads->connection_control) != 0)
        return 0; /* Preserve the running sender and its join state. */

    threads->connection_started = 1;
    return 1;
}

static int sender_thread_join_one(
    pthread_t *thread,
    int *started,
    hop_mac_sender_threads *threads,
    hop_mac_sender_thread_control *control,
    const struct timespec *deadline) {
    if (!*started) return 1;
    if (!sender_thread_wait_exited(threads, control, deadline)) return 0;
    if (pthread_join(*thread, NULL) != 0) return 0;
    *started = 0;
    return 1;
}

int hop_mac_sender_threads_join(hop_mac_sender_threads *threads) {
    if (!threads) return 0;
    if (!threads->exit_sync_initialized)
        return !threads->connection_started && !threads->sender_started;

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return 0;
    deadline.tv_sec += HOP_MAC_SENDER_JOIN_TIMEOUT_SECONDS;

    int connection_joined = sender_thread_join_one(
        &threads->connection_thread, &threads->connection_started,
        threads, &threads->connection_control, &deadline);
    int sender_joined = sender_thread_join_one(
        &threads->sender_thread, &threads->sender_started,
        threads, &threads->sender_control, &deadline);

    if (!connection_joined || !sender_joined ||
        threads->connection_started || threads->sender_started)
        return 0;

    pthread_cond_destroy(&threads->exit_cv);
    pthread_mutex_destroy(&threads->exit_lock);
    threads->exit_sync_initialized = 0;
    memset(&threads->connection_control, 0, sizeof(threads->connection_control));
    memset(&threads->sender_control, 0, sizeof(threads->sender_control));
    return 1;
}
