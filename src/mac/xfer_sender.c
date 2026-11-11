#include "xfer_sender_internal.h"
#include "../core/xfer_sender_ack_adapter.h"
#include "../core/xfer_sender_context_adapter.h"
#include "../core/xfer_sender_engine.h"
#include "../core/xfer_sender_scheduler.h"
#include "../core/xfer_sender_retry.h"
#include "../core/xfer_sender_lifecycle.h"
#include "../core/xfer_sender_worker_flow.h"
#include "../core/xfer_sender_worker.h"
#include "../core/xfer_ack_window.h"
#include "../core/xfer_buffer_pool.h"
#include "../core/xfer_budget.h"
#include "../core/xfer_task.h"
#include "../core/xfer_tracking.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread/qos.h>

int sender_context_ack_create(void);
void sender_context_ack_destroy(void);
void sender_context_wake_ack(void);

uint64_t rate_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t sender_context_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}

int sender_context_is_stopping(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    return !ctx || !ctx->stop || atomic_load(ctx->stop) != 0;
}

static int sender_context_is_cancelled(void *user, uint32_t batch,
                                      uint64_t task_id) {
    (void)user;
    (void)batch;
    /* Task Manager owns cancellation state. Do not take the queue lock here:
     * queue_take_cancelled calls this while already holding that lock. */
    return task_id && hop_xfer_task_cancel_requested(task_id);
}

static int sender_context_is_batch_cancelled(void *user, uint32_t batch) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->queue_lock || !ctx->cancel_batch) return 0;
    pthread_mutex_lock(ctx->queue_lock);
    int cancelled = *ctx->cancel_batch == batch;
    pthread_mutex_unlock(ctx->queue_lock);
    return cancelled;
}

static int sender_context_peer_aborted(void *user, uint32_t batch) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->queue_lock || !ctx->abort_batch) return 0;
    pthread_mutex_lock(ctx->queue_lock);
    int aborted = *ctx->abort_batch == batch;
    pthread_mutex_unlock(ctx->queue_lock);
    return aborted;
}

/* wait_ns is an interruptible full-duration wait. Long waits are split into
 * short slices so shutdown is observed promptly without reporting early success. */
static int sender_context_wait_ns(void *user, uint64_t duration_ns) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->stop || sender_context_is_stopping(user)) return 0;
    uint64_t start = rate_now_ns();
    uint64_t deadline = duration_ns > UINT64_MAX - start
        ? UINT64_MAX : start + duration_ns;
    while (!sender_context_is_stopping(user)) {
        uint64_t now = rate_now_ns();
        if (now >= deadline) return 1;
        uint64_t remaining = deadline - now;
        if (remaining > 10000000ull) remaining = 10000000ull;
        struct timespec ts = {
            (time_t)(remaining / 1000000000ull),
            (long)(remaining % 1000000000ull)
        };
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR || sender_context_is_stopping(user)) return 0;
        }
    }
    return 0;
}

static int sender_context_task_bind_session(
    void *user, const hop_xfer_task *task,
    const uint8_t session[HOP_TASK_SESSION_BYTES]) {
    (void)user;
    return hop_xfer_task_bind_session(task, session);
}

static int sender_context_task_set_state(void *user, hop_xfer_task *task,
                                         int state) {
    (void)user;
    return hop_xfer_task_set_state(task, (hop_xfer_task_state)state);
}

static int sender_context_task_begin_completion(void *user,
                                                 const hop_xfer_task *task) {
    (void)user;
    return hop_xfer_task_begin_completion(task);
}

static void sender_context_task_finish(void *user, hop_xfer_task *task,
                                       int outcome) {
    (void)user;
    (void)hop_xfer_task_finish(task, (hop_xfer_task_state)outcome);
}

static void sender_context_task_dispose(void *user, hop_xfer_task *task) {
    (void)user;
    hop_xfer_task_dispose(task);
}

static void sender_context_status_stage_time(void *user, uint32_t batch,
                                              int stage, uint64_t elapsed_ns) {
    (void)user;
    status_stage_time(HOP_XFER_DIR_SEND, batch, stage, elapsed_ns);
}

static void sender_context_status_start_job(void *user, const void *job) {
    (void)user;
    if (job) status_start_job((const xf_job *)job);
}

static void sender_context_status_end(void *user, uint32_t batch,
                                      int phase, const char *detail) {
    (void)user;
    status_end(HOP_XFER_DIR_SEND, batch, phase, detail);
}

static void sender_context_status_file_phase(void *user, uint32_t batch,
                                             int index, int phase,
                                             uint32_t attempts) {
    (void)user;
    status_file_phase(HOP_XFER_DIR_SEND, batch, index, phase, attempts);
}

static void sender_context_status_chunk_timed(
    void *user, uint32_t batch, uint32_t id, uint64_t offset,
    uint32_t length, uint64_t read_ns, uint64_t crc_ns,
    uint64_t send_ns, uint64_t receive_ns) {
    (void)user;
    status_chunk_timed(HOP_XFER_DIR_SEND, batch, id, offset, length,
                       read_ns, crc_ns, send_ns, receive_ns);
}

static void sender_context_log_message(void *user, int level,
                                       const char *message) {
    (void)user;
    hop_log((hop_log_level)level, "%s", message ? message : "");
}

void sender_context_request_reconnect(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->session_lock || !ctx->sock || !ctx->peer_ready) return;
    pthread_mutex_lock(ctx->session_lock);
    *ctx->peer_ready = 0;
    if (*ctx->sock >= 0) shutdown(*ctx->sock, SHUT_RDWR);
    pthread_mutex_unlock(ctx->session_lock);
}

