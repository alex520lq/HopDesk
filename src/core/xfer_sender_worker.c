#include "xfer_sender_worker.h"
#include "xfer_sender_context_adapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SENDER_CHUNK_HEADER_BYTES 25u
static int ops_valid(const hop_xfer_sender_worker_runtime_ops *o) {
    return o&&o->queue&&o->queue_ops&&o->poll_interval_ms&&o->chunk_size&&o->view_job&&
        o->is_stopping&&o->snapshot_connection&&o->status_should_start&&o->status_reset&&
        o->clear_active&&o->send_abort&&o->run_job&&o->take_peer_cancelled_and_clear&&
        o->finish_job&&o->release_job;
}
static int view_valid(const hop_xfer_sender_worker_job_view *v,const void *job) {
    return v&&v->platform_job==job&&v->context&&v->task&&v->task->id&&v->session&&
        v->next_index&&v->ok_count&&v->pending_finish_state&&v->pending_finish_valid&&
        v->pending_status_end&&v->pending_status_phase&&v->pending_status_detail&&
        v->pending_status_detail_capacity;
}
static void pending_terminal(hop_xfer_sender_worker_job_view *v,hop_xfer_task_state state,
    int phase,const char *detail,int publish) {
    *v->pending_finish_state=state;*v->pending_finish_valid=1;if(!publish)return;
    *v->pending_status_end=1;*v->pending_status_phase=phase;
    (void)snprintf(v->pending_status_detail,v->pending_status_detail_capacity,"%s",
                   detail?detail:"Transfer ended");
}
static int snapshot(const hop_xfer_sender_worker_runtime_ops *o,intptr_t *socket,uint64_t *epoch,
    hop_session_id *session,int *connected) {
    *socket=(intptr_t)-1;*epoch=0;memset(session,0,sizeof(*session));*connected=0;
    if(o->snapshot_connection(o->user,socket,epoch,session,connected))return 1;
    *socket=(intptr_t)-1;*epoch=0;memset(session,0,sizeof(*session));*connected=0;return 0;
}
static void release_selected(const hop_xfer_sender_worker_runtime_ops *o,
    hop_xfer_sender_worker_job_view *v,void *job) {
    if(v&&v->task&&v->task->id)o->clear_active(o->user,v->batch,v->task->id);
    o->release_job(o->user,job);
}
hop_xfer_sender_worker_run_result hop_xfer_sender_worker_run(const hop_xfer_sender_worker_runtime_ops *o) {
    if(!ops_valid(o)||(size_t)o->chunk_size>SIZE_MAX-SENDER_CHUNK_HEADER_BYTES-HOP_DATA_PREFIX) {
        hop_log(HOP_LOG_ERROR,"xfer: invalid shared Sender Worker adapter");
        return HOP_XFER_SENDER_WORKER_RUN_ERROR;
    }
    size_t bytes=SENDER_CHUNK_HEADER_BYTES+(size_t)HOP_DATA_PREFIX+(size_t)o->chunk_size;
    uint8_t *tx=(uint8_t *)malloc(bytes);
    if(!tx){hop_log(HOP_LOG_ERROR,"xfer: sender tx buffer allocation failed");return HOP_XFER_SENDER_WORKER_RUN_ERROR;}
    hop_xfer_sender_worker_run_result result=HOP_XFER_SENDER_WORKER_RUN_STOPPED;
    for(;;) {
        void *job=NULL;
        hop_xfer_sender_scheduler_result selected=hop_xfer_sender_scheduler_wait_next(
            o->queue,o->queue_ops,&o->scheduler,o->poll_interval_ms,&job);
        if(selected==HOP_XFER_SENDER_SCHEDULER_STOPPED)break;
        if(selected==HOP_XFER_SENDER_SCHEDULER_ERROR||!job) {
            hop_log(HOP_LOG_ERROR,"xfer: shared Sender scheduler failed");
            result=HOP_XFER_SENDER_WORKER_RUN_ERROR;break;
        }
        hop_xfer_sender_worker_job_view v;memset(&v,0,sizeof(v));
        if(!o->view_job(o->user,job,&v)||!view_valid(&v,job)) {
            hop_log(HOP_LOG_ERROR,"xfer: invalid Sender Job view; releasing worker-owned Job");
            o->release_job(o->user,job);continue;
        }
        if(selected==HOP_XFER_SENDER_SCHEDULER_EXPIRED) {
            intptr_t socket=(intptr_t)-1;uint64_t epoch=0;hop_session_id current;int connected=0;
            int snap=snapshot(o,&socket,&epoch,&current,&connected);
            int cancelled=hop_xfer_sender_context_job_cancel_requested(v.context,v.batch,v.task->id);
            if(snap&&cancelled&&connected&&hop_session_id_valid(&current)&&hop_session_id_equal(v.session,&current))
                o->send_abort(o->user,v.context,socket,epoch,v.batch);
            int show=o->status_should_start(o->user,v.batch);
            if(show)hop_xfer_sender_context_status_start_job(v.context,job);
            pending_terminal(&v,cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED,
                cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued",show);
            o->clear_active(o->user,v.batch,v.task->id);o->release_job(o->user,job);continue;
        }
        if(selected!=HOP_XFER_SENDER_SCHEDULER_READY) {
            hop_log(HOP_LOG_ERROR,"xfer: unexpected Sender scheduler result");
            result=HOP_XFER_SENDER_WORKER_RUN_ERROR;break;
        }
        if(o->is_stopping(o->user)){release_selected(o,&v,job);continue;}
        intptr_t socket=(intptr_t)-1;uint64_t epoch=0;hop_session_id current;int connected=0;
        int snap=snapshot(o,&socket,&epoch,&current,&connected);
        if(snap&&hop_session_id_valid(&current)&&!hop_session_id_equal(v.session,&current)) {
            *v.session=current;*v.next_index=0;*v.ok_count=0;o->status_reset(o->user);
        }
        if(hop_session_id_valid(v.session))
            (void)hop_xfer_sender_context_task_bind_session(v.context,v.task,v.session->bytes);
        if(!hop_xfer_sender_context_task_set_state(v.context,v.task,HOP_XFER_TASK_ACTIVE)) {
            o->clear_active(o->user,v.batch,v.task->id);
            pending_terminal(&v,HOP_XFER_TASK_FAILED,HOP_XFER_FAILED,"Task activation failed",0);
            o->release_job(o->user,job);continue;
        }
        hop_xfer_sender_context_status_start_job(v.context,job);
        hop_xfer_sender_job_result jr=connected?o->run_job(o->user,job,socket,epoch,tx):HOP_XFER_SENDER_JOB_NET_DOWN;
        int completion=jr==HOP_XFER_SENDER_JOB_DONE;
        int peer=completion?0:o->take_peer_cancelled_and_clear(o->user,v.batch,v.task->id);
        (void)o->finish_job(o->user,job,socket,epoch,jr,completion,peer);
    }
    free(tx);return result;
}
