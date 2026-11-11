#include "xfer_sender_job_lifecycle.h"
#include "xfer_sender_context_adapter.h"
#include "hop.h"
#include <stdlib.h>
static int valid(const hop_xfer_sender_job_release_view *v,const hop_xfer_sender_job_release_ops *o){
 return v&&v->job&&v->context&&v->task&&v->pixels&&v->image_payload_released&&
 v->pending_finish_state&&v->pending_finish_valid&&v->pending_status_end&&
 v->pending_status_phase&&v->pending_status_detail&&o&&o->claim&&
 o->defer_to_shutdown&&o->mark_released&&o->free_storage;
}
static void log_release(const hop_xfer_sender_job_release_view *v,const char *msg){
 hop_xfer_sender_context_logf(v->context,HOP_LOG_ERROR,
  "xfer: %s (batch=%u task=%llu)",msg,v->batch,(unsigned long long)v->task->id);
}
static hop_xfer_sender_job_release_result defer(const hop_xfer_sender_job_release_view *v,
 const hop_xfer_sender_job_release_ops *o,hop_xfer_sender_job_owner owner,const char *msg){
 o->defer_to_shutdown(o->user,v->job,owner);log_release(v,msg);
 return HOP_XFER_SENDER_JOB_RELEASE_DEFERRED;
}
static int release_resources(const hop_xfer_sender_job_release_view *v){
 if(*v->pixels){free(*v->pixels);*v->pixels=NULL;*v->image_payload_released=1;}
 else if(v->is_image&&!*v->image_payload_released){
  log_release(v,"image Job lost its pixel owner before Reservation release");return 0;
 }
 if(v->task->initialized){
  uint64_t bytes=hop_xfer_task_image_temp_bytes(v->task);
  if(bytes&&!*v->image_payload_released){
   log_release(v,"image-temp Reservation has no owned pixel buffer");return 0;
  }
  if(bytes&&!hop_xfer_task_release_image_temp(v->task,bytes)){
   log_release(v,"Job image-temp Reservation release deferred");return 0;
  }
 }else if(v->task->id)return 0;
 return 1;
}
hop_xfer_sender_job_release_result hop_xfer_sender_job_release(
 const hop_xfer_sender_job_release_view *v,const hop_xfer_sender_job_release_ops *o,
 hop_xfer_sender_job_owner expected){
 if(!v||!v->job)return HOP_XFER_SENDER_JOB_RELEASED;
 if(!valid(v,o))return HOP_XFER_SENDER_JOB_RELEASE_INVALID;
 hop_xfer_sender_job_owner actual=expected;
 if(!o->claim(o->user,v->job,expected,&actual)){
  hop_xfer_sender_context_logf(v->context,HOP_LOG_ERROR,
   "xfer: refusing Job release by non-owner (batch=%u owner=%d expected=%d)",
   v->batch,(int)actual,(int)expected);
  return HOP_XFER_SENDER_JOB_RELEASE_NOT_OWNER;
 }
 if((v->task->initialized||v->task->id)&&expected==HOP_XFER_SENDER_JOB_OWNER_SUBMITTER&&
    !*v->pending_finish_valid){
  *v->pending_finish_state=HOP_XFER_TASK_FAILED;*v->pending_finish_valid=1;
 }
 if(!release_resources(v))return defer(v,o,expected,"Job resource release deferred");
 if(v->task->initialized||v->task->id){
  if(!v->task->initialized)return defer(v,o,expected,"refusing to release Job with uninitialized live Task handle");
  if(!hop_xfer_task_is_terminal(v->task)){
   hop_xfer_task_state state=*v->pending_finish_valid?*v->pending_finish_state:
    (expected==HOP_XFER_SENDER_JOB_OWNER_SUBMITTER?HOP_XFER_TASK_FAILED:HOP_XFER_TASK_INTERRUPTED);
   if(!*v->pending_finish_valid&&hop_xfer_sender_context_job_cancel_requested(v->context,v->batch,v->task->id))
    state=HOP_XFER_TASK_CANCELLED;
   hop_xfer_sender_context_task_finish(v->context,v->task,state);
   if(!hop_xfer_task_is_terminal(v->task))return defer(v,o,expected,"Job Task terminalization deferred");
  }
  if(!hop_xfer_sender_context_task_dispose(v->context,v->task)||v->task->initialized||v->task->id)
   return defer(v,o,expected,"Job Task disposal deferred");
 }
 if(*v->pending_status_end){
  hop_xfer_sender_context_status_end(v->context,v->batch,*v->pending_status_phase,
   v->pending_status_detail[0]?v->pending_status_detail:"Transfer ended");
  *v->pending_status_end=0;
 }
 if(!o->mark_released(o->user,v->job))
  return defer(v,o,expected,"Job release ownership changed before free");
 o->free_storage(o->user,v->job);return HOP_XFER_SENDER_JOB_RELEASED;
}
