#include "../src/core/xfer_sender_engine_adapter.h"
#include "../src/core/hop.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now_ns;
    int ack_registers,ack_marks,ack_deadlines,ack_unregisters,ack_batch_unregisters;
    int messages,chunks,yields,opens,reads,closes;
    int ack_status;
    int stopping_flag;
    int done_calls;
    uint8_t done_payload[20];
    uint8_t session[HOP_SESSION_BYTES];
    uint8_t message_types[8];
} fixture;
typedef struct {const char *name,*path;uint64_t size,mtime;uint32_t flags;} entry;

static uint64_t now(void *u){return ((fixture *)u)->now_ns;}
static int wait_ns(void *u,uint64_t ns){
    fixture *f=(fixture *)u;
    f->now_ns+=ns;
    return !f->stopping_flag;
}
static int stopping(void *u){return ((fixture *)u)->stopping_flag;}
static int cancelled(void *u,uint32_t b,uint64_t id){(void)u;(void)b;(void)id;return 0;}
static int batch_cancelled(void *u,uint32_t b){(void)u;(void)b;return 0;}
static int peer_aborted(void *u,uint32_t b){(void)u;(void)b;return 0;}
static int session_current(void *u,intptr_t socket,uint64_t epoch){(void)u;return socket==7&&epoch==9;}
int hop_xfer_ack_coordinator_register(hop_xfer_ack_coordinator *c,uint32_t b,uint32_t id){fixture *f=(fixture *)c;(void)b;(void)id;f->ack_registers++;return 1;}
int hop_xfer_ack_coordinator_mark_sent(hop_xfer_ack_coordinator *c,uint32_t b,uint32_t id,uint64_t sent){fixture *f=(fixture *)c;(void)b;(void)id;(void)sent;f->ack_marks++;return 1;}
int hop_xfer_ack_coordinator_set_deadline(hop_xfer_ack_coordinator *c,uint32_t b,uint32_t id,uint64_t deadline){fixture *f=(fixture *)c;(void)b;(void)id;assert(deadline>0);f->ack_deadlines++;return 1;}
int hop_xfer_ack_coordinator_wait_take(hop_xfer_ack_coordinator *c,uint32_t b,uint32_t id,uint32_t ms,int *status,uint64_t *rtt,int *expired){
    fixture *f=(fixture *)c;(void)b;(void)id;(void)ms;
    *status=f->ack_status ? f->ack_status : HOP_FILE_ACK_OK;
    *rtt=0;*expired=0;return 1;
}
void hop_xfer_ack_coordinator_unregister(hop_xfer_ack_coordinator *c,uint32_t b,uint32_t id){fixture *f=(fixture *)c;(void)b;(void)id;f->ack_unregisters++;}
void hop_xfer_ack_coordinator_unregister_batch(hop_xfer_ack_coordinator *c,uint32_t b){fixture *f=(fixture *)c;(void)b;f->ack_batch_unregisters++;}
static int send_message(void *u,intptr_t socket,uint8_t type,const uint8_t *payload,uint32_t len,uint32_t batch){
    fixture *f=u;(void)payload;(void)len;(void)batch;assert(socket==7);f->message_types[f->messages++]=type;return 1;
}
static int send_chunk(void *u,intptr_t socket,uint8_t *tx,uint32_t batch,uint32_t id,
                      uint64_t off,const uint8_t *data,uint32_t len,uint32_t crc,
                      uint64_t read_ns,uint64_t crc_ns){
    fixture *f=u;(void)tx;(void)batch;(void)id;(void)off;(void)data;(void)len;
    (void)crc;(void)read_ns;(void)crc_ns;assert(socket==7);f->chunks++;return 1;
}
static const entry s_entry={"folder","folder",0,0,HOP_FILE_FLAG_DIR};
static const void *entry_at(void *u,int i){(void)u;return i==0?&s_entry:NULL;}
static const char *entry_name(void *u,const void *p){(void)u;return ((const entry *)p)->name;}
static const char *entry_path(void *u,const void *p){(void)u;return ((const entry *)p)->path;}
static uint64_t entry_size(void *u,const void *p){(void)u;return ((const entry *)p)->size;}
static uint64_t entry_mtime(void *u,const void *p){(void)u;return ((const entry *)p)->mtime;}
static uint32_t entry_flags(void *u,const void *p){(void)u;return ((const entry *)p)->flags;}
static int open_file(void *u,const void *p,intptr_t *h,uint32_t *e){fixture *f=u;(void)p;(void)h;(void)e;f->opens++;return 0;}
static hop_xfer_sender_read_result read_file(void *u,intptr_t h,uint8_t *b,uint32_t n,uint32_t *got,uint32_t *e){
    fixture *f=u;(void)h;(void)b;(void)n;(void)got;(void)e;f->reads++;return HOP_XFER_SENDER_READ_EOF;
}
static void close_file(void *u,intptr_t h){fixture *f=u;(void)h;f->closes++;}
static void yield_cpu(void *u){((fixture *)u)->yields++;}
static void init_context(fixture *f,hop_xfer_sender_context *c){
    memset(c,0,sizeof(*c));c->user=f;c->ack_coordinator=(hop_xfer_ack_coordinator *)f;c->now_ns=now;c->wait_ns=wait_ns;
    c->is_stopping=stopping;c->is_cancelled=cancelled;c->is_batch_cancelled=batch_cancelled;
    c->peer_aborted=peer_aborted;c->session_current=session_current;
}
static void init_ops(fixture *f,hop_xfer_sender_context *c,hop_xfer_sender_engine_adapter_ops *o,
                     int *next,int *ok,uint8_t *tx){
    memset(o,0,sizeof(*o));o->user=f;o->context=c;o->socket_handle=7;o->connection_epoch=9;
    o->batch=22;o->task_id=2200;o->entry_count=1;o->next_index=next;o->ok_count=ok;
    o->tx_buffer=tx;o->chunk_size=64;o->chunk_header_size=45;o->ack_timeout_ns=1000000000ull;
    o->ack_poll_interval_ms=100;o->max_attempts=3;o->directory_flag=HOP_FILE_FLAG_DIR;
    o->file_ok_status=HOP_FILE_OK;o->file_io_error_status=HOP_FILE_IO_ERROR;
    o->file_begin_message=HOP_MSG_FILE_BEGIN;o->file_end_message=HOP_MSG_FILE_END;
    o->image_begin_message=HOP_MSG_IMAGE_BEGIN;
    o->entry_at=entry_at;o->entry_name=entry_name;o->entry_path=entry_path;
    o->entry_size=entry_size;o->entry_mtime=entry_mtime;o->entry_flags=entry_flags;
    o->open_file=open_file;o->read_file=read_file;o->close_file=close_file;
    o->send_message=send_message;o->send_chunk=send_chunk;o->yield_cpu=yield_cpu;
}
static void test_files_adapter(void){
    fixture f={0};hop_xfer_sender_context c;init_context(&f,&c);
    uint8_t tx[45+64]={0};int next=0,ok=0;hop_xfer_sender_engine_adapter_ops o;
    init_ops(&f,&c,&o,&next,&ok,tx);
    assert(hop_xfer_sender_engine_adapter_run(&o)==HOP_XFER_SENDER_JOB_DONE);
    assert(next==1&&ok==1&&f.messages==2);
    assert(f.message_types[0]==HOP_MSG_FILE_BEGIN&&f.message_types[1]==HOP_MSG_FILE_END);
    assert(f.ack_registers==1&&f.ack_marks==1&&f.ack_deadlines==1);
    assert(f.ack_unregisters==0&&f.ack_batch_unregisters>=2);
    assert(!f.opens&&!f.reads&&!f.closes);
}
static void test_image_adapter(void){
    fixture f={0};hop_xfer_sender_context c;init_context(&f,&c);
    uint8_t tx[45+8]={0},pixels[16]={0};int next=0,ok=0;
    hop_xfer_sender_engine_adapter_ops o;init_ops(&f,&c,&o,&next,&ok,tx);
    o.is_image=1;o.image_pixels=pixels;o.image_width=2;o.image_height=2;o.image_stride=8;
    o.chunk_size=8;
    assert(hop_xfer_sender_engine_adapter_run(&o)==HOP_XFER_SENDER_JOB_DONE);
    assert(next==1&&ok==1&&f.messages==2&&f.chunks==2);
    assert(f.message_types[0]==HOP_MSG_IMAGE_BEGIN&&f.message_types[1]==HOP_MSG_FILE_END);
}
static int commit_connection(void *u,intptr_t *sock,uint64_t *epoch,uint8_t *session,size_t cap){
    fixture *f=(fixture *)u;
    assert(cap>=HOP_SESSION_BYTES);
    *sock=7;*epoch=9;memcpy(session,f->session,HOP_SESSION_BYTES);
    return 1;
}
static int commit_send_done(void *u,intptr_t sock,uint64_t epoch,const uint8_t *payload,uint32_t len,uint32_t batch){
    fixture *f=(fixture *)u;
    assert(sock==7&&epoch==9&&len==20&&batch==22);
    memcpy(f->done_payload,payload,len);
    f->done_calls++;
    return 1;
}
static void test_commit_adapter(void){
    fixture f={0};hop_xfer_sender_context c;init_context(&f,&c);
    hop_session_id sid;memset(&sid,0,sizeof(sid));sid.bytes[0]=0x5a;
    hop_session_publish(&sid);
    memcpy(f.session,sid.bytes,HOP_SESSION_BYTES);
    f.ack_status=HOP_DONE_ACK_COMPLETE;
    hop_xfer_sender_commit_adapter_ops ops;memset(&ops,0,sizeof(ops));
    ops.user=&f;ops.context=&c;ops.session=sid.bytes;ops.batch=22;ops.task_id=2200;
    ops.entry_count=3;ops.ok_count=3;ops.done_ack_slot=0;
    ops.ack_timeout_ms=1000;ops.reconnect_budget_ms=5000;ops.ack_poll_interval_ms=50;
    ops.current_connection=commit_connection;ops.send_done=commit_send_done;
    assert(hop_xfer_sender_engine_adapter_run_commit(&ops)==HOP_XFER_SENDER_JOB_DONE);
    assert(f.done_calls==1);
    assert(hop_get_be32(f.done_payload)==22&&hop_get_be32(f.done_payload+4)==3&&
           hop_get_be32(f.done_payload+8)==3&&hop_get_be64(f.done_payload+12)==0);
    f.ack_status=HOP_DONE_ACK_FAILED;
    assert(hop_xfer_sender_engine_adapter_run_commit(&ops)==HOP_XFER_SENDER_JOB_COMMIT_REJECTED);
    f.stopping_flag=1;
    assert(hop_xfer_sender_engine_adapter_run_commit(&ops)==HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN);
    hop_session_clear(&sid);
}
int main(void){
    test_files_adapter();test_image_adapter();test_commit_adapter();
    puts("PASS: shared Sender engine adapter wires file, image and commit policies");
    return 0;
}