void sender_context_init(void) {
    memset(&s_sender_platform, 0, sizeof(s_sender_platform));
    s_sender_platform.session_lock = &s_x.slock;
    s_sender_platform.queue_lock = &s_x.qlock;
    s_sender_platform.tx_lock = &s_txlock;
    s_sender_platform.sock = &s_x.sock;
    s_sender_platform.peer_ready = &s_x.peer_ready;
    s_sender_platform.session = &s_x.session;
    s_sender_platform.epoch = &s_x.epoch;
    s_sender_platform.stop = &s_x.stop;
    s_sender_platform.abort_batch = &s_x.active_job.abort_batch;
    s_sender_platform.cancel_batch = &s_x.cancel_batch;
    memset(&s_sender_context, 0, sizeof(s_sender_context));
    s_sender_context.user = &s_sender_platform;
    s_sender_context.now_ns = sender_context_now_ns;
    s_sender_context.wait_ns = sender_context_wait_ns;
    s_sender_context.is_stopping = sender_context_is_stopping;
    s_sender_context.is_cancelled = sender_context_is_cancelled;
    s_sender_context.is_batch_cancelled = sender_context_is_batch_cancelled;
    s_sender_context.peer_aborted = sender_context_peer_aborted;
    s_sender_context.request_reconnect = sender_context_request_reconnect;
    s_sender_context.ack_register = hop_xfer_sender_ack_adapter_register;
    s_sender_context.ack_mark_sent = hop_xfer_sender_ack_adapter_mark_sent;
    s_sender_context.ack_set_deadline = hop_xfer_sender_ack_adapter_set_deadline;
    s_sender_context.ack_wait_take = hop_xfer_sender_ack_adapter_wait_take;
    s_sender_context.ack_unregister = hop_xfer_sender_ack_adapter_unregister;
    s_sender_context.ack_unregister_batch = hop_xfer_sender_ack_adapter_unregister_batch;
    s_sender_context.ack_deliver = hop_xfer_sender_ack_adapter_deliver;
    s_sender_context.ack_wake = hop_xfer_sender_ack_adapter_wake;
    s_sender_context.session_current = sender_context_session_current;
    s_sender_context.snapshot_session = sender_context_snapshot_session;
    s_sender_context.write_frame = sender_context_write_frame;
    s_sender_context.task_bind_session = sender_context_task_bind_session;
    s_sender_context.task_set_state = sender_context_task_set_state;
    s_sender_context.task_begin_completion = sender_context_task_begin_completion;
    s_sender_context.task_finish = sender_context_task_finish;
    s_sender_context.task_dispose = sender_context_task_dispose;
    s_sender_context.status_start_job = sender_context_status_start_job;
    s_sender_context.status_end = sender_context_status_end;
    s_sender_context.status_file_phase = sender_context_status_file_phase;
    s_sender_context.status_stage_time = sender_context_status_stage_time;
    s_sender_context.status_chunk_timed = sender_context_status_chunk_timed;
    s_sender_context.log_message = sender_context_log_message;
}



static void rate_feedback(uint64_t ack_rtt_us) {
    uint64_t before = s_x.rate.current_rate_bytes_per_sec;
    uint32_t pressure = hop_xfer_rate_feedback_from_budget(&s_x.rate, ack_rtt_us);
    if (s_x.rate.current_rate_bytes_per_sec < before) {
        hop_log(HOP_LOG_INFO, "xfer: adaptive send rate %.1f MB/s (ack %.1f ms, local rx queue %u/1000)",
                (double)s_x.rate.current_rate_bytes_per_sec / 1048576.0,
                (double)ack_rtt_us / 1000.0, pressure);
    }
}

static int send_chunk(const hop_xfer_sender_context *context,
                       int fd,
                       uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                       const uint8_t *data, uint32_t len, uint32_t crc,
                       uint64_t read_ns, uint64_t crc_ns) {
    if (!context) context = &s_sender_context;
    hop_xfer_sender_chunk_ops ops = {
        context, &s_x.rate, (intptr_t)fd, tx,
        (uint32_t)(CHUNK_HDR + s_x.chunk), rate_feedback
    };
    return hop_xfer_sender_send_chunk(&ops, batch, id, off, data, len,
                                      crc, read_ns, crc_ns);
}

/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  HOP_XFER_SENDER_ACK_WAIT_TIMEOUT
#define XFER_SEND_DEAD    HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD
#define XFER_LOCAL_IO     HOP_XFER_SENDER_ACK_WAIT_LOCAL_IO
#define XFER_CANCELLED    HOP_XFER_SENDER_ACK_WAIT_CANCELLED
#define XFER_PEER_ABORT   HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT
#define XFER_ACK_PENDING  (-6)
#define SENDER_ACK_POLL_INTERVAL_MS 100u

static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L); }

