#include "../src/core/xfer_sender_ack_wait.h"
#include "../src/core/xfer_sender_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now_ns, rtt_ns;
    uint32_t last_wait_ms, batch, id;
    int hit, status, expired, cancel, batch_cancel, peer_abort, stopping, current;
    int waits, unregisters;
} fixture;

static uint64_t now(void *u){return ((fixture *)u)->now_ns;}
int hop_xfer_ack_coordinator_wait_take(hop_xfer_ack_coordinator *coordinator,
    uint32_t b,uint32_t id,uint32_t ms,int *status,uint64_t *rtt,int *expired){
    fixture *f=(fixture *)coordinator;f->waits++;f->last_wait_ms=ms;f->batch=b;f->id=id;
    if(!f->hit){f->now_ns+=(uint64_t)ms*1000000ull;return 0;}
    *status=f->status;*rtt=f->rtt_ns;*expired=f->expired;return 1;
}
void hop_xfer_ack_coordinator_unregister(hop_xfer_ack_coordinator *coordinator,
    uint32_t b,uint32_t id){
    fixture *f=(fixture *)coordinator;f->unregisters++;f->batch=b;f->id=id;
}
static int is_stopping(void *u){return ((fixture *)u)->stopping;}
static int session_current(void *u,intptr_t s,uint64_t e){
    fixture *f=u;return f->current&&s!=(intptr_t)-1&&e==9;
}
static int is_cancelled(void *u,uint32_t b,uint64_t id){
    fixture *f=u;(void)b;(void)id;return f->cancel;
}
static int is_batch_cancelled(void *u,uint32_t b){
    fixture *f=u;(void)b;return f->batch_cancel;
}
static int peer_aborted(void *u,uint32_t b){
    fixture *f=u;(void)b;return f->peer_abort;
}
static hop_xfer_sender_context make_context(fixture *f){
    hop_xfer_sender_context c;memset(&c,0,sizeof(c));c.user=f;
    c.ack_coordinator=(hop_xfer_ack_coordinator *)f;c.now_ns=now;
    c.is_stopping=is_stopping;c.session_current=session_current;
    c.is_cancelled=is_cancelled;c.is_batch_cancelled=is_batch_cancelled;
    c.peer_aborted=peer_aborted;return c;
}
static void init(fixture *f){memset(f,0,sizeof(*f));f->current=1;f->now_ns=100;}
static void test_delivered(void){
    fixture f;init(&f);f.hit=1;f.status=HOP_XFER_SENDER_ACK_OK;f.rtt_ns=45000;
    hop_xfer_sender_context c=make_context(&f);uint64_t rtt=0;
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,1000000,100,&rtt)==HOP_XFER_SENDER_ACK_OK);
    assert(rtt==45&&f.waits==1&&f.last_wait_ms==100&&f.unregisters==0);
}
static void test_dynamic_slice(void){
    fixture f;init(&f);f.hit=1;f.status=17;
    hop_xfer_sender_context c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,2000100,0,NULL)==17);
    assert(f.last_wait_ms==2);
}
static void test_deadline_before_wait(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,100,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_TIMEOUT);
    assert(f.waits==0&&f.unregisters==1);
}
static void test_cancel_and_abort(void){
    fixture f;init(&f);f.cancel=1;hop_xfer_sender_context c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,99999999,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_CANCELLED);
    assert(f.unregisters==1);
    init(&f);f.peer_abort=1;c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,99999999,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT);
    assert(f.unregisters==1);
}
static void test_send_dead_and_late_ack(void){
    fixture f;init(&f);f.current=0;hop_xfer_sender_context c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,99999999,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD);
    assert(f.unregisters==1);
    init(&f);f.hit=1;f.expired=1;c=make_context(&f);
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,99999999,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_TIMEOUT);
    assert(f.unregisters==0);
}
static void test_invalid_contract(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);c.ack_coordinator=NULL;
    assert(hop_xfer_sender_ack_wait(&c,7,9,12,99,3,99999999,100,NULL)==HOP_XFER_SENDER_ACK_WAIT_LOCAL_IO);
}
int main(void){
    test_delivered();test_dynamic_slice();test_deadline_before_wait();
    test_cancel_and_abort();test_send_dead_and_late_ack();test_invalid_contract();
    puts("PASS: shared Sender ACK wait preserves deadline, cancellation, peer ABORT and connection identity");
    return 0;
}
