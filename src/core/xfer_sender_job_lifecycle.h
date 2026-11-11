#ifndef HOP_XFER_SENDER_JOB_LIFECYCLE_H
#define HOP_XFER_SENDER_JOB_LIFECYCLE_H
#include <stdint.h>
#include "xfer_sender_context.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
 HOP_XFER_SENDER_JOB_OWNER_SUBMITTER=0, HOP_XFER_SENDER_JOB_OWNER_QUEUE,
 HOP_XFER_SENDER_JOB_OWNER_WORKER, HOP_XFER_SENDER_JOB_OWNER_SHUTDOWN,
 HOP_XFER_SENDER_JOB_OWNER_RELEASING, HOP_XFER_SENDER_JOB_OWNER_RELEASED
} hop_xfer_sender_job_owner;
typedef struct {
 void *job; const hop_xfer_sender_context *context; hop_xfer_task *task;
 uint32_t batch; int is_image; uint8_t **pixels; int *image_payload_released;
 hop_xfer_task_state *pending_finish_state; int *pending_finish_valid;
 int *pending_status_end; int *pending_status_phase; char *pending_status_detail;
} hop_xfer_sender_job_release_view;
typedef struct {
 void *user;
 int (*claim)(void *,void *,hop_xfer_sender_job_owner,hop_xfer_sender_job_owner *);
 void (*defer_to_shutdown)(void *,void *,hop_xfer_sender_job_owner);
 int (*mark_released)(void *,void *);
 void (*free_storage)(void *,void *);
} hop_xfer_sender_job_release_ops;
typedef enum { HOP_XFER_SENDER_JOB_RELEASE_DEFERRED=0,
 HOP_XFER_SENDER_JOB_RELEASED=1, HOP_XFER_SENDER_JOB_RELEASE_NOT_OWNER=-1,
 HOP_XFER_SENDER_JOB_RELEASE_INVALID=-2 } hop_xfer_sender_job_release_result;
hop_xfer_sender_job_release_result hop_xfer_sender_job_release(
 const hop_xfer_sender_job_release_view *,const hop_xfer_sender_job_release_ops *,
 hop_xfer_sender_job_owner);
#ifdef __cplusplus
}
#endif
#endif
