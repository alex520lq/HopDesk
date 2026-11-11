#include "../src/core/xfer_sender_worker.h"
#include "../src/core/xfer_sender_context_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct fixture fixture;
typedef struct job {
    struct job *next;uint32_t batch;uint64_t retry_at,give_up_at;int cancelled,owner;
    hop_xfer_task task;hop_xfer_sender_context context;hop_session_id session;
    int next_index,ok;hop_xfer_task_state finish_state;int finish_valid,status_end,status_phase;char detail[96];
} job;
struct fixture {
    hop_xfer_sender_queue queue;int locked,peer,stopping,worker_stop,stop_after_wait,connected;
    hop_session_id session;intptr_t socket;uint64_t epoch;int cancelled,activate,status_visible;
    unsigned claims,snapshots,resets,starts,binds,state_sets,aborts,clears,releases,runs,finishes;
    int invalid_view;
};
void hop_log(hop_log_level l,const char *f,...){(void)l;(void)f;}
int hop_session_id_valid(const hop_session_id *s){if(!s)return 0;for(size_t i=0;i<HOP_SESSION_BYTES;i++)if(s->bytes[i])return 1;return 0;}
int hop_session_id_equal(const hop_session_id *a,const hop_session_id *b){return a&&b&&!memcmp(a->bytes,b->bytes,HOP_SESSION_BYTES);}
static void *qn(void *u,const void *n){(void)u;return ((const job *)n)->next;}
static void qs(void *u,void *n,void *x){(void)u;((job *)n)->next=(job *)x;}
static int qc(void *u,const void *n){(void)u;return ((const job *)n)->cancelled;}
static uint32_t qb(void *u,const void *n){(void)u;return ((const job *)n)->batch;}
static uint64_t qr(void *u,const void *n){(void)u;return ((const job *)n)->retry_at;}
static uint64_t qg(void *u,const void *n){(void)u;return ((const job *)n)->give_up_at;}
static const hop_xfer_sender_queue_ops qops={NULL,qn,qs,qc,qb,qr,qg};
static int sb(void *u,int *p,uint64_t *n,uint32_t *c){fixture *f=u;if(f->locked)return 0;f->locked=1;*p=f->peer;*n=1;*c=0;return 1;}
static int ss(void *u){fixture *f=u;assert(f->locked);return f->stopping;}
static int sw(void *u,uint32_t ms){fixture *f=u;assert(f->locked&&ms==200);if(f->stop_after_wait)f->stopping=1;return 1;}
static void sc(void *u,void *n,int e){fixture *f=u;assert(f->locked&&n);(void)e;f->claims++;((job *)n)->owner=2;}
static void se(void *u){fixture *f=u;assert(f->locked);f->locked=0;}
static int cc(void *u,uint32_t b,uint64_t id){(void)b;(void)id;return ((fixture *)u)->cancelled;}
static int cb(void *u,uint32_t b){(void)b;return ((fixture *)u)->cancelled;}
static int bind(void *u,const hop_xfer_task *t,const uint8_t *s){fixture *f=u;assert(t&&s);f->binds++;return 1;}
static int setstate(void *u,hop_xfer_task *t,int s){fixture *f=u;assert(t&&s==HOP_XFER_TASK_ACTIVE);f->state_sets++;return f->activate;}
static void start(void *u,const void *j){fixture *f=u;assert(j);f->starts++;}
static int view(void *u,void *p,hop_xfer_sender_worker_job_view *v){
    fixture *f=(fixture *)u;job *j=(job *)p;if(!j||!v)return 0;
    v->platform_job=j;v->context=&j->context;v->task=&j->task;v->session=&j->session;
    v->next_index=&j->next_index;v->ok_count=&j->ok;
    v->pending_finish_state=&j->finish_state;v->pending_finish_valid=&j->finish_valid;
    v->pending_status_end=&j->status_end;v->pending_status_phase=&j->status_phase;
    v->pending_status_detail=j->detail;v->pending_status_detail_capacity=sizeof(j->detail);
    v->batch=j->batch;
    if(f->invalid_view){v->context=NULL;return 1;}
    return 1;
}
static int stopping(void *u){return ((fixture *)u)->worker_stop;}
static int snap(void *u,intptr_t *s,uint64_t *e,hop_session_id *id,int *c){fixture *f=u;f->snapshots++;*s=f->socket;*e=f->epoch;*id=f->session;*c=f->connected;return 1;}
static int show(void *u,uint32_t b){(void)b;return ((fixture *)u)->status_visible;}
static void reset(void *u){((fixture *)u)->resets++;}
static void clear(void *u,uint32_t b,uint64_t id){fixture *f=u;(void)b;(void)id;f->clears++;}
static void abort_send(void *u,const hop_xfer_sender_context *c,intptr_t s,uint64_t e,uint32_t b){fixture *f=u;assert(c&&s==f->socket&&e==f->epoch);(void)b;f->aborts++;}
static hop_xfer_sender_job_result run(void *u,void *j,intptr_t s,uint64_t e,uint8_t *tx){fixture *f=u;assert(j&&s==f->socket&&e==f->epoch&&tx);f->runs++;return HOP_XFER_SENDER_JOB_DONE;}
static int peer_cancel(void *u,uint32_t b,uint64_t id){(void)u;(void)b;(void)id;return 0;}
static hop_xfer_sender_worker_action finish(void *u,void *j,intptr_t s,uint64_t e,hop_xfer_sender_job_result r,int c,int p){
    fixture *f=u;assert(j&&s==f->socket&&e==f->epoch&&r==HOP_XFER_SENDER_JOB_DONE&&c&&!p);f->finishes++;return HOP_XFER_SENDER_WORKER_FINISHED;}
