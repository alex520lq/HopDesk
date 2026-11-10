#include "../src/core/xfer_sender_worker_flow.h"
#include "../src/core/xfer_sender_lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EV_CANCEL=1,EV_BEGIN,EV_STORE,EV_SET_QUEUED,EV_REQUEUE,EV_ABORT,EV_DONE,EV_PEER_CLEAR,EV_FINISH,EV_STATUS,EV_DISPOSE };
typedef struct {
    uint64_t now_ms; unsigned delay_ms; int cancelled,stopping,stop_on_requeue,begin_ok,set_queued_ok,requeue_ok,peer_cancelled;
    hop_xfer_sender_job_result done_result;
    int cancel_calls,begin_calls,set_queued_calls,requeue_calls,store_calls,abort_calls,done_calls,peer_clear_calls;
    int finish_calls,status_calls,dispose_calls,log_calls;
    hop_xfer_task_state finished_state; int status_phase; char status_detail[96];
    uint32_t stored_attempt; uint64_t stored_retry_at,stored_give_up_at;
    int events[24],event_count;
} fake_worker;
static void record(fake_worker*f,int e){assert(f->event_count<24);f->events[f->event_count++]=e;}
static uint64_t fake_now(void*u){return ((fake_worker*)u)->now_ms;}
static unsigned fake_delay(void*u,uint32_t a){(void)a;return ((fake_worker*)u)->delay_ms;}
static int fake_cancel(void*u,uint32_t b,uint64_t t){fake_worker*f=u;(void)b;(void)t;f->cancel_calls++;record(f,EV_CANCEL);return f->cancelled;}
static int fake_stopping(void*u){return ((fake_worker*)u)->stopping;}
static int fake_begin(void*u,uint64_t t){fake_worker*f=u;(void)t;f->begin_calls++;record(f,EV_BEGIN);return f->begin_ok;}
static int fake_set_queued(void*u,uint64_t t){fake_worker*f=u;(void)t;f->set_queued_calls++;record(f,EV_SET_QUEUED);return f->set_queued_ok;}
static void fake_store(void*u,const hop_xfer_sender_worker_job*j){fake_worker*f=u;f->store_calls++;f->stored_attempt=j->attempt;f->stored_retry_at=j->retry_at_ms;f->stored_give_up_at=j->give_up_at_ms;record(f,EV_STORE);}
static int fake_requeue(void*u){fake_worker*f=u;f->requeue_calls++;assert(f->set_queued_calls==1&&f->store_calls==1);record(f,EV_REQUEUE);if(f->stop_on_requeue)f->stopping=1;return f->requeue_ok;}
static int fake_peer_clear(void*u,uint32_t b,uint64_t t){fake_worker*f=u;(void)b;(void)t;f->peer_clear_calls++;assert(f->done_calls||f->abort_calls);record(f,EV_PEER_CLEAR);return f->peer_cancelled;}
static void fake_abort(void*u,uint32_t b){fake_worker*f=u;(void)b;f->abort_calls++;record(f,EV_ABORT);}
static hop_xfer_sender_job_result fake_done(void*u){fake_worker*f=u;f->done_calls++;record(f,EV_DONE);return f->done_result;}
static void fake_finish(void*u,hop_xfer_task_state s){fake_worker*f=u;f->finish_calls++;f->finished_state=s;record(f,EV_FINISH);}
static void fake_status(void*u,uint32_t b,int p,const char*d){fake_worker*f=u;(void)b;f->status_calls++;f->status_phase=p;(void)snprintf(f->status_detail,sizeof(f->status_detail),"%s",d);record(f,EV_STATUS);}
static void fake_dispose(void*u){fake_worker*f=u;assert(f->finish_calls==1&&f->status_calls==1);f->dispose_calls++;record(f,EV_DISPOSE);}
static void fake_log(void*u,hop_xfer_sender_worker_log_event e,const hop_xfer_sender_worker_job*j,unsigned d){fake_worker*f=u;(void)e;(void)j;(void)d;f->log_calls++;}
static hop_xfer_sender_worker_ops make_ops(fake_worker*f){hop_xfer_sender_worker_ops o;memset(&o,0,sizeof(o));o.user=f;o.max_attempts=5;o.reconnect_budget_ms=3000;o.now_ms=fake_now;o.retry_delay_ms=fake_delay;o.is_cancelled=fake_cancel;o.is_stopping=fake_stopping;o.begin_completion=fake_begin;o.set_queued=fake_set_queued;o.store_retry_state=fake_store;o.requeue_front=fake_requeue;o.peer_cancelled_and_clear_current=fake_peer_clear;o.send_abort=fake_abort;o.send_done=fake_done;o.task_finish=fake_finish;o.status_end=fake_status;o.dispose_job=fake_dispose;o.log_event=fake_log;return o;}
static fake_worker make_worker(void){fake_worker f;memset(&f,0,sizeof(f));f.begin_ok=1;f.set_queued_ok=1;f.requeue_ok=1;f.done_result=HOP_XFER_SENDER_JOB_DONE;return f;}
static hop_xfer_sender_worker_job make_job(hop_xfer_sender_job_result r){hop_xfer_sender_worker_job j;memset(&j,0,sizeof(j));j.batch=17;j.task_id=99;j.result=r;j.entry_count=3;j.total_count=3;j.ok_count=3;return j;}
static void assert_terminal(const fake_worker*f,int status_first){assert(f->finish_calls==1&&f->status_calls==1&&f->dispose_calls==1);if(status_first){assert(f->events[f->event_count-3]==EV_STATUS&&f->events[f->event_count-2]==EV_FINISH);}else{assert(f->events[f->event_count-3]==EV_FINISH&&f->events[f->event_count-2]==EV_STATUS);}assert(f->events[f->event_count-1]==EV_DISPOSE);}
static void test_retry_scheduled(void){fake_worker f=make_worker();f.now_ms=1000;f.delay_ms=250;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_REQUEUED);assert(j.attempt==1&&j.retry_at_ms==1250&&j.give_up_at_ms==4000);assert(f.stored_attempt==1&&f.stored_retry_at==1250&&f.stored_give_up_at==4000);assert(f.cancel_calls==1&&f.store_calls==1&&f.set_queued_calls==1&&f.requeue_calls==1);assert(!f.finish_calls&&!f.status_calls&&!f.dispose_calls);}
static void test_retry_exhausted(void){fake_worker f=make_worker();f.now_ms=2000;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);j.attempt=4;j.give_up_at_ms=1500;assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(j.attempt==5&&f.store_calls==1&&!f.requeue_calls);assert(f.finished_state==HOP_XFER_TASK_FAILED&&f.status_phase==HOP_XFER_FAILED);assert(strcmp(f.status_detail,"Reconnect attempts exhausted")==0&&f.log_calls==1);assert_terminal(&f,0);}
static void test_cancel_overrides_network_down(void){fake_worker f=make_worker();f.cancelled=1;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.abort_calls==1&&!f.store_calls&&!f.requeue_calls);assert(f.finished_state==HOP_XFER_TASK_CANCELLED&&strcmp(f.status_detail,"Cancelled")==0);assert_terminal(&f,0);}
static void test_done_commit_unknown(void){fake_worker f=make_worker();f.done_result=HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_DONE);j.completion_path=1;assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.begin_calls==1&&f.done_calls==1&&f.peer_clear_calls==1);assert(f.finished_state==HOP_XFER_TASK_INTERRUPTED&&f.status_phase==HOP_XFER_FAILED);assert(strcmp(f.status_detail,"Completion delivery uncertain; peer may have received it")==0);assert(f.events[0]==EV_BEGIN&&f.events[1]==EV_DONE&&f.events[2]==EV_PEER_CLEAR);assert_terminal(&f,0);}
static void test_completion_gate_cancel_sends_abort(void){fake_worker f=make_worker();f.begin_ok=0;f.peer_cancelled=1;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_DONE);j.completion_path=1;assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.abort_calls==1&&!f.done_calls&&f.peer_clear_calls==1);assert(f.events[0]==EV_BEGIN&&f.events[1]==EV_ABORT&&f.events[2]==EV_PEER_CLEAR);assert(f.finished_state==HOP_XFER_TASK_CANCELLED);assert_terminal(&f,0);}
static void test_receiver_abort_cancel_mapping(void){fake_worker f=make_worker();hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_PEER_ABORT);j.peer_cancelled=1;assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(!f.abort_calls&&!f.peer_clear_calls&&f.finished_state==HOP_XFER_TASK_CANCELLED);assert_terminal(&f,0);}
static void test_requeue_failure_is_terminal(void){fake_worker f=make_worker();f.now_ms=1000;f.requeue_ok=0;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.set_queued_calls==1&&f.requeue_calls==1&&f.finished_state==HOP_XFER_TASK_FAILED);assert(strcmp(f.status_detail,"Retry queue admission failed")==0);assert_terminal(&f,1);}