int sender_context_ack_create(void) {
    if (s_sender_platform.ack_adapter) return 0;
    hop_xfer_sender_ack_adapter *adapter =
        hop_xfer_sender_ack_adapter_create(sender_context_now_ns, NULL);
    if (!adapter) return 0;
    s_sender_platform.ack_adapter = adapter;
    s_sender_context.ack_user = adapter;
    return 1;
}
void sender_context_ack_destroy(void) {
    hop_xfer_sender_ack_adapter_destroy(
        (hop_xfer_sender_ack_adapter *)s_sender_platform.ack_adapter);
    s_sender_platform.ack_adapter = NULL;
    s_sender_context.ack_user = NULL;
}
void sender_context_wake_ack(void) {
    if (s_sender_context.ack_wake && s_sender_context.ack_user)
        s_sender_context.ack_wake(s_sender_context.ack_user);
}
static int ack_register(const hop_xfer_sender_context *context,
                        uint32_t batch, uint32_t id) {
    return context && context->ack_user && context->ack_register &&
           context->ack_register(context->ack_user, batch, id);
}
static int ack_mark_sent(const hop_xfer_sender_context *context,
                         uint32_t batch, uint32_t id) {
    return context && context->ack_user && context->ack_mark_sent &&
           context->ack_mark_sent(context->ack_user, batch, id);
}
static int ack_set_deadline(const hop_xfer_sender_context *context,
                            uint32_t batch, uint32_t id, uint64_t deadline_ns) {
    return context && context->ack_user && context->ack_set_deadline &&
           context->ack_set_deadline(context->ack_user, batch, id, deadline_ns);
}
static void ack_unregister(const hop_xfer_sender_context *context,
                           uint32_t batch, uint32_t id) {
    if (context && context->ack_user && context->ack_unregister)
        context->ack_unregister(context->ack_user, batch, id);
}
static void ack_unregister_batch(const hop_xfer_sender_context *context,
                                 uint32_t batch) {
    if (context && context->ack_user && context->ack_unregister_batch)
        context->ack_unregister_batch(context->ack_user, batch);
}
void ack_deliver(uint32_t batch, uint32_t id, int status) {
    if (s_sender_context.ack_user && s_sender_context.ack_deliver)
        (void)s_sender_context.ack_deliver(s_sender_context.ack_user,
                                           batch, id, status);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */
#define JOB_COMMIT_UNKNOWN 4 /* receiver outcome is unknown */
#define JOB_COMMIT_REJECTED 5 /* receiver explicitly rejected completion */

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg_with_context(
    const hop_xfer_sender_context *context, int fd,
    uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = HOP_ABORT_TO_RECEIVER;
    send_small_with_context(context, fd, HOP_MSG_XFER_ABORT,
                            ab, sizeof(ab), batch);
}

void send_abort_msg(int fd, uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    ab[5] = HOP_ABORT_TO_RECEIVER;
    send_small(fd, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 取当前连接再发 ABORT。sock 由 slock 保护，不能裸读。
 * 锁序固定 slock -> txlock，不会和发送线程反序。 */
void send_abort(uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = HOP_ABORT_TO_SENDER;

    pthread_mutex_lock(&s_x.slock);
    int fd = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (fd >= 0) send_small(fd, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
void send_ack(int fd, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(fd, HOP_MSG_FILE_ACK, b, 9, batch);
}

typedef struct {
    int fd;
    uint8_t *tx;
    xf_job *job;
} sender_file_engine_call;


static int sender_engine_file_open(void *user, const void *platform_entry,
                                   intptr_t *file_handle, uint32_t *system_error) {
    (void)user;
    const xf_entry *entry = (const xf_entry *)platform_entry;
    int guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
    int fd = open(entry->full, O_RDONLY);
    uint32_t error = fd < 0 ? (uint32_t)errno : 0;
    hop_watchdog_end(guard);
    if (system_error) *system_error = error;
    if (fd < 0) return 0;
    *file_handle = (intptr_t)fd;
    return 1;
}

static hop_xfer_sender_read_result sender_engine_file_read(
    void *user, intptr_t file_handle, uint8_t *buffer, uint32_t request,
    uint32_t *bytes_read, uint32_t *system_error) {
    (void)user;
    ssize_t got;
    uint32_t error = 0;
    do {
        int guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
        got = read((int)file_handle, buffer, request);
        error = got < 0 ? (uint32_t)errno : 0;
        hop_watchdog_end(guard);
    } while (got < 0 && error == EINTR);
    if (system_error) *system_error = error;
    if (got < 0) return HOP_XFER_SENDER_READ_ERROR;
    if (bytes_read) *bytes_read = (uint32_t)got;
    return got == 0 ? HOP_XFER_SENDER_READ_EOF : HOP_XFER_SENDER_READ_OK;
}

static void sender_engine_file_close(void *user, intptr_t file_handle) {
    (void)user;
    close((int)file_handle);
}













static const void *sender_engine_entry_at(void *user, int index) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && index >= 0 && index < call->job->n ? &call->job->e[index] : NULL;
}
static const char *sender_engine_entry_path(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xf_entry *)entry)->rel : NULL;
}
static const char *sender_engine_entry_name(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xf_entry *)entry)->name : NULL;
}
static uint64_t sender_engine_entry_size(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xf_entry *)entry)->size : 0;
}
static uint64_t sender_engine_entry_mtime(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xf_entry *)entry)->mtime : 0;
}
static uint32_t sender_engine_entry_flags(void *user, const void *entry) {
    (void)user;
    return entry ? (uint32_t)((const xf_entry *)entry)->flags : 0;
}
static int sender_engine_send_message(void *user, intptr_t socket_handle,
                                      uint8_t type, const uint8_t *payload,
                                      uint32_t length, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && send_small_with_context(
        &call->job->sender_context, (int)socket_handle,
        type, payload, length, batch);
}
static int sender_engine_send_chunk(void *user, intptr_t socket_handle,
                                    uint8_t *tx_buffer, uint32_t batch,
                                    uint32_t id, uint64_t offset,
                                    const uint8_t *data, uint32_t length,
                                    uint32_t crc, uint64_t read_ns,
                                    uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && send_chunk(&call->job->sender_context,
        (int)socket_handle, tx_buffer,
        batch, id, offset, data, length, crc, read_ns, crc_ns);
}
static void sender_engine_yield_cpu(void *user) {
    (void)user;
    yield_cpu();
}














static void sender_engine_rate_feedback(void *user, uint64_t rtt_us) {
    (void)user;
    rate_feedback(rtt_us);
}





















