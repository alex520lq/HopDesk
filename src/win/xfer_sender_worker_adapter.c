#include <winsock2.h>
#include <windows.h>
#include "xfer_sender_internal.h"
#include "../core/xfer_sender_worker.h"
#include "../core/xfer_sender_scheduler.h"
#include <string.h>

typedef struct { int unused; } sender_scheduler_state;
static int sender_scheduler_begin_pass(void *user,int *peer_ready,uint64_t *now,uint32_t *cancel_batch){
    (void)user;if(!peer_ready||!now||!cancel_batch)return 0;EnterCriticalSection(&s_x.lock);
    *peer_ready=s_x.peer_ready&&s_x.client!=INVALID_SOCKET;*now=now_ms();*cancel_batch=s_x.cancel_batch;return 1;
}
static int sender_scheduler_is_stopping(void *user) {
    (void)user;
    return sender_context_is_stopping(&s_sender_platform);
}
static int sender_scheduler_wait_locked(void *user, uint32_t timeout_ms) {
    (void)user;
    HANDLE events[2]={s_x.stop,s_x.q_evt};
    LeaveCriticalSection(&s_x.lock);
    DWORD result=WaitForMultipleObjects(2,events,FALSE,timeout_ms);
    if (result==WAIT_FAILED) {
        hop_log(HOP_LOG_WARN,"xfer: sender queue wait failed; falling back to bounded poll");
        Sleep(timeout_ms);
    }
    EnterCriticalSection(&s_x.lock);
    return 1;
}
static void sender_scheduler_claim_locked(void *user, void *node, int expired) {
    (void)user;
    xfer_job *job=(xfer_job *)node;
    job->owner=XFER_JOB_OWNER_WORKER;
    if (!expired && !hop_xfer_sender_active_job_begin(
            &s_x.active_job,job->batch,job->task.id))
        hop_log(HOP_LOG_ERROR,"xfer: failed to publish active Sender Job (batch=%u task=%llu)",
                job->batch,(unsigned long long)job->task.id);
}
static void sender_scheduler_end_pass(void *user) {
    (void)user;
    LeaveCriticalSection(&s_x.lock);
}


static int sender_worker_view_job_adapter(void *u,void *p,hop_xfer_sender_worker_job_view *v){
    (void)u;xfer_job *j=p;if(!j||!v)return 0;v->platform_job=j;v->context=&j->sender_context;
    v->task=&j->task;v->session=&j->session;v->next_index=&j->next_i;v->ok_count=&j->ok;
    v->pending_finish_state=&j->pending_finish_state;v->pending_finish_valid=&j->pending_finish_valid;
    v->pending_status_end=&j->pending_status_end;v->pending_status_phase=&j->pending_status_phase;
    v->pending_status_detail=j->pending_status_detail;v->pending_status_detail_capacity=sizeof(j->pending_status_detail);
    v->batch=j->batch;return 1;
}
static int sender_worker_snapshot_connection(void *u,intptr_t *socket,uint64_t *epoch,hop_session_id *session,int *connected){
    (void)u;if(!socket||!epoch||!session||!connected)return 0;EnterCriticalSection(&s_x.lock);
    SOCKET c=s_x.peer_ready?s_x.client:INVALID_SOCKET;*session=s_x.session;*epoch=(uint64_t)s_x.epoch;s_send_epoch=s_x.epoch;
    LeaveCriticalSection(&s_x.lock);*socket=(intptr_t)c;*connected=c!=INVALID_SOCKET;return 1;
}
static int sender_worker_status_should_start(void *u,uint32_t batch){
    (void)u;AcquireSRWLockShared(&s_status_lock);int show=!s_send_status.status.active||s_send_status.status.batch==batch;
    ReleaseSRWLockShared(&s_status_lock);return show;
}
static void sender_worker_status_reset(void *u){
    (void)u;AcquireSRWLockExclusive(&s_status_lock);memset(&s_send_status,0,sizeof(s_send_status));ReleaseSRWLockExclusive(&s_status_lock);
}
static void sender_worker_clear_active_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;EnterCriticalSection(&s_x.lock);(void)hop_xfer_sender_active_job_clear_if_matches(&s_x.active_job,batch,task);
    LeaveCriticalSection(&s_x.lock);
}
static void sender_worker_send_abort_adapter(void *u,const hop_xfer_sender_context *ctx,intptr_t socket,uint64_t epoch,uint32_t batch){
    (void)u;(void)epoch;if(socket!=(intptr_t)INVALID_SOCKET)
        send_abort_msg_with_context(ctx,(SOCKET)socket,batch,HOP_ABORT_CANCEL,HOP_ABORT_TO_RECEIVER);
}
static hop_xfer_sender_job_result sender_worker_run_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,uint8_t *tx){
    (void)u;(void)epoch;return (hop_xfer_sender_job_result)run_job((SOCKET)socket,tx,(xfer_job *)job);
}
static int sender_worker_take_peer_cancelled_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;EnterCriticalSection(&s_x.lock);int c=hop_xfer_sender_active_job_take_cancelled_and_clear(
        &s_x.active_job,batch,task,HOP_ABORT_CANCEL);LeaveCriticalSection(&s_x.lock);return c;
}
static hop_xfer_sender_worker_action sender_worker_finish_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,
    hop_xfer_sender_job_result result,int completion,int peer_cancelled){
    (void)u;(void)epoch;return sender_worker_finish_job((xfer_job *)job,(SOCKET)socket,(int)result,completion,peer_cancelled);
}
static void sender_worker_release_job_adapter(void *u,void *job){(void)u;(void)job_free((xfer_job *)job,XFER_JOB_OWNER_WORKER);}

DWORD WINAPI send_thread(LPVOID arg){
    (void)arg;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    sender_scheduler_state state;memset(&state,0,sizeof(state));
    hop_xfer_sender_scheduler_ops scheduler={&state,sender_scheduler_begin_pass,sender_scheduler_is_stopping,
        sender_scheduler_wait_locked,sender_scheduler_claim_locked,sender_scheduler_end_pass};
    hop_xfer_sender_worker_runtime_ops worker;memset(&worker,0,sizeof(worker));
    worker.user=&state;worker.queue=&s_x.queue;worker.queue_ops=&s_sender_queue_ops;worker.scheduler=scheduler;
    worker.poll_interval_ms=200;worker.chunk_size=s_x.chunk;worker.view_job=sender_worker_view_job_adapter;
    worker.is_stopping=sender_scheduler_is_stopping;worker.snapshot_connection=sender_worker_snapshot_connection;
    worker.status_should_start=sender_worker_status_should_start;worker.status_reset=sender_worker_status_reset;
    worker.clear_active=sender_worker_clear_active_adapter;worker.send_abort=sender_worker_send_abort_adapter;
    worker.run_job=sender_worker_run_job_adapter;worker.take_peer_cancelled_and_clear=sender_worker_take_peer_cancelled_adapter;
    worker.finish_job=sender_worker_finish_job_adapter;worker.release_job=sender_worker_release_job_adapter;
    return hop_xfer_sender_worker_run(&worker)==HOP_XFER_SENDER_WORKER_RUN_STOPPED?0:1;
}
