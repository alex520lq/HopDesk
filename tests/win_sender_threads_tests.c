#include "../src/win/xfer_sender_threads.h"

#include <assert.h>

static DWORD WINAPI count_thread(LPVOID user) {
    LONG *count = (LONG *)user;
    InterlockedIncrement(count);
    return 0;
}

static DWORD WINAPI blocked_thread(LPVOID user) {
    HANDLE gate=(HANDLE)user;
    (void)WaitForSingleObject(gate,INFINITE);
    return 0;
}

int main(void) {
    hop_win_sender_threads threads = {0};
    LONG count = 0;
    int joined = 0;

    assert(hop_win_sender_threads_start(
        &threads, count_thread, count_thread, &count));
    assert(threads.listener_thread != NULL);
    assert(threads.sender_thread != NULL);
    assert(hop_win_sender_threads_join(&threads, 5000, &joined));
    assert(joined);
    assert(count == 2);
    assert(threads.listener_thread == NULL);
    assert(threads.sender_thread == NULL);

    assert(hop_win_sender_threads_join(&threads, 0, &joined));
    assert(joined);

    /* A successful per-thread Join must be retained when its peer times out.
     * The next Shutdown attempt may only join/close the still-live handle. */
    HANDLE partial_gate=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(partial_gate!=NULL);
    assert(hop_win_sender_threads_start(&threads,count_thread,blocked_thread,partial_gate));
    assert(WaitForSingleObject(threads.listener_thread,5000)==WAIT_OBJECT_0);
    assert(!hop_win_sender_threads_join(&threads,0,&joined));
    assert(!joined);
    assert(threads.listener_thread==NULL&&threads.sender_thread!=NULL);
    SetEvent(partial_gate);
    assert(hop_win_sender_threads_join(&threads,5000,&joined));
    assert(joined);
    assert(threads.listener_thread==NULL&&threads.sender_thread==NULL);
    CloseHandle(partial_gate);

    /* If both workers time out, both native handles remain available to retry. */
    HANDLE gate=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(gate!=NULL);
    assert(hop_win_sender_threads_start(&threads,blocked_thread,blocked_thread,gate));
    assert(!hop_win_sender_threads_join(&threads,0,&joined));
    assert(!joined);
    assert(threads.listener_thread!=NULL&&threads.sender_thread!=NULL);
    SetEvent(gate);
    assert(hop_win_sender_threads_join(&threads,5000,&joined));
    assert(joined);
    assert(threads.listener_thread==NULL&&threads.sender_thread==NULL);
    CloseHandle(gate);
    return 0;
}