static int run_job(int fd, uint8_t *tx, xf_job *j) {
    if (!j) return JOB_NET_DOWN;
    sender_file_engine_call call = {fd, tx, j};
    hop_xfer_sender_engine_adapter_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &call;
    ops.context = &j->sender_context;
    ops.socket_handle = (intptr_t)fd;
    ops.connection_epoch = s_send_epoch;
    ops.batch = j->batch;
    ops.task_id = j->task.id;
    ops.is_image = j->is_image;
    ops.entry_count = j->n;
    ops.next_index = &j->next_i;
    ops.ok_count = &j->ok;
    ops.image_pixels = j->pixels;
    ops.image_width = (uint32_t)j->w;
    ops.image_height = (uint32_t)j->h;
    ops.image_stride = (uint32_t)j->stride;
    ops.tx_buffer = tx;
    ops.chunk_size = s_x.chunk;
    ops.chunk_header_size = CHUNK_HDR;
    ops.ack_timeout_ns = (uint64_t)HOP_XFER_ACK_TIMEOUT_MS * 1000000ull;
    ops.ack_poll_interval_ms = SENDER_ACK_POLL_INTERVAL_MS;
    ops.max_attempts = HOP_XFER_FILE_RETRY_MAX;
    ops.directory_flag = HOP_FILE_FLAG_DIR;
    ops.file_ok_status = HOP_FILE_OK;
    ops.file_io_error_status = HOP_FILE_IO_ERROR;
    ops.file_begin_message = HOP_MSG_FILE_BEGIN;
    ops.file_end_message = HOP_MSG_FILE_END;
    ops.image_begin_message = HOP_MSG_IMAGE_BEGIN;
    ops.entry_at = sender_engine_entry_at;
    ops.entry_name = sender_engine_entry_name;
    ops.entry_path = sender_engine_entry_path;
    ops.entry_size = sender_engine_entry_size;
    ops.entry_mtime = sender_engine_entry_mtime;
    ops.entry_flags = sender_engine_entry_flags;
    ops.open_file = sender_engine_file_open;
    ops.read_file = sender_engine_file_read;
    ops.close_file = sender_engine_file_close;
    ops.send_message = sender_engine_send_message;
    ops.send_chunk = sender_engine_send_chunk;
    ops.yield_cpu = sender_engine_yield_cpu;
    ops.rate_feedback = sender_engine_rate_feedback;
    return hop_xfer_sender_engine_adapter_run(&ops);
}

/* Platform node accessors; queue ordering/removal policy lives in Core. */
static void *sender_queue_next(void *user, const void *node) {
    (void)user;
    return ((const xf_job *)node)->next;
}
static void sender_queue_set_next(void *user, void *node, void *next) {
    (void)user;
    ((xf_job *)node)->next = (xf_job *)next;
}
static int sender_queue_is_cancelled(void *user, const void *node) {
    (void)user;
    const xf_job *job = (const xf_job *)node;
    return hop_xfer_sender_context_task_cancel_requested(&job->sender_context, job->batch, &job->task);
}
static uint32_t sender_queue_batch(void *user, const void *node) {
    (void)user;
    return ((const xf_job *)node)->batch;
}
static uint64_t sender_queue_retry_at(void *user, const void *node) {
    (void)user;
    return ((const xf_job *)node)->retry_at;
}
static uint64_t sender_queue_give_up_at(void *user, const void *node) {
    (void)user;
    return ((const xf_job *)node)->give_up_at;
}
const hop_xfer_sender_queue_ops s_sender_queue_ops = {
    NULL, sender_queue_next, sender_queue_set_next, sender_queue_is_cancelled,
    sender_queue_batch, sender_queue_retry_at, sender_queue_give_up_at
};

static void job_defer_to_shutdown(xf_job *j, xfer_job_owner expected_owner) {
    if (!j) return;
    pthread_mutex_lock(&s_x.qlock);
    if (j->owner == XFER_JOB_OWNER_RELEASING) {
        j->owner = XFER_JOB_OWNER_SHUTDOWN;
        /* A Job already on shutdown_jobs stays at its current list position. */
        if (expected_owner != XFER_JOB_OWNER_SHUTDOWN) {
            j->next = s_x.shutdown_jobs;
            s_x.shutdown_jobs = j;
        }
    }
    pthread_mutex_unlock(&s_x.qlock);
}

static int job_claim_for_release(xf_job *j, xfer_job_owner expected_owner) {
    pthread_mutex_lock(&s_x.qlock);
    int actual_owner = (int)j->owner;
    int claimed = j->owner == expected_owner;
    if (claimed) j->owner = XFER_JOB_OWNER_RELEASING;
    pthread_mutex_unlock(&s_x.qlock);
    if (!claimed) {
        hop_log(HOP_LOG_ERROR, "xfer: refusing Job release by non-owner (batch=%u owner=%d expected=%d)",
                j->batch, actual_owner, (int)expected_owner);
    }
    return claimed;
}

static int job_release_task_resources(xf_job *j) {
    if (!j) return 0;
    /* The Job owns the pixel allocation and the Task owns its image-temp
     * reservation. Drop the allocation first, then return the exact ledger
     * bytes before Task terminalization (which rejects live temp bytes). */
    if (j->pixels) {
        free(j->pixels);
        j->pixels = NULL;
        j->image_payload_released = 1;
    } else if (j->is_image && !j->image_payload_released) {
        hop_log(HOP_LOG_ERROR, "xfer: image Job lost its pixel owner before Reservation release (batch=%u)", j->batch);
        return 0;
    }
    if (j->task.initialized) {
        uint64_t bytes = hop_xfer_task_image_temp_bytes(&j->task);
        if (bytes && !j->image_payload_released) {
            hop_log(HOP_LOG_ERROR, "xfer: image-temp Reservation has no owned pixel buffer (batch=%u task=%llu bytes=%llu)",
                    j->batch, (unsigned long long)j->task.id, (unsigned long long)bytes);
            return 0;
        }
        if (bytes && !hop_xfer_task_release_image_temp(&j->task, bytes)) {
            hop_log(HOP_LOG_ERROR, "xfer: Job image-temp Reservation release deferred (batch=%u task=%llu bytes=%llu)",
                    j->batch, (unsigned long long)j->task.id, (unsigned long long)bytes);
            return 0;
        }
    } else if (j->task.id) {
        return 0;
    }
    return 1;
}

