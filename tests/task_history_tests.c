/* Manager history is in-memory metadata, not an independent lifecycle owner. */
#include "xfer_task.h"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static void add_task(int direction, hop_xfer_task_state state, hop_xfer_task_id *id) {
    hop_xfer_task task={0};
    assert(hop_xfer_task_init_classified(&task,0,0,123,direction,HOP_TASK_KIND_FILES));
    assert(hop_xfer_task_reserve_job(&task));
    *id=task.id;
    assert(hop_xfer_task_finish(&task,state));
    assert(!hop_xfer_task_finish(&task,state)); /* one terminal event */
    hop_xfer_task_dispose(&task);
}
static void worker_run(void) {
    for(int i=0;i<128;i++) {
        hop_xfer_task_id id;
        add_task(i%2?HOP_XFER_DIR_SEND:HOP_XFER_DIR_RECEIVE,
                 i%3?HOP_XFER_TASK_COMPLETED:HOP_XFER_TASK_FAILED,&id);
    }
}
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID unused) { (void)unused;worker_run();return 0; }
#else
static void *worker(void *unused) { (void)unused;worker_run();return NULL; }
#endif
int main(void) {
    hop_xfer_task_snapshot out[HOP_TASK_HISTORY_CAPACITY], snap;
    hop_xfer_task_audit audit;
    hop_xfer_task_id a,b,c,internal_id; int total=0;
    assert(hop_xfer_task_history_list(NULL,0,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,&total)==0 && total==0);
    add_task(HOP_XFER_DIR_SEND,HOP_XFER_TASK_COMPLETED,&a);
    add_task(HOP_XFER_DIR_RECEIVE,HOP_XFER_TASK_CANCELLED,&b);
    add_task(HOP_XFER_DIR_SEND,HOP_XFER_TASK_FAILED,&c);
    add_task(HOP_XFER_DIR_NONE,HOP_XFER_TASK_COMPLETED,&internal_id);
    assert(!hop_xfer_task_get_snapshot(a,&snap)); /* disposed, but retained in history */
    assert(hop_xfer_task_history_list(out,2,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,&total)==2 && total==3);
    assert(out[0].id==c && out[1].id==b);
    assert(!out[0].jobs && !out[0].entries && !out[0].pending_bytes);
    assert(hop_xfer_task_history_list(out,3,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,0,&total)==3);
    assert(out[0].id==a && out[1].id==b && out[2].id==c);
    assert(hop_xfer_task_history_list(out,3,HOP_XFER_DIR_SEND,HOP_XFER_TASK_FAILED,1,&total)==1 && total==1 && out[0].id==c);
    assert(hop_xfer_task_history_list(NULL,0,HOP_XFER_DIR_RECEIVE,HOP_XFER_TASK_EMPTY,1,&total)==0 && total==1);
    assert(hop_xfer_task_history_list(NULL,1,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,NULL)==-1);
    assert(hop_xfer_task_history_list(out,1,HOP_XFER_DIR_NONE,HOP_XFER_TASK_ACTIVE,1,NULL)==-1);
    assert(hop_xfer_task_history_list(out,1,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,2,NULL)==-1);
    hop_xfer_task interrupted={0};
    assert(hop_xfer_task_init_classified(&interrupted,0,0,123,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
    hop_xfer_task_id interrupted_id=interrupted.id;
    hop_xfer_task_dispose(&interrupted); /* implicit interrupted transition */
    assert(hop_xfer_task_history_list(out,1,HOP_XFER_DIR_RECEIVE,HOP_XFER_TASK_INTERRUPTED,1,&total)==1 && out[0].id==interrupted_id);
#ifdef _WIN32
    HANDLE threads[4];
    for(int i=0;i<4;i++){threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);assert(threads[i]);}
    for(int i=0;i<4;i++){assert(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);CloseHandle(threads[i]);}
#else
    pthread_t threads[4];
    for(int i=0;i<4;i++)assert(pthread_create(&threads[i],NULL,worker,NULL)==0);
    for(int i=0;i<4;i++)assert(pthread_join(threads[i],NULL)==0);
#endif
    assert(hop_xfer_task_history_list(out,HOP_TASK_HISTORY_CAPACITY,HOP_XFER_DIR_NONE,HOP_XFER_TASK_EMPTY,1,&total)==HOP_TASK_HISTORY_CAPACITY);
    assert(total==HOP_TASK_HISTORY_CAPACITY);
    for(unsigned i=0;i<HOP_TASK_HISTORY_CAPACITY;i++) {
        assert(!out[i].jobs && !out[i].entries && !out[i].pending_bytes);
        for(unsigned j=0;j<i;j++)assert(out[i].id!=out[j].id);
    }
    assert(hop_xfer_task_reconcile(&audit) && !audit.mismatches && !audit.jobs && !audit.entries && !audit.pending_bytes);
    puts("PASS: bounded manager history, filters, ordering, eviction and concurrent ownership");
    return 0;
}
