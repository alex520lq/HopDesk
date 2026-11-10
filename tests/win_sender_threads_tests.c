#include "../src/win/xfer_sender_threads.h"

#include <assert.h>

static DWORD WINAPI count_thread(LPVOID user) {
    LONG *count = (LONG *)user;
    InterlockedIncrement(count);
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
    return 0;
}