int job_free(xf_job *j, xfer_job_owner expected_owner) {
    if (!j) return 1;
    if (!job_claim_for_release(j, expected_owner)) return -1;

    /* Preserve submitter-side failure intent even if Reservation return
     * has to be retried by Shutdown. */
    if ((j->task.initialized || j->task.id) &&
        expected_owner == XFER_JOB_OWNER_SUBMITTER && !j->pending_finish_valid) {
        j->pending_finish_state = HOP_XFER_TASK_FAILED;
        j->pending_finish_valid = 1;
    }

    if (!job_release_task_resources(j)) {
        job_defer_to_shutdown(j, expected_owner);
        hop_log(HOP_LOG_ERROR, "xfer: Job resource release deferred (batch=%u task=%llu)",
                j->batch, (unsigned long long)j->task.id);
        return 0;
    }

    if (j->task.initialized || j->task.id) {
        if (!j->task.initialized) {
            job_defer_to_shutdown(j, expected_owner);
            hop_log(HOP_LOG_ERROR, "xfer: refusing to release Job with uninitialized live Task handle (batch=%u task=%llu)",
                    j->batch, (unsigned long long)j->task.id);
            return 0;
        }
        if (!hop_xfer_task_is_terminal(&j->task)) {
            hop_xfer_task_state state = j->pending_finish_valid
                ? j->pending_finish_state
                : (expected_owner == XFER_JOB_OWNER_SUBMITTER
                    ? HOP_XFER_TASK_FAILED : HOP_XFER_TASK_INTERRUPTED);
            if (!j->pending_finish_valid &&
                hop_xfer_sender_context_job_cancel_requested(&j->sender_context,j->batch,j->task.id))
                state = HOP_XFER_TASK_CANCELLED;
            hop_xfer_sender_context_task_finish(&j->sender_context,&j->task,state);
            if (!hop_xfer_task_is_terminal(&j->task)) {
                job_defer_to_shutdown(j, expected_owner);
                hop_log(HOP_LOG_ERROR, "xfer: Job Task terminalization deferred (batch=%u task=%llu)",
                        j->batch, (unsigned long long)j->task.id);
                return 0;
            }
        }
        if (!hop_xfer_sender_context_task_dispose(&j->sender_context, &j->task) ||
            j->task.initialized || j->task.id) {
            job_defer_to_shutdown(j, expected_owner);
            hop_log(HOP_LOG_ERROR, "xfer: Job Task disposal deferred (batch=%u task=%llu)",
                    j->batch, (unsigned long long)j->task.id);
            return 0;
        }
    }

    if (j->pending_status_end) {
        hop_xfer_sender_context_status_end(&j->sender_context,j->batch,j->pending_status_phase,
                          j->pending_status_detail[0] ? j->pending_status_detail : "Transfer ended");
        j->pending_status_end=0;
    }
    int release_owner_ok = 0;
    pthread_mutex_lock(&s_x.qlock);
    if (j->owner == XFER_JOB_OWNER_RELEASING) {
        j->owner = XFER_JOB_OWNER_RELEASED;
        release_owner_ok = 1;
    }
    pthread_mutex_unlock(&s_x.qlock);
    if (!release_owner_ok) {
        job_defer_to_shutdown(j, expected_owner);
        hop_log(HOP_LOG_ERROR, "xfer: Job release ownership changed before free (batch=%u)",
                j->batch);
        return 0;
    }
    for (int i = 0; i < j->n; ++i) {
        free(j->e[i].full);
        free(j->e[i].name);
        free(j->e[i].rel);
    }
    free(j->e);
    free(j);
    return 1;
}

