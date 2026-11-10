#include "xfer_ack_coordinator.h"

#include <assert.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
typedef struct { hop_xfer_ack_coordinator *coordinator; } delivery_args;
static DWORD WINAPI deliver_later(LPVOID value) {
    delivery_args *args = (delivery_args *)value;
    Sleep(20);
    assert(hop_xfer_ack_coordinator_deliver(args->coordinator, 10, 1, 0, 650));
    return 0;
}
#else
#include <pthread.h>
#include <time.h>
typedef struct { hop_xfer_ack_coordinator *coordinator; } delivery_args;
static void *deliver_later(void *value) {
    delivery_args *args = (delivery_args *)value;
    struct timespec pause = {0, 20000000L};
    nanosleep(&pause, NULL);
    assert(hop_xfer_ack_coordinator_deliver(args->coordinator, 10, 1, 0, 650));
    return NULL;
}
#endif

int main(void) {
    hop_xfer_ack_coordinator *c = hop_xfer_ack_coordinator_create();
    assert(c != NULL);

    int status = -1, expired = 0;
    uint64_t rtt = 0;

    /* ACK may arrive before the sender installs the post-FILE_END deadline. */
    assert(hop_xfer_ack_coordinator_register(c, 7, 1));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 7, 1, 100));
    assert(hop_xfer_ack_coordinator_deliver(c, 7, 1, 0, 150));
    assert(hop_xfer_ack_coordinator_set_deadline(c, 7, 1, 200));
    assert(hop_xfer_ack_coordinator_wait_take(c, 7, 1, 0, &status, &rtt, &expired));
    assert(status == 0 && rtt == 50 && expired == 0);

    /* The receive timestamp, not the time the sender wakes, decides timeout. */
    assert(hop_xfer_ack_coordinator_register(c, 7, 2));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 7, 2, 300));
    assert(hop_xfer_ack_coordinator_set_deadline(c, 7, 2, 350));
    assert(hop_xfer_ack_coordinator_deliver(c, 7, 2, 0, 360));
    status = -1; expired = 0; rtt = 0;
    assert(hop_xfer_ack_coordinator_wait_take(c, 7, 2, 0, &status, &rtt, &expired));
    assert(status == 0 && rtt == 60 && expired == 1);

    /* Duplicate and stale ACKs cannot overwrite a reused (batch,id) slot. */
    assert(!hop_xfer_ack_coordinator_deliver(c, 7, 2, 1, 400));
    assert(hop_xfer_ack_coordinator_register(c, 8, 3));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 8, 3, 500));
    hop_xfer_ack_coordinator_unregister_batch(c, 8);
    assert(!hop_xfer_ack_coordinator_deliver(c, 8, 3, 0, 550));
    assert(hop_xfer_ack_coordinator_register(c, 8, 3));

    /* Delivery from another thread must wake a waiter without a lost signal. */
    assert(hop_xfer_ack_coordinator_register(c, 10, 1));
    assert(hop_xfer_ack_coordinator_mark_sent(c, 10, 1, 600));
    assert(hop_xfer_ack_coordinator_set_deadline(c, 10, 1, 700));
    delivery_args args = {c};
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, deliver_later, &args, 0, NULL);
    assert(thread != NULL);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, deliver_later, &args) == 0);
#endif
    status = -1; expired = 0; rtt = 0;
    assert(hop_xfer_ack_coordinator_wait_take(c, 10, 1, 1000, &status, &rtt, &expired));
    assert(status == 0 && rtt == 50 && expired == 0);
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread, NULL) == 0);
#endif

    /* All slots are bounded and batch cleanup releases every reservation. */
    assert(hop_xfer_ack_coordinator_register(c, 8, 4));
    assert(hop_xfer_ack_coordinator_register(c, 8, 5));
    assert(hop_xfer_ack_coordinator_register(c, 8, 6));
    assert(!hop_xfer_ack_coordinator_register(c, 8, 7));
    hop_xfer_ack_coordinator_unregister_batch(c, 8);
    assert(hop_xfer_ack_coordinator_register(c, 9, 1));

    hop_xfer_ack_coordinator_destroy(c);
    puts("PASS: platform ACK coordinator lifecycle, early/late ACK, stale ACK and bounded slots");
    return 0;
}
