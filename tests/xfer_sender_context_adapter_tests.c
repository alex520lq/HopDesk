#include "../src/core/xfer_sender_context_adapter.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t cancel_task_id;
    uint32_t cancel_batch;
    uint8_t session[HOP_TASK_SESSION_BYTES];
    int bind_calls;
    int state_calls;
    int completion_calls;
    int finish_calls;
    int dispose_calls;
    int finish_state;
    int state;
    int completion_allowed;
    int status_start_calls;
    int status_end_calls;
    int status_phase;
    uint32_t status_batch;
    int file_phase_calls;
    int file_phase;
    int file_index;
    uint32_t file_attempts;
    int stage_calls;
    int stage;
    uint64_t stage_elapsed;
    int chunk_calls;
    uint32_t chunk_id;
    uint64_t chunk_offset;
    uint32_t chunk_length;
    uint64_t chunk_read,chunk_crc,chunk_send,chunk_receive;
    int log_level;
    char log_message[512];
} fixture;

static int is_cancelled(void *user,uint32_t batch,uint64_t task_id) {
    fixture *f=(fixture *)user;
    return f->cancel_batch==batch && f->cancel_task_id==task_id;
}
static int is_batch_cancelled(void *user,uint32_t batch) {
    fixture *f=(fixture *)user;
    return f->cancel_batch==batch;
}
static int bind_session(void *user,const hop_xfer_task *task,
                        const uint8_t session[HOP_TASK_SESSION_BYTES]) {
    fixture *f=(fixture *)user;
    assert(task && session);
    memcpy(f->session,session,sizeof(f->session));
    f->bind_calls++;
    return 1;
}
static int set_state(void *user,hop_xfer_task *task,int state) {
    fixture *f=(fixture *)user;
    assert(task);
    f->state=state;f->state_calls++;
    return 1;
}
static int begin_completion(void *user,const hop_xfer_task *task) {
    fixture *f=(fixture *)user;
    assert(task);
    f->completion_calls++;
    return f->completion_allowed;
}
static void finish_task(void *user,hop_xfer_task *task,int state) {
    fixture *f=(fixture *)user;
    assert(task);
    f->finish_state=state;f->finish_calls++;
}
static void dispose_task(void *user,hop_xfer_task *task) {
    fixture *f=(fixture *)user;
    assert(task);
    task->initialized=0;task->id=0;f->dispose_calls++;
}
static void status_start(void *user,const void *job) {
    fixture *f=(fixture *)user;
    assert(job);f->status_start_calls++;
}
static void status_end(void *user,uint32_t batch,int phase,const char *detail) {
    fixture *f=(fixture *)user;
    f->status_batch=batch;f->status_phase=phase;
    assert(detail && detail[0]);f->status_end_calls++;
}
static void status_file(void *user,uint32_t batch,int index,int phase,uint32_t attempts) {
    fixture *f=(fixture *)user;
    f->status_batch=batch;f->file_index=index;f->file_phase=phase;
    f->file_attempts=attempts;f->file_phase_calls++;
}
static void status_stage(void *user,uint32_t batch,int stage,uint64_t elapsed) {
    fixture *f=(fixture *)user;
    f->status_batch=batch;f->stage=stage;f->stage_elapsed=elapsed;f->stage_calls++;
}
static void status_chunk(void *user,uint32_t batch,uint32_t id,uint64_t offset,
                         uint32_t length,uint64_t read_ns,uint64_t crc_ns,
                         uint64_t send_ns,uint64_t receive_ns) {
    fixture *f=(fixture *)user;
    f->status_batch=batch;f->chunk_id=id;f->chunk_offset=offset;f->chunk_length=length;
    f->chunk_read=read_ns;f->chunk_crc=crc_ns;f->chunk_send=send_ns;
    f->chunk_receive=receive_ns;f->chunk_calls++;
}
static void log_message(void *user,int level,const char *message) {
    fixture *f=(fixture *)user;
    f->log_level=level;
    snprintf(f->log_message,sizeof(f->log_message),"%s",message?message:"");
}
static hop_xfer_sender_context make_context(fixture *f) {
    hop_xfer_sender_context c;
    memset(&c,0,sizeof(c));
    c.user=f;c.is_cancelled=is_cancelled;c.is_batch_cancelled=is_batch_cancelled;
    c.task_bind_session=bind_session;c.task_set_state=set_state;
    c.task_begin_completion=begin_completion;c.task_finish=finish_task;
    c.task_dispose=dispose_task;c.status_start_job=status_start;
    c.status_end=status_end;c.status_file_phase=status_file;
    c.status_stage_time=status_stage;c.status_chunk_timed=status_chunk;
    c.log_message=log_message;
    return c;
}

