#include "../src/core/xfer_sender_chunk.h"
#include "../src/core/hop.h"
#include "../src/core/xfer_tracking.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHUNK_HEADER (HOP_HEADER_SIZE + HOP_DATA_PREFIX + 20u)
typedef struct {
    uint64_t now_ns;
    uint8_t session[HOP_TASK_SESSION_BYTES];
    uint8_t frame[512];
    size_t frame_size;
    int snapshot_ok,write_ok,wait_calls,stage_calls,chunk_calls;
} fixture;
static int feedback_calls;
static uint64_t now(void *u){return ((fixture *)u)->now_ns;}
static int wait_ns(void *u, uint64_t ns) {
    fixture *f = u;
    if (++f->wait_calls > 2) return 0;
    f->now_ns += ns;
    return 1;
}
static int stopping(void *u){(void)u;return 0;}
static int snapshot(void *u,intptr_t socket,uint8_t session[HOP_TASK_SESSION_BYTES],uint64_t *epoch){
    fixture *f=u;if(!f->snapshot_ok||socket!=7||!session||!epoch)return 0;
    memcpy(session,f->session,sizeof(f->session));*epoch=9;return 1;
}
static int write_frame(void *u,intptr_t socket,uint64_t epoch,
                       const hop_xfer_sender_frame_part *parts,size_t count){
    fixture *f=u;if(!f->write_ok||socket!=7||epoch!=9)return 0;f->frame_size=0;
    for(size_t i=0;i<count;i++){assert(f->frame_size+parts[i].size<=sizeof(f->frame));
        memcpy(f->frame+f->frame_size,parts[i].data,parts[i].size);f->frame_size+=parts[i].size;}
    return 1;
}
static void stage_time(void *u,uint32_t b,int stage,uint64_t ns){
    fixture *f=u;(void)b;(void)stage;(void)ns;f->stage_calls++;
}
static void chunk_time(void *u,uint32_t b,uint32_t id,uint64_t off,uint32_t len,
                       uint64_t read_ns,uint64_t crc_ns,uint64_t send_ns,uint64_t recv_ns){
    fixture *f=u;(void)b;(void)id;(void)off;(void)len;(void)read_ns;(void)crc_ns;(void)send_ns;(void)recv_ns;f->chunk_calls++;
}
static void feedback(uint64_t rtt){(void)rtt;feedback_calls++;}
static void log_message(void *u,int level,const char *message){(void)u;(void)level;(void)message;}
static hop_xfer_sender_context make_context(fixture *f){
    hop_xfer_sender_context c;memset(&c,0,sizeof(c));c.user=f;c.now_ns=now;c.wait_ns=wait_ns;
    c.is_stopping=stopping;c.snapshot_session=snapshot;c.write_frame=write_frame;
    c.status_stage_time=stage_time;c.status_chunk_timed=chunk_time;c.log_message=log_message;return c;
}
static void init(fixture *f){memset(f,0,sizeof(*f));f->snapshot_ok=1;f->write_ok=1;f->session[0]=5;}
static void test_frame_and_owned_buffer(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);hop_xfer_rate_limiter rate;
    hop_xfer_rate_init(&rate,1000000,1000000,1000000,8,0);
    uint8_t tx[CHUNK_HEADER+8]={0};uint8_t *data=tx+CHUNK_HEADER;data[0]=1;data[1]=2;data[2]=3;data[3]=4;
    hop_xfer_sender_chunk_ops o={&c,&rate,7,tx,sizeof(tx),feedback};
    assert(hop_xfer_sender_send_chunk(&o,42,3,9,data,4,0x12345678,11,12));
    assert(f.frame_size==CHUNK_HEADER+4&&f.frame[0]==HOP_MSG_FILE_CHUNK);
    assert(hop_get_be32(f.frame+1)==HOP_DATA_PREFIX+20+4);
    hop_session_id sid={{5}};uint32_t batch=0,n=0;const uint8_t *body=NULL;
    assert(hop_data_unwrap(HOP_MSG_FILE_CHUNK,f.frame+HOP_HEADER_SIZE,
        (uint32_t)(f.frame_size-HOP_HEADER_SIZE),&sid,&batch,&body,&n));
    assert(batch==42&&n==24&&hop_get_be32(body)==3&&hop_get_be64(body+4)==9);
    assert(hop_get_be32(body+12)==4&&hop_get_be32(body+16)==0x12345678);
    assert(!memcmp(body+20,data,4)&&f.chunk_calls==1);
}
static void test_external_buffer_and_feedback(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);hop_xfer_rate_limiter rate;
    hop_xfer_rate_init(&rate,1000000,1000000,1000000,8,0);rate.send_samples=7;feedback_calls=0;
    uint8_t tx[CHUNK_HEADER]={0},data[2]={8,9};
    hop_xfer_sender_chunk_ops o={&c,&rate,7,tx,sizeof(tx),feedback};
    assert(hop_xfer_sender_send_chunk(&o,4,5,6,data,2,7,0,0));
    assert(f.frame_size==CHUNK_HEADER+2&&f.frame[CHUNK_HEADER]==8&&f.frame[CHUNK_HEADER+1]==9);
    assert(feedback_calls==1);
}
static void test_pacing_and_invalid_session(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);hop_xfer_rate_limiter rate;
    hop_xfer_rate_init(&rate,1000,1000,1000,4,0);rate.tokens_bytes=2;
    uint8_t tx[CHUNK_HEADER+4]={0};uint8_t *data=tx+CHUNK_HEADER;
    hop_xfer_sender_chunk_ops o={&c,&rate,7,tx,sizeof(tx),feedback};
    assert(hop_xfer_sender_send_chunk(&o,1,1,0,data,4,0,0,0));
    assert(f.wait_calls == 1 && f.now_ns == 2000000);
    f.snapshot_ok=0;assert(!hop_xfer_sender_send_chunk(&o,1,1,0,data,4,0,0,0));
    assert(f.stage_calls>=3);
}
static void test_burst_rejected(void){
    fixture f;init(&f);hop_xfer_sender_context c=make_context(&f);hop_xfer_rate_limiter rate;
    hop_xfer_rate_init(&rate,1000,1000,1000,2,0);
    uint8_t tx[CHUNK_HEADER+4]={0};uint8_t *data=tx+CHUNK_HEADER;
    hop_xfer_sender_chunk_ops o={&c,&rate,7,tx,sizeof(tx),feedback};
    assert(!hop_xfer_sender_send_chunk(&o,1,1,0,data,4,0,1,2));
    assert(f.stage_calls==2&&f.frame_size==0);
}
int main(void){
    test_frame_and_owned_buffer();test_external_buffer_and_feedback();
    test_pacing_and_invalid_session();test_burst_rejected();
    puts("PASS: shared Sender chunk pacing, framing, write and telemetry");
    return 0;
}