typedef struct { xf_job *job; int socket; } sender_worker_call;
static uint64_t sender_worker_now_ms(void *user) { (void)user; return now_ms(); }
static unsigned sender_worker_retry_delay_ms(void *user, uint32_t attempt) {
    (void)user; return hop_xfer_job_retry_delay_ms((int)attempt);
}
static int sender_worker_is_cancelled(void *user, uint32_t batch, uint64_t task_id) {
    sender_worker_call *call=(sender_worker_call *)user;
    return call&&call->job&&hop_xfer_sender_context_job_cancel_requested(&call->job->sender_context,batch,task_id);
}
static int sender_worker_is_stopping(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    return !call || !call->job || !call->job->sender_context.is_stopping ||
        call->job->sender_context.is_stopping(call->job->sender_context.user);
}
static int sender_worker_begin_completion(void *user, uint64_t task_id) {
    sender_worker_call *call=(sender_worker_call *)user;
    return call&&call->job&&call->job->task.id==task_id&&
        hop_xfer_sender_context_task_begin_completion(&call->job->sender_context,&call->job->task);
}
static int sender_worker_set_queued(void *user, uint64_t task_id) {
    sender_worker_call *call=(sender_worker_call *)user;
    return call&&call->job&&call->job->task.id==task_id&&
        hop_xfer_sender_context_task_set_state(&call->job->sender_context,&call->job->task,HOP_XFER_TASK_QUEUED);
}
static void sender_worker_store_retry_state(void *user,const hop_xfer_sender_worker_job *state) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job||!state)return;
    call->job->attempt=(int)state->attempt;
    call->job->retry_at=state->retry_at_ms;
    call->job->give_up_at=state->give_up_at_ms;
}
static int sender_worker_requeue_front(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return 0;
    pthread_mutex_lock(&s_x.qlock);
    int queued=call->job->owner==XFER_JOB_OWNER_WORKER &&
        !sender_worker_is_stopping(call) &&
        hop_xfer_sender_queue_push_front(&s_x.queue,&s_sender_queue_ops,call->job);
    if (queued) {
        (void)hop_xfer_sender_active_job_clear_if_matches(
            &s_x.active_job,call->job->batch,call->job->task.id);
        call->job->owner=XFER_JOB_OWNER_QUEUE;
        pthread_cond_signal(&s_x.qcond);
    }
    pthread_mutex_unlock(&s_x.qlock);
    return queued;
}
static void sender_worker_clear_active_job(xf_job *job) {
    if (!job) return;
    pthread_mutex_lock(&s_x.qlock);
    (void)hop_xfer_sender_active_job_clear_if_matches(
        &s_x.active_job,job->batch,job->task.id);
    pthread_mutex_unlock(&s_x.qlock);
}
static int sender_worker_peer_cancelled_and_clear_current(void *user,uint32_t batch,uint64_t task_id) {
    (void)user;
    pthread_mutex_lock(&s_x.qlock);
    int cancelled=hop_xfer_sender_active_job_take_cancelled_and_clear(
        &s_x.active_job,batch,task_id,HOP_ABORT_CANCEL);
    pthread_mutex_unlock(&s_x.qlock);
    return cancelled;
}
static void sender_worker_send_abort(void *user,uint32_t batch) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job)if(call->socket>=0)send_abort_msg_with_context(&call->job->sender_context,call->socket,
        batch,HOP_ABORT_CANCEL);
}
static hop_xfer_sender_job_result sender_worker_send_done(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN;
    return (hop_xfer_sender_job_result)sender_commit_adapter_run(call->job);
}
static void sender_worker_task_finish(void *user,hop_xfer_task_state state) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return;
    xf_job *job=call->job;
    job->pending_finish_state=state;
    job->pending_finish_valid=1;
    job->pending_status_end=1;
    if(job_release_task_resources(job))
        hop_xfer_sender_context_task_finish(&job->sender_context,&job->task,state);
}
static void sender_worker_status_end(void *user,uint32_t batch,int phase,const char *detail) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job){
        xf_job *job=call->job;
        /* Publish only after job_free confirms Task disposal. A terminal Task
         * can still retain its Reservation handle while disposal is retried. */
        (void)batch;
        job->pending_status_end=1;
        job->pending_status_phase=phase;
        snprintf(job->pending_status_detail,sizeof(job->pending_status_detail),"%s",
                 detail?detail:"Transfer terminalization pending");
    }
}
static void sender_worker_dispose_job(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job){
        sender_worker_clear_active_job(call->job);
        (void)job_free(call->job,XFER_JOB_OWNER_WORKER);
        call->job=NULL; /* Worker relinquishes the pointer; Shutdown owns any deferred release. */
    }
}
static void sender_worker_log_event(void *user,hop_xfer_sender_worker_log_event event,
    const hop_xfer_sender_worker_job *state,unsigned delay_ms) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job||!state)return;
    const hop_xfer_sender_context *context=&call->job->sender_context;
    switch(event) {
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_INVALID:
            hop_xfer_sender_context_logf(context,HOP_LOG_ERROR,"xfer: invalid retry policy input for batch %u",state->batch);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_SCHEDULED:
            hop_xfer_sender_context_logf(context,HOP_LOG_WARN,"xfer: batch %u interrupted at entry %d, retry %u/%u in %u ms",
                state->batch,state->next_index,state->attempt,HOP_XFER_JOB_RETRY_MAX-1,delay_ms);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_STATE_FAILED:
            hop_xfer_sender_context_logf(context,HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                (unsigned long long)state->task_id);break;
        case HOP_XFER_SENDER_WORKER_LOG_REQUEUE_FAILED:
            hop_xfer_sender_context_logf(context,HOP_LOG_ERROR,"xfer: failed to requeue batch %u after reconnect",state->batch);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_EXHAUSTED:
            hop_xfer_sender_context_logf(context,HOP_LOG_ERROR,"xfer: batch %u abandoned after %u attempt(s), %d/%d delivered",
                state->batch,state->attempt,state->ok_count,state->entry_count);break;
        case HOP_XFER_SENDER_WORKER_LOG_PARTIAL_DONE:
            hop_xfer_sender_context_logf(context,HOP_LOG_WARN,"xfer: batch %u finished %d/%d",
                state->batch,state->ok_count,state->entry_count);break;
        default: break;
    }
}
static hop_xfer_sender_worker_action sender_worker_finish_job(
    xf_job *job,int socket,int result,int completion_path,int peer_cancelled) {
    sender_worker_call call={job,socket};
    hop_xfer_sender_worker_job state;
    memset(&state,0,sizeof(state));
    state.batch=job->batch;state.task_id=job->task.id;
    state.attempt=job->attempt>0?(uint32_t)job->attempt:0u;
    state.retry_at_ms=job->retry_at;state.give_up_at_ms=job->give_up_at;
    state.next_index=job->next_i;state.ok_count=job->ok;state.entry_count=job->n;
    state.total_count=(int)hop_xfer_job_total(job->is_image,job->n);
    state.result=(hop_xfer_sender_job_result)result;
    state.completion_path=completion_path;state.peer_cancelled=peer_cancelled;
    hop_xfer_sender_worker_ops ops;
    memset(&ops,0,sizeof(ops));
    ops.user=&call;ops.max_attempts=HOP_XFER_JOB_RETRY_MAX;
    ops.reconnect_budget_ms=HOP_XFER_RECONNECT_BUDGET_MS;
    ops.now_ms=sender_worker_now_ms;ops.retry_delay_ms=sender_worker_retry_delay_ms;
    ops.is_cancelled=sender_worker_is_cancelled;ops.is_stopping=sender_worker_is_stopping;ops.begin_completion=sender_worker_begin_completion;
    ops.set_queued=sender_worker_set_queued;ops.store_retry_state=sender_worker_store_retry_state;
    ops.requeue_front=sender_worker_requeue_front;
    ops.peer_cancelled_and_clear_current=sender_worker_peer_cancelled_and_clear_current;
    ops.send_abort=sender_worker_send_abort;ops.send_done=sender_worker_send_done;
    ops.task_finish=sender_worker_task_finish;ops.status_end=sender_worker_status_end;
    ops.dispose_job=sender_worker_dispose_job;ops.log_event=sender_worker_log_event;
    hop_xfer_sender_worker_action action =
        hop_xfer_sender_worker_finish_job(&ops,&state);
    if (action == HOP_XFER_SENDER_WORKER_INVALID) {
        sender_worker_clear_active_job(job);
        job->pending_finish_state=HOP_XFER_TASK_FAILED;
        job->pending_finish_valid=1;
        job->pending_status_end=1;
        job->pending_status_phase=HOP_XFER_FAILED;
        snprintf(job->pending_status_detail,sizeof(job->pending_status_detail),"%s",
                 "Invalid sender worker context");
        (void)job_free(job,XFER_JOB_OWNER_WORKER);
        return HOP_XFER_SENDER_WORKER_FINISHED;
    }
    return action;
}
int sender_shutdown_drain_jobs(void) {
    pthread_mutex_lock(&s_x.qlock);
    /* No worker remains. Clear any active identity left by an interrupted
     * retry before Shutdown becomes the sole owner of every queued Job. */
    memset(&s_x.active_job,0,sizeof(s_x.active_job));
    xf_job *queued=(xf_job *)hop_xfer_sender_queue_detach_all(&s_x.queue);
    if (queued) {
        xf_job *tail=queued;
        for (xf_job *p=queued;p;p=p->next) {
            p->owner=XFER_JOB_OWNER_SHUTDOWN;
            tail=p;
        }
        tail->next=s_x.shutdown_jobs;
        s_x.shutdown_jobs=queued;
    }
    pthread_mutex_unlock(&s_x.qlock);

    while (s_x.shutdown_jobs) {
        xf_job *job=s_x.shutdown_jobs;
        xf_job *next=job->next;
        int terminal_was_pending=!hop_xfer_task_is_terminal(&job->task);
        int was_cancelled=hop_xfer_sender_context_job_cancel_requested(&job->sender_context,
                                                       job->batch,job->task.id);
        if (terminal_was_pending && !job->pending_finish_valid) {
            job->pending_finish_state=was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_INTERRUPTED;
            job->pending_finish_valid=1;
        }
        if (terminal_was_pending && !job->pending_status_end) {
            job->pending_status_end=1;
            job->pending_status_phase=was_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED;
            snprintf(job->pending_status_detail,sizeof(job->pending_status_detail),"%s",
                     was_cancelled?"Cancelled":"Transfer interrupted during shutdown");
        }
        int released=job_free(job,XFER_JOB_OWNER_SHUTDOWN);
        if (released!=1) return 0; /* retain the list head and its Reservation */
        s_x.shutdown_jobs=next;
    }
    return 1;
}