static void test_shutdown_before_retry(void){fake_worker f=make_worker();f.stopping=1;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(!f.store_calls&&!f.set_queued_calls&&!f.requeue_calls&&!f.abort_calls);assert(f.finished_state==HOP_XFER_TASK_INTERRUPTED&&f.status_phase==HOP_XFER_FAILED);assert(strcmp(f.status_detail,"Transfer interrupted during shutdown")==0);assert_terminal(&f,0);}
static void test_shutdown_wins_requeue_race(void){fake_worker f=make_worker();f.now_ms=1000;f.stop_on_requeue=1;f.requeue_ok=0;hop_xfer_sender_worker_ops o=make_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.store_calls==1&&f.set_queued_calls==1&&f.requeue_calls==1);assert(f.finished_state==HOP_XFER_TASK_INTERRUPTED&&f.status_phase==HOP_XFER_FAILED);assert(strcmp(f.status_detail,"Transfer interrupted during shutdown")==0);assert_terminal(&f,0);}

/* Production-linked Task/Reservation shutdown cases: the worker policy must
 * finish the real manager-owned Task once and return its entire reservation. */
#include "../src/core/xfer_budget.h"
#include "../src/core/xfer_task.h"
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
typedef struct {hop_xfer_task task;int stopping,stop_on_requeue,finish_calls,finish_ok,status_calls,dispose_calls;hop_xfer_task_state finished_state;} task_shutdown_fixture;
static uint64_t task_now(void*u){(void)u;return 1000;}
static unsigned task_delay(void*u,uint32_t a){(void)u;(void)a;return 100;}
static int task_cancel(void*u,uint32_t b,uint64_t id){(void)u;(void)b;(void)id;return 0;}
static int task_stopping(void*u){return ((task_shutdown_fixture*)u)->stopping;}
static int task_begin(void*u,uint64_t id){(void)u;(void)id;return 1;}
static int task_set_queued(void*u,uint64_t id){task_shutdown_fixture*f=u;(void)id;return hop_xfer_task_set_state(&f->task,HOP_XFER_TASK_QUEUED);}
static void task_store(void*u,const hop_xfer_sender_worker_job*j){(void)u;(void)j;}
static int task_requeue(void*u){task_shutdown_fixture*f=u;if(f->stop_on_requeue)f->stopping=1;return 0;}
static int task_peer_clear(void*u,uint32_t b,uint64_t id){(void)u;(void)b;(void)id;return 0;}
static void task_abort(void*u,uint32_t b){(void)u;(void)b;}
static hop_xfer_sender_job_result task_done(void*u){(void)u;return HOP_XFER_SENDER_JOB_DONE;}
static void task_finish(void*u,hop_xfer_task_state state){task_shutdown_fixture*f=u;f->finish_calls++;f->finished_state=state;f->finish_ok=hop_xfer_task_finish(&f->task,state);}
static void task_status(void*u,uint32_t b,int p,const char*d){task_shutdown_fixture*f=u;(void)b;(void)p;(void)d;f->status_calls++;}
static void task_dispose(void*u){task_shutdown_fixture*f=u;f->dispose_calls++;hop_xfer_task_dispose(&f->task);}
static hop_xfer_sender_worker_ops make_task_ops(task_shutdown_fixture*f){hop_xfer_sender_worker_ops o;memset(&o,0,sizeof(o));o.user=f;o.max_attempts=5;o.reconnect_budget_ms=3000;o.now_ms=task_now;o.retry_delay_ms=task_delay;o.is_cancelled=task_cancel;o.is_stopping=task_stopping;o.begin_completion=task_begin;o.set_queued=task_set_queued;o.store_retry_state=task_store;o.requeue_front=task_requeue;o.peer_cancelled_and_clear_current=task_peer_clear;o.send_abort=task_abort;o.send_done=task_done;o.task_finish=task_finish;o.status_end=task_status;o.dispose_job=task_dispose;return o;}
static void run_real_task_shutdown_case(int race){task_shutdown_fixture f;memset(&f,0,sizeof(f));f.stop_on_requeue=race;assert(hop_xfer_task_init_classified(&f.task,0,0,701u+(uint32_t)race,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));assert(hop_xfer_task_reserve_job(&f.task));assert(hop_xfer_task_reserve_entry(&f.task,4096));assert(hop_xfer_task_set_state(&f.task,HOP_XFER_TASK_SCANNING));assert(hop_xfer_task_set_state(&f.task,HOP_XFER_TASK_QUEUED));assert(hop_xfer_task_set_state(&f.task,HOP_XFER_TASK_ACTIVE));hop_xfer_sender_worker_ops o=make_task_ops(&f);hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_NET_DOWN);j.task_id=f.task.id;j.batch=701u+(uint32_t)race;if(!race)f.stopping=1;assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_FINISHED);assert(f.stopping&&f.finish_calls==1&&f.finish_ok&&f.finished_state==HOP_XFER_TASK_INTERRUPTED);assert(f.status_calls==1&&f.dispose_calls==1&&!f.task.initialized);hop_xfer_task_audit audit;assert(hop_xfer_task_reconcile(&audit));assert(!audit.mismatches&&!audit.task_count&&!audit.jobs&&!audit.entries&&!audit.pending_bytes&&!audit.image_bytes&&!audit.rx_queue_slots&&!audit.rx_queue_bytes&&!audit.image_temp_bytes);}
static void test_shutdown_returns_task_reservations(void){run_real_task_shutdown_case(0);run_real_task_shutdown_case(1);}
static void test_invalid_contract_has_no_side_effects(void){fake_worker f=make_worker();hop_xfer_sender_worker_ops o=make_ops(&f);o.max_attempts=0;hop_xfer_sender_worker_job j=make_job(HOP_XFER_SENDER_JOB_DONE);assert(hop_xfer_sender_worker_finish_job(&o,&j)==HOP_XFER_SENDER_WORKER_INVALID);assert(!f.finish_calls&&!f.status_calls&&!f.dispose_calls);}
int main(void){test_retry_scheduled();test_retry_exhausted();test_cancel_overrides_network_down();test_done_commit_unknown();test_completion_gate_cancel_sends_abort();test_receiver_abort_cancel_mapping();test_requeue_failure_is_terminal();test_shutdown_before_retry();test_shutdown_wins_requeue_race();test_shutdown_returns_task_reservations();test_invalid_contract_has_no_side_effects();puts("PASS: Sender worker retry, shutdown, Task terminal and Reservation release");return 0;}