static void test_cancellation_helpers(void) {
    fixture f={.cancel_batch=7,.cancel_task_id=7001};
    hop_xfer_sender_context c=make_context(&f);
    hop_xfer_task task;memset(&task,0,sizeof(task));task.id=7001;
    assert(hop_xfer_sender_context_task_cancel_requested(&c,7,&task));
    assert(!hop_xfer_sender_context_task_cancel_requested(&c,8,&task));
    assert(hop_xfer_sender_context_job_cancel_requested(&c,7,0));
    assert(hop_xfer_sender_context_job_cancel_requested(&c,8,7001)==0);
    f.cancel_batch=0;
    assert(hop_xfer_sender_context_job_cancel_requested(&c,8,7001)==0);
    f.cancel_batch=8;
    assert(hop_xfer_sender_context_job_cancel_requested(&c,8,7001));
    task.id=0;
    assert(!hop_xfer_sender_context_task_cancel_requested(&c,8,&task));
}

static void test_task_lifecycle_forwarding(void) {
    fixture f={.completion_allowed=1};
    hop_xfer_sender_context c=make_context(&f);
    hop_xfer_task task;memset(&task,0,sizeof(task));task.id=9001;task.initialized=1;
    uint8_t session[HOP_TASK_SESSION_BYTES];
    for(size_t i=0;i<sizeof(session);i++)session[i]=(uint8_t)(i+1);
    assert(hop_xfer_sender_context_task_bind_session(&c,&task,session));
    assert(f.bind_calls==1 && !memcmp(f.session,session,sizeof(session)));
    assert(hop_xfer_sender_context_task_set_state(&c,&task,HOP_XFER_TASK_ACTIVE));
    assert(f.state_calls==1 && f.state==HOP_XFER_TASK_ACTIVE);
    assert(hop_xfer_sender_context_task_begin_completion(&c,&task));
    assert(f.completion_calls==1);
    hop_xfer_sender_context_task_finish(&c,&task,HOP_XFER_TASK_COMPLETED);
    assert(f.finish_calls==1 && f.finish_state==HOP_XFER_TASK_COMPLETED);
    assert(hop_xfer_sender_context_task_dispose(&c,&task));
    assert(f.dispose_calls==1 && !task.initialized && !task.id);
}

static void test_status_and_log_forwarding(void) {
    fixture f={0};
    hop_xfer_sender_context c=make_context(&f);
    int job=1;
    hop_xfer_sender_context_status_start_job(&c,&job);
    hop_xfer_sender_context_status_end(&c,12,HOP_XFER_COMPLETED,"done");
    hop_xfer_sender_context_status_file_phase(&c,12,3,HOP_XFER_FILE_COMPLETED,2);
    hop_xfer_sender_context_status_stage_time(&c,12,HOP_XFER_TIME_CRC,55);
    hop_xfer_sender_context_status_chunk_timed(&c,12,4,1024,512,1,2,3,4);
    hop_xfer_sender_context_logf(&c,4,"batch=%u task=%llu",12u,
                                 (unsigned long long)9001);
    assert(f.status_start_calls==1 && f.status_end_calls==1);
    assert(f.status_batch==12 && f.status_phase==HOP_XFER_COMPLETED);
    assert(f.file_phase_calls==1 && f.file_index==3 &&
           f.file_phase==HOP_XFER_FILE_COMPLETED && f.file_attempts==2);
    assert(f.stage_calls==1 && f.stage==HOP_XFER_TIME_CRC && f.stage_elapsed==55);
    assert(f.chunk_calls==1 && f.chunk_id==4 && f.chunk_offset==1024 &&
           f.chunk_length==512 && f.chunk_read==1 && f.chunk_crc==2 &&
           f.chunk_send==3 && f.chunk_receive==4);
    assert(f.log_level==4 && !strcmp(f.log_message,"batch=12 task=9001"));
}

static void test_optional_observability_is_safe(void) {
    hop_xfer_sender_context c;memset(&c,0,sizeof(c));
    hop_xfer_sender_context_status_start_job(&c,NULL);
    hop_xfer_sender_context_status_end(&c,1,0,"none");
    hop_xfer_sender_context_status_file_phase(&c,1,0,0,0);
    hop_xfer_sender_context_status_stage_time(&c,1,0,0);
    hop_xfer_sender_context_status_chunk_timed(&c,1,0,0,0,0,0,0,0);
    hop_xfer_sender_context_logf(&c,0,"ignored");
}

int main(void) {
    test_cancellation_helpers();
    test_task_lifecycle_forwarding();
    test_status_and_log_forwarding();
    test_optional_observability_is_safe();
    puts("PASS: Sender context callback forwarding and validation");
    return 0;
}
