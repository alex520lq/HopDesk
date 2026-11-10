#include "xfer_sender_context_adapter.h"

#include <stdarg.h>
#include <stdio.h>

int hop_xfer_sender_context_task_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch,
    const hop_xfer_task *task) {
    if (!context || !context->is_cancelled || !task || !task->id) return 0;
    return context->is_cancelled(context->user,batch,task->id);
}

int hop_xfer_sender_context_job_cancel_requested(
    const hop_xfer_sender_context *context, uint32_t batch, uint64_t task_id) {
    if (!context) return 0;
    int batch_cancelled = context->is_batch_cancelled &&
        context->is_batch_cancelled(context->user,batch);
    int task_cancelled = task_id && context->is_cancelled &&
        context->is_cancelled(context->user,batch,task_id);
    return batch_cancelled || task_cancelled;
}

int hop_xfer_sender_context_task_bind_session(
    const hop_xfer_sender_context *context, const hop_xfer_task *task,
    const uint8_t session[HOP_TASK_SESSION_BYTES]) {
    return context && context->task_bind_session && task && session
        ? context->task_bind_session(context->user,task,session) : 0;
}

int hop_xfer_sender_context_task_set_state(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state) {
    return context && context->task_set_state && task
        ? context->task_set_state(context->user,task,(int)state) : 0;
}

int hop_xfer_sender_context_task_begin_completion(
    const hop_xfer_sender_context *context, const hop_xfer_task *task) {
    return context && context->task_begin_completion && task
        ? context->task_begin_completion(context->user,task) : 0;
}

void hop_xfer_sender_context_task_finish(
    const hop_xfer_sender_context *context, hop_xfer_task *task,
    hop_xfer_task_state state) {
    if (context && context->task_finish && task)
        context->task_finish(context->user,task,(int)state);
}

int hop_xfer_sender_context_task_dispose(
    const hop_xfer_sender_context *context, hop_xfer_task *task) {
    if (!task || !context || !context->task_dispose) return 0;
    context->task_dispose(context->user,task);
    return !task->initialized && !task->id;
}

void hop_xfer_sender_context_status_start_job(
    const hop_xfer_sender_context *context, const void *job) {
    if (context && context->status_start_job)
        context->status_start_job(context->user,job);
}

void hop_xfer_sender_context_status_end(
    const hop_xfer_sender_context *context, uint32_t batch, int phase,
    const char *detail) {
    if (context && context->status_end)
        context->status_end(context->user,batch,phase,detail);
}

void hop_xfer_sender_context_status_file_phase(
    const hop_xfer_sender_context *context, uint32_t batch, int index,
    int phase, uint32_t attempts) {
    if (context && context->status_file_phase)
        context->status_file_phase(context->user,batch,index,phase,attempts);
}

void hop_xfer_sender_context_status_stage_time(
    const hop_xfer_sender_context *context, uint32_t batch, int stage,
    uint64_t elapsed_ns) {
    if (context && context->status_stage_time)
        context->status_stage_time(context->user,batch,stage,elapsed_ns);
}

void hop_xfer_sender_context_status_chunk_timed(
    const hop_xfer_sender_context *context, uint32_t batch, uint32_t id,
    uint64_t offset, uint32_t length, uint64_t read_ns, uint64_t crc_ns,
    uint64_t send_ns, uint64_t receive_ns) {
    if (context && context->status_chunk_timed)
        context->status_chunk_timed(context->user,batch,id,offset,length,
                                     read_ns,crc_ns,send_ns,receive_ns);
}

void hop_xfer_sender_context_logf(
    const hop_xfer_sender_context *context, int level,
    const char *format, ...) {
    if (!context || !context->log_message || !format) return;
    char message[512];
    va_list args;
    va_start(args,format);
    (void)vsnprintf(message,sizeof(message),format,args);
    va_end(args);
    context->log_message(context->user,level,message);
}
