#include "xfer_sender_threads.h"

int hop_win_sender_threads_start(
    hop_win_sender_threads *threads,
    LPTHREAD_START_ROUTINE listener_entry,
    LPTHREAD_START_ROUTINE sender_entry,
    void *argument) {
    if (!threads || !listener_entry || !sender_entry ||
        threads->listener_thread || threads->sender_thread)
        return 0;

    threads->listener_thread = CreateThread(
        NULL, 0, listener_entry, argument, 0, NULL);
    if (!threads->listener_thread) return 0;

    threads->sender_thread = CreateThread(
        NULL, 0, sender_entry, argument, 0, NULL);
    return threads->sender_thread != NULL;
}

int hop_win_sender_threads_join(
    hop_win_sender_threads *threads, DWORD timeout_ms, int *all_joined) {
    if (!threads) {
        if (all_joined) *all_joined = 0;
        return 0;
    }

    int joined = 1;
    if (threads->listener_thread) {
        if (WaitForSingleObject(threads->listener_thread, timeout_ms) == WAIT_OBJECT_0) {
            CloseHandle(threads->listener_thread);
            threads->listener_thread = NULL;
        } else {
            /* A timeout is not a join. Keep the handle so Shutdown can retry
             * after the worker observes stop and leaves its current Job. */
            joined = 0;
        }
    }
    if (threads->sender_thread) {
        if (WaitForSingleObject(threads->sender_thread, timeout_ms) == WAIT_OBJECT_0) {
            CloseHandle(threads->sender_thread);
            threads->sender_thread = NULL;
        } else {
            joined = 0;
        }
    }

    if (all_joined) *all_joined = joined;
    return joined;
}