typedef struct {
    int peer_ready_snapshot;
} sender_scheduler_state;

static int sender_scheduler_begin_pass(void *user, int *peer_ready,
                                       uint64_t *now, uint32_t *cancel_batch) {
    sender_scheduler_state *state=(sender_scheduler_state *)user;
    if (!state || !peer_ready || !now || !cancel_batch) return 0;
    /* Preserve the lock order: snapshot connection state before taking qlock. */
    pthread_mutex_lock(&s_x.slock);
    state->peer_ready_snapshot=s_x.peer_ready;
    pthread_mutex_unlock(&s_x.slock);

    pthread_mutex_lock(&s_x.qlock);
    *peer_ready=state->peer_ready_snapshot;
    *now=now_ms();
    *cancel_batch=s_x.cancel_batch;
    return 1; /* qlock remains held until end_pass */
}
static int sender_scheduler_is_stopping(void *user) {
    (void)user;
    return sender_context_is_stopping(&s_sender_platform);
}
static int sender_scheduler_wait_locked(void *user, uint32_t timeout_ms) {
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    ts.tv_sec += (time_t)(timeout_ms/1000u);
    ts.tv_nsec += (long)(timeout_ms%1000u)*1000000L;
    if (ts.tv_nsec>=1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec-=1000000000L;
    }
    int rc=pthread_cond_timedwait(&s_x.qcond,&s_x.qlock,&ts);
    if (rc!=0 && rc!=ETIMEDOUT) {
        hop_log(HOP_LOG_WARN,"xfer: sender queue condition wait failed; falling back to bounded poll");
        pthread_mutex_unlock(&s_x.qlock);
        struct timespec pause={(time_t)(timeout_ms/1000u),
                               (long)(timeout_ms%1000u)*1000000L};
        nanosleep(&pause,NULL);
        pthread_mutex_lock(&s_x.qlock);
    }
    return 1;
}
static void sender_scheduler_claim_locked(void *user, void *node, int expired) {
    (void)user;
    xf_job *job=(xf_job *)node;
    job->owner=XFER_JOB_OWNER_WORKER;
    if (!expired && !hop_xfer_sender_active_job_begin(
            &s_x.active_job,job->batch,job->task.id))
        hop_log(HOP_LOG_ERROR,"xfer: failed to publish active Sender Job (batch=%u task=%llu)",
                job->batch,(unsigned long long)job->task.id);
}
static void sender_scheduler_end_pass(void *user) {
    (void)user;
    pthread_mutex_unlock(&s_x.qlock);
}


