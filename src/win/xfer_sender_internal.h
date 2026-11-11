#ifndef HOP_WIN_XFER_SENDER_INTERNAL_H
#define HOP_WIN_XFER_SENDER_INTERNAL_H
#include <winsock2.h>
#include <stddef.h>
#include <stdint.h>
#include "xfer.h"
#include "xfer_scanner.h"
#include "xfer_sender_threads.h"
#include "../core/xfer_rate.h"
#include "../core/xfer_tracking.h"
#include "../core/xfer_sender_context.h"
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

typedef struct xfer_job {
    struct xfer_job *next; xfer_job_owner owner; hop_session_id session;
    uint32_t batch; int is_image; hop_xfer_task task;
    hop_xfer_task_state pending_finish_state; int pending_finish_valid;
    int pending_status_end; int pending_status_phase; char pending_status_detail[96];
    hop_xfer_sender_context sender_context; intptr_t commit_socket; uint64_t commit_epoch;
    int n; xfer_entry *e; uint8_t *pixels; int image_payload_released;
    int w,h,stride,next_i,ok,attempt; uint64_t retry_at,give_up_at,scan_ns;
} xfer_job;
typedef struct {
    HWND hwnd; hop_config cfg; SOCKET listen_s,client; volatile LONG listening;
    hop_session_id session; unsigned long epoch; int peer_ready;
    CRITICAL_SECTION lock; HANDLE stop,q_evt,rate_timer;
    hop_win_sender_threads sender_threads; hop_xfer_sender_queue queue;
    int sync_initialized,shutting_down; xfer_job *shutdown_jobs;
    uint32_t batch_seq,chunk; hop_xfer_rate_limiter rate;
    hop_xfer_sender_active_job active_job; uint32_t cancel_batch; volatile LONG cancel_rx;
} hop_win_xfer_state;
typedef struct {
    CRITICAL_SECTION *state_lock,*tx_lock; SOCKET *client; int *peer_ready;
    void *ack_adapter; hop_session_id *session; unsigned long *epoch;
    HANDLE *stop,*rate_timer; uint32_t *abort_batch,*cancel_batch;
} sender_platform_context;
extern hop_win_xfer_state s_x;
extern SRWLOCK s_x_lifecycle_lock,s_status_lock;
extern CRITICAL_SECTION s_txlock;
#if defined(_MSC_VER)
extern __declspec(thread) unsigned long s_send_epoch;
#else
extern __thread unsigned long s_send_epoch;
#endif
extern hop_xfer_tracking s_send_status,s_receive_status;
extern sender_platform_context s_sender_platform;
extern hop_xfer_sender_context s_sender_context;
extern const hop_xfer_sender_queue_ops s_sender_queue_ops;
int sender_context_snapshot_session(void *,SOCKET,hop_session_id *,uint64_t *);
void ack_deliver(uint32_t,uint32_t,int);
int sender_context_session_current(void *,intptr_t,uint64_t);
int sender_context_write_frame(void *,intptr_t,uint64_t,const hop_xfer_sender_frame_part *,size_t);
void yield_cpu(void);
int send_small_with_context(const hop_xfer_sender_context *,SOCKET,uint8_t,const uint8_t *,uint32_t,uint32_t);
int send_small(SOCKET,uint8_t,const uint8_t *,uint32_t,uint32_t);
void status_chunk_timed(int,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t);
void status_stage_time(int,uint32_t,int,uint64_t);
void status_file_phase(int,uint32_t,int,int,uint32_t);
void status_end(int,uint32_t,int,const char *);
void status_start_job(const xfer_job *);
uint64_t rate_now_ns(void);
int sender_context_is_stopping(void *);
void sender_context_request_reconnect(void *);
int job_free(xfer_job *,xfer_job_owner);
void sender_context_init(void);
int sender_context_ack_create(void);
void sender_context_ack_destroy(void);
void sender_context_wake_ack(void);
int sender_shutdown_drain_jobs(void);
DWORD WINAPI send_thread(LPVOID);
void send_abort_msg(SOCKET,uint32_t,uint8_t,uint8_t);
void send_ack(SOCKET,uint32_t,uint32_t,int);
int xfer_platform_connected_locked(void);
#endif
