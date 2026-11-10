#ifndef HOP_WIN_XFER_SENDER_THREADS_H
#define HOP_WIN_XFER_SENDER_THREADS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HANDLE listener_thread;
    HANDLE sender_thread;
} hop_win_sender_threads;

/* Starts listener first, then sender. If the second start fails, the first
 * handle remains owned by the set and must be joined after signalling stop. */
int hop_win_sender_threads_start(
    hop_win_sender_threads *threads,
    LPTHREAD_START_ROUTINE listener_entry,
    LPTHREAD_START_ROUTINE sender_entry,
    void *argument);

/* Waits each started thread for timeout_ms, then closes its handle even on
 * timeout (callers must skip shared-state cleanup if all_joined is false). */
int hop_win_sender_threads_join(
    hop_win_sender_threads *threads, DWORD timeout_ms, int *all_joined);

#ifdef __cplusplus
}
#endif
#endif