static int sender_worker_view_job_adapter(void *u,void *p,hop_xfer_sender_worker_job_view *v){
    (void)u;xf_job *j=p;if(!j||!v)return 0;v->platform_job=j;v->context=&j->sender_context;
    v->task=&j->task;v->session=&j->session;v->next_index=&j->next_i;v->ok_count=&j->ok;
    v->pending_finish_state=&j->pending_finish_state;v->pending_finish_valid=&j->pending_finish_valid;
    v->pending_status_end=&j->pending_status_end;v->pending_status_phase=&j->pending_status_phase;
    v->pending_status_detail=j->pending_status_detail;v->pending_status_detail_capacity=sizeof(j->pending_status_detail);
    v->batch=j->batch;return 1;
}
static int sender_worker_snapshot_connection(void *u,intptr_t *socket,uint64_t *epoch,hop_session_id *session,int *connected){
    (void)u;if(!socket||!epoch||!session||!connected)return 0;pthread_mutex_lock(&s_x.slock);
    int fd=s_x.peer_ready?s_x.sock:-1;*session=s_x.session;*epoch=(uint64_t)s_x.epoch;s_send_epoch=s_x.epoch;
    pthread_mutex_unlock(&s_x.slock);*socket=(intptr_t)fd;*connected=fd>=0;return 1;
}
static int sender_worker_status_should_start(void *u,uint32_t batch){
    (void)u;pthread_mutex_lock(&s_status_lock);int show=!s_send_status.status.active||s_send_status.status.batch==batch;
    pthread_mutex_unlock(&s_status_lock);return show;
}
static void sender_worker_status_reset(void *u){
    (void)u;pthread_mutex_lock(&s_status_lock);memset(&s_send_status,0,sizeof(s_send_status));pthread_mutex_unlock(&s_status_lock);
}
static void sender_worker_clear_active_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;pthread_mutex_lock(&s_x.qlock);(void)hop_xfer_sender_active_job_clear_if_matches(&s_x.active_job,batch,task);
    pthread_mutex_unlock(&s_x.qlock);
}
static void sender_worker_send_abort_adapter(void *u,const hop_xfer_sender_context *ctx,intptr_t socket,uint64_t epoch,uint32_t batch){
    (void)u;(void)epoch;if(socket>=0)send_abort_msg_with_context(ctx,(int)socket,batch,HOP_ABORT_CANCEL);
}
static hop_xfer_sender_job_result sender_worker_run_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,uint8_t *tx){
    (void)u;(void)epoch;if(socket<0)return HOP_XFER_SENDER_JOB_NET_DOWN;
    return (hop_xfer_sender_job_result)run_job((int)socket,tx,(xf_job *)job);
}
static int sender_worker_take_peer_cancelled_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;pthread_mutex_lock(&s_x.qlock);int c=hop_xfer_sender_active_job_take_cancelled_and_clear(
        &s_x.active_job,batch,task,HOP_ABORT_CANCEL);pthread_mutex_unlock(&s_x.qlock);return c;
}
static hop_xfer_sender_worker_action sender_worker_finish_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,
    hop_xfer_sender_job_result result,int completion,int peer_cancelled){
    (void)u;(void)epoch;return sender_worker_finish_job((xf_job *)job,(int)socket,(int)result,completion,peer_cancelled);
}
static void sender_worker_release_job_adapter(void *u,void *job){(void)u;(void)job_free((xf_job *)job,XFER_JOB_OWNER_WORKER);}

void *send_thread(void *arg){
    (void)arg;pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
    sender_scheduler_state state;memset(&state,0,sizeof(state));
    hop_xfer_sender_scheduler_ops scheduler={&state,sender_scheduler_begin_pass,sender_scheduler_is_stopping,
        sender_scheduler_wait_locked,sender_scheduler_claim_locked,sender_scheduler_end_pass};
    hop_xfer_sender_worker_runtime_ops worker;memset(&worker,0,sizeof(worker));
    worker.user=&state;worker.queue=&s_x.queue;worker.queue_ops=&s_sender_queue_ops;worker.scheduler=scheduler;
    worker.poll_interval_ms=200;worker.chunk_size=s_x.chunk;worker.view_job=sender_worker_view_job_adapter;
    worker.is_stopping=sender_scheduler_is_stopping;worker.snapshot_connection=sender_worker_snapshot_connection;
    worker.status_should_start=sender_worker_status_should_start;worker.status_reset=sender_worker_status_reset;
    worker.clear_active=sender_worker_clear_active_adapter;worker.send_abort=sender_worker_send_abort_adapter;
    worker.run_job=sender_worker_run_job_adapter;worker.take_peer_cancelled_and_clear=sender_worker_take_peer_cancelled_adapter;
    worker.finish_job=sender_worker_finish_job_adapter;worker.release_job=sender_worker_release_job_adapter;
    return hop_xfer_sender_worker_run(&worker)==HOP_XFER_SENDER_WORKER_RUN_STOPPED?NULL:(void *)(uintptr_t)1;
}
