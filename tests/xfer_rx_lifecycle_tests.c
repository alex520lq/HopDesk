#include "xfer_rx_lifecycle.h"
#include <assert.h>
#include <stdio.h>

static int events[16], event_count, observed_reason, observed_cancelled;
static uint32_t observed_batch;
static void record(int event){assert(event_count<(int)(sizeof(events)/sizeof(events[0])));events[event_count++]=event;}
static void cb_send(void *ctx,uint32_t batch,uint8_t reason){(void)ctx;observed_batch=batch;observed_reason=reason;record(1);}
static void cb_cancel(void *ctx,uint32_t batch){(void)ctx;assert(batch==observed_batch);record(2);}
static void cb_release(void *ctx,int discard){(void)ctx;record(3);if(discard)record(4);}
static void cb_clear(void *ctx){(void)ctx;record(7);}
static void cb_dead(void *ctx,uint32_t batch,uint8_t reason){(void)ctx;assert(batch==observed_batch&&reason==observed_reason);record(5);}
static void cb_finish(void *ctx,int cancelled){(void)ctx;observed_cancelled=cancelled;record(6);}
static void cb_status(void *ctx,uint32_t batch,int cancelled){(void)ctx;assert(batch==observed_batch&&cancelled==observed_cancelled);record(8);}

static void test_abort_terminal_order(void){
    hop_xfer_rx_abort_ops ops={0};
    ops.send_abort=cb_send;ops.cancel_clipboard=cb_cancel;ops.release_resources=cb_release;
    ops.mark_dead=cb_dead;ops.finish_task=cb_finish;ops.clear_state=cb_clear;ops.status_end=cb_status;
    observed_batch=41;observed_reason=HOP_ABORT_CANCEL;event_count=0;
    assert(hop_xfer_rx_abort_execute(HOP_RX_BATCH_ABORT_IGNORE,41,HOP_ABORT_CANCEL,1,1,&ops)==HOP_RX_ABORT_IGNORED);
    assert(event_count==0);
    assert(hop_xfer_rx_abort_execute(HOP_RX_BATCH_ABORT_REPLY_ONLY,41,HOP_ABORT_CANCEL,1,1,&ops)==HOP_RX_ABORT_REPLIED);
    assert(event_count==1&&events[0]==1); /* stale batch only replays the wire ABORT */
    event_count=0;
    assert(hop_xfer_rx_abort_execute(HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE,41,HOP_ABORT_CANCEL,1,1,&ops)==HOP_RX_ABORT_TERMINATED);
    const int paste_order[]={2,1,3,4,5,6,7,8};
    assert(event_count==8);
    for(int i=0;i<8;i++)assert(events[i]==paste_order[i]);
    assert(observed_cancelled==1);

    event_count=0;observed_batch=42;observed_reason=HOP_ABORT_DISK;
    assert(hop_xfer_rx_abort_execute(HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE,42,HOP_ABORT_DISK,0,0,&ops)==HOP_RX_ABORT_TERMINATED);
    const int normal_order[]={3,4,5,6,7,8};
    assert(event_count==6);
    for(int i=0;i<6;i++)assert(events[i]==normal_order[i]);
    assert(observed_cancelled==0);
}

int main(void) {
    hop_xfer_rx_lifecycle lifecycle;
    hop_xfer_rx_lifecycle_init(&lifecycle);
    hop_session_id a = {{1}}, b = {{2}};
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &a));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 77, HOP_ABORT_CANCEL);
    uint8_t reason = 0;
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(reason == HOP_ABORT_CANCEL);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 78, NULL));

    assert(!hop_xfer_rx_lifecycle_bind_session(&lifecycle, &a));
    assert(hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, &reason));
    assert(hop_xfer_rx_lifecycle_bind_session(&lifecycle, &b));
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 77, NULL));
    assert(hop_session_id_equal(hop_xfer_rx_lifecycle_session(&lifecycle), &b));

    hop_xfer_rx_lifecycle_mark_dead(&lifecycle, 88, HOP_ABORT_DISK);
    hop_xfer_rx_lifecycle_init(&lifecycle);
    assert(!hop_xfer_rx_lifecycle_dead_reason(&lifecycle, 88, NULL));
    test_abort_terminal_order();
    puts("PASS: receiver session binding and ordered ABORT terminal effects");
    return 0;
}
