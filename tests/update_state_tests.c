#include "update_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static volatile LONG stop,working;
#define INC(p) InterlockedIncrement(p)
#define DEC(p) InterlockedDecrement(p)
#define READ(p) InterlockedCompareExchange(p,0,0)
#define PAUSE() Sleep(0)
static DWORD WINAPI producer(void *unused) {
#else
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
static _Atomic int stop,working;
#define INC(p) atomic_fetch_add(p,1)
#define DEC(p) atomic_fetch_sub(p,1)
#define READ(p) atomic_load(p)
#define PAUSE() sched_yield()
static void *producer(void *unused) {
#endif
    (void)unused;
    while (!READ(&stop)) {
        if (hop_update_work_enter()) {
            INC(&working); PAUSE(); DEC(&working);
            hop_update_work_leave();
        }
    }
    return 0;
}
static int idle(void) {
    assert(!hop_update_work_enter()); /* admission closes BEFORE busy inspection */
    assert(!READ(&working));
    return 0;
}
static int busy(void) { return 1; }
#if defined(_WIN32)
static volatile LONG frame_attempted,frame_done;
static HANDLE reader;

#else
static _Atomic int frame_attempted,frame_done;
static pthread_t reader;

#endif
static int probe_result;
static int frame_entered;
#if defined(_WIN32)
static DWORD WINAPI record_receive_probe(void *unused) {
#else
static void *record_receive_probe(void *unused) {
#endif
    /* Separate entry records result without changing the production gate. */
    (void)unused;
    INC(&frame_attempted);
    frame_entered=hop_update_receive_enter();
    if (frame_entered) hop_update_work_leave();
    INC(&frame_done);
    return 0;
}
static int probe_with_waiting_frame(void) {
    assert(!READ(&frame_done));
#if defined(_WIN32)
    reader=CreateThread(NULL,0,record_receive_probe,NULL,0,NULL);assert(reader);
#else
    assert(!pthread_create(&reader,NULL,record_receive_probe,NULL));
#endif
    while(!READ(&frame_attempted)) PAUSE();
    for(int i=0;i<1000;i++) PAUSE();
    assert(!READ(&frame_done)); /* never reject/disconnect during a probe */
    return probe_result;
}
static void join_reader(void) {
#if defined(_WIN32)
    assert(WaitForSingleObject(reader,5000)==WAIT_OBJECT_0);CloseHandle(reader);
#else
    pthread_join(reader,NULL);
#endif
    DEC(&frame_attempted);DEC(&frame_done);
}

int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--ready")) {
        hop_update_report_ready(argv[2]); return 0;
    }
    assert(hop_update_work_enter());
    assert(!hop_update_install_begin(idle)); /* directory scan/frame in flight */
    hop_update_work_leave();
    assert(!hop_update_install_begin(busy)); /* queues/active receive */
    assert(hop_update_work_enter()); hop_update_work_leave(); /* failure reopens */
    assert(!hop_update_install_begin(NULL));
    assert(hop_update_install_begin(idle));
    assert(!hop_update_install_begin(idle));
    assert(!hop_update_work_enter());
    hop_update_install_end();
    probe_result=1;assert(!hop_update_install_begin(probe_with_waiting_frame));
    join_reader();assert(frame_entered); /* busy => old receive proceeds */
    probe_result=0;assert(hop_update_install_begin(probe_with_waiting_frame));
    join_reader();assert(!frame_entered); /* idle => committed maintenance rejects */
    hop_update_install_end();
#if defined(_WIN32)
    HANDLE thread=CreateThread(NULL,0,producer,NULL,0,NULL); assert(thread);
#else
    pthread_t thread; assert(!pthread_create(&thread,NULL,producer,NULL));
#endif
    for (int i=0;i<20000;i++) {
        if (hop_update_install_begin(idle)) {
            assert(!READ(&working)); PAUSE(); assert(!READ(&working));
            hop_update_install_end();
        }
    }
    INC(&stop);
#if defined(_WIN32)
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
#else
    pthread_join(thread,NULL);
#endif
    puts("PASS: real maintenance gate blocks scans, new frames and concurrent submissions");
    return 0;
}
