#ifndef HOP_MAC_XFER_SENDER_INTERNAL_H
#define HOP_MAC_XFER_SENDER_INTERNAL_H
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include "xfer.h"
#include "xfer_scanner.h"
#include "xfer_sender_threads.h"
#include "../core/xfer_rate.h"
#include "../core/xfer_tracking.h"
#include "../core/xfer_sender_context.h"
#include "../core/xfer_sender_ack_wait.h"
#include "../core/xfer_sender_chunk.h"
#include "../core/xfer_sender_engine_adapter.h"
#include "../core/xfer_sender_queue.h"
#include "../core/xfer_sender_active_job.h"
#define CHUNK_HDR (25u + HOP_DATA_PREFIX)
typedef enum {
    XFER_JOB_OWNER_SUBMITTER = 0,
    XFER_JOB_OWNER_QUEUE,
    XFER_JOB_OWNER_WORKER,
    XFER_JOB_OWNER_SHUTDOWN,
    XFER_JOB_OWNER_RELEASING,
    XFER_JOB_OWNER_RELEASED
} xfer_job_owner;

typedef struct xf_job {
    struct xf_job *next; xfer_job_owner owner; hop_session_id session;
    uint32_t batch; int is_image; hop_xfer_task task;
    hop_xfer_task_state pending_finish_state; int pending_finish_valid;
    int pending_status_end; int pending_status_phase; char pending_status_detail[96];
    hop_xfer_sender_context sender_context; intptr_t commit_socket; uint64_t commit_epoch;
    int n; xf_entry *e; uint8_t *pixels; int image_payload_released;
    int w,h,stride,next_i,ok,attempt; uint64_t retry_at,give_up_at,scan_ns;
} xf_job;
typedef struct {
    int sock; hop_session_id session; unsigned long epoch; int peer_ready;
    pthread_mutex_t slock,qlock; pthread_cond_t qcond; _Atomic int stop;
    hop_xfer_sender_queue queue; uint32_t batch_seq,chunk;
    hop_xfer_rate_limiter rate; hop_mac_sender_threads sender_threads;
    int sync_initialized,shutting_down; xf_job *shutdown_jobs;
    hop_xfer_sender_active_job active_job; uint32_t cancel_batch; volatile int cancel_rx;
} hop_mac_xfer_state;
typedef struct {
    pthread_mutex_t *session_lock,*queue_lock,*tx_lock; int *sock,*peer_ready;
    void *ack_adapter; hop_session_id *session; unsigned long *epoch;
    _Atomic int *stop; uint32_t *abort_batch,*cancel_batch;
} sender_platform_context;
extern hop_mac_xfer_state s_x;
extern pthread_rwlock_t s_x_lifecycle_lock;
extern pthread_mutex_t s_status_lock,s_txlock;
extern _Thread_local unsigned long s_send_epoch;
extern hop_xfer_tracking s_send_status,s_receive_status;
extern sender_platform_context s_sender_platform;
extern hop_xfer_sender_context s_sender_context;
extern const hop_xfer_sender_queue_ops s_sender_queue_ops;
int sender_context_snapshot_session(void *,intptr_t,uint8_t [HOP_TASK_SESSION_BYTES],uint64_t *);
hop_xfer_sender_job_result sender_commit_adapter_run(const xf_job *);
void ack_deliver(uint32_t,uint32_t,int);
int sender_context_session_current(void *,intptr_t,uint64_t);
int sender_context_write_frame(void *,intptr_t,uint64_t,const hop_xfer_sender_frame_part *,size_t);
void yield_cpu(void);
int send_small_with_context(const hop_xfer_sender_context *,int,uint8_t,const uint8_t *,uint32_t,uint32_t);
int send_small(int,uint8_t,const uint8_t *,uint32_t,uint32_t);
void status_chunk_timed(int,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t);
void status_stage_time(int,uint32_t,int,uint64_t);
void status_file_phase(int,uint32_t,int,int,uint32_t);
void status_end(int,uint32_t,int,const char *);
void status_start_job(const xf_job *);
uint64_t rate_now_ns(void);
int sender_context_is_stopping(void *);
void sender_context_request_reconnect(void *);
int job_free(xf_job *,xfer_job_owner);
void sender_context_init(void);
int sender_context_ack_create(void);
void sender_context_ack_destroy(void);
void sender_context_wake_ack(void);
int sender_shutdown_drain_jobs(void);
void *send_thread(void *);
void send_abort_msg(int,uint32_t,uint8_t);
void send_abort(uint32_t,uint8_t);
void send_ack(int,uint32_t,uint32_t,int);
int xfer_platform_connected_locked(void);
#endif