static void release(void *u,void *p){fixture *f=u;job *j=p;assert(j->owner==2);j->owner=5;f->releases++;}
static void init(fixture *f,job *j){memset(f,0,sizeof(*f));memset(j,0,sizeof(*j));f->peer=1;f->stop_after_wait=1;f->connected=1;
    f->socket=77;f->epoch=9;f->session.bytes[0]=2;f->activate=1;f->status_visible=1;
    j->batch=12;j->task.id=1200;j->task.initialized=1;j->session.bytes[0]=1;j->next_index=4;j->ok=3;
    j->context.user=f;j->context.is_cancelled=cc;j->context.is_batch_cancelled=cb;j->context.task_bind_session=bind;
    j->context.task_set_state=setstate;j->context.status_start_job=start;assert(hop_xfer_sender_queue_push_back(&f->queue,&qops,j));}
static hop_xfer_sender_worker_runtime_ops makeops(fixture *f){hop_xfer_sender_worker_runtime_ops o;memset(&o,0,sizeof(o));o.user=f;o.queue=&f->queue;o.queue_ops=&qops;
    o.scheduler.user=f;o.scheduler.begin_pass=sb;o.scheduler.is_stopping=ss;o.scheduler.wait_for_work_locked=sw;o.scheduler.claim_job_locked=sc;o.scheduler.end_pass=se;
    o.poll_interval_ms=200;o.chunk_size=1024;o.view_job=view;o.is_stopping=stopping;o.snapshot_connection=snap;o.status_should_start=show;
    o.status_reset=reset;o.clear_active=clear;o.send_abort=abort_send;o.run_job=run;o.take_peer_cancelled_and_clear=peer_cancel;
    o.finish_job=finish;o.release_job=release;return o;}
static void test_ready(void){fixture f;job j;init(&f,&j);hop_xfer_sender_worker_runtime_ops o=makeops(&f);
    assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(!f.locked&&f.claims==1&&f.snapshots==1&&f.resets==1&&f.binds==1&&f.state_sets==1&&f.starts==1&&f.runs==1&&f.finishes==1&&!f.releases);
    assert(j.next_index==0&&j.ok==0&&j.owner==2);}
static void test_expired_cancel(void){fixture f;job j;init(&f,&j);j.session=f.session;j.cancelled=1;f.peer=0;f.cancelled=1;
    hop_xfer_sender_worker_runtime_ops o=makeops(&f);assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(f.claims==1&&f.snapshots==1&&f.aborts==1&&f.starts==1&&f.clears==1&&f.releases==1);
    assert(j.finish_valid&&j.finish_state==HOP_XFER_TASK_CANCELLED&&j.status_end&&j.status_phase==HOP_XFER_CANCELLED);
    assert(!strcmp(j.detail,"Transfer stopped while queued")&&!f.runs&&!f.finishes);}
static void test_old_session_no_abort(void){fixture f;job j;init(&f,&j);j.cancelled=1;f.peer=0;f.cancelled=1;f.session.bytes[0]=3;
    hop_xfer_sender_worker_runtime_ops o=makeops(&f);assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(!f.aborts&&f.releases==1);}
static void test_stop_after_selection(void){fixture f;job j;init(&f,&j);f.worker_stop=1;hop_xfer_sender_worker_runtime_ops o=makeops(&f);
    assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(f.claims==1&&f.clears==1&&f.releases==1&&!f.state_sets&&!f.runs&&!f.finishes);}
static void test_invalid_view_clears_active(void){fixture f;job j;init(&f,&j);f.invalid_view=1;hop_xfer_sender_worker_runtime_ops o=makeops(&f);
    assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(f.claims==1&&f.clears==1&&f.releases==1&&!f.runs&&!f.finishes);}
static void test_activation_failure(void){fixture f;job j;init(&f,&j);f.activate=0;hop_xfer_sender_worker_runtime_ops o=makeops(&f);
    assert(hop_xfer_sender_worker_run(&o)==HOP_XFER_SENDER_WORKER_RUN_STOPPED);
    assert(f.state_sets==1&&f.clears==1&&f.releases==1&&j.finish_valid&&j.finish_state==HOP_XFER_TASK_FAILED&&!j.status_end&&!f.runs);}
int main(void){test_ready();test_expired_cancel();test_old_session_no_abort();test_stop_after_selection();test_invalid_view_clears_active();test_activation_failure();
    puts("PASS: shared Sender Worker orchestration, session binding, cancellation, expiry and release");return 0;}
