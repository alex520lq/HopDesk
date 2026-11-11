#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

int sender_context_ack_create(void);
void sender_context_ack_destroy(void);
void sender_context_wake_ack(void);

uint64_t rate_now_ns(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    uint64_t q = (uint64_t)counter.QuadPart;
    uint64_t f = (uint64_t)frequency.QuadPart;
    return (q / f) * 1000000000ull + ((q % f) * 1000000000ull) / f;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static void rate_sleep_ns(uint64_t ns, HANDLE stop, HANDLE rate_timer) {
    if (rate_timer) {
        LARGE_INTEGER due;
        uint64_t units = (ns + 99ull) / 100ull; /* negative 100 ns relative due time */
        if (!units) units = 1;
        due.QuadPart = -(LONGLONG)units;
        if (SetWaitableTimer(rate_timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE waits[2] = {stop, rate_timer};
            (void)WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            return;
        }
    }
    /* Old Windows fallback: coarse waits for longer intervals, QPC yield for
     * sub-millisecond pacing. Modern Windows uses the high-resolution timer. */
    if (ns >= 1000000ull) {
        DWORD ms = (DWORD)(ns / 1000000ull);
        if (ms > 10) ms = 10;
        if (ms) Sleep(ms);
        return;
    }
    LARGE_INTEGER start, now, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    do {
        SwitchToThread();
        QueryPerformanceCounter(&now);
    } while (((uint64_t)(now.QuadPart - start.QuadPart) * 1000000000ull) /
             (uint64_t)frequency.QuadPart < ns);
}

static uint64_t sender_context_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}

int sender_context_is_stopping(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    return !ctx || !ctx->stop || !*ctx->stop ||
           WaitForSingleObject(*ctx->stop, 0) == WAIT_OBJECT_0;
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
    if (!ctx || !ctx->state_lock || !ctx->cancel_batch) return 0;
    EnterCriticalSection(ctx->state_lock);
    int cancelled = *ctx->cancel_batch == batch;
    LeaveCriticalSection(ctx->state_lock);
    return cancelled;
}

static int sender_context_peer_aborted(void *user, uint32_t batch) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->state_lock || !ctx->abort_batch) return 0;
    EnterCriticalSection(ctx->state_lock);
    int aborted = *ctx->abort_batch == batch;
    LeaveCriticalSection(ctx->state_lock);
    return aborted;
}

/* wait_ns waits for the full requested interval, but polls shutdown in short
 * slices even when the platform timer falls back to coarse Sleep/QPC pacing. */
static int sender_context_wait_ns(void *user, uint64_t duration_ns) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->stop || !ctx->rate_timer ||
        sender_context_is_stopping(user)) return 0;
    uint64_t start = rate_now_ns();
    uint64_t deadline = duration_ns > UINT64_MAX - start
        ? UINT64_MAX : start + duration_ns;
    for (;;) {
        if (sender_context_is_stopping(user)) return 0;
        uint64_t now = rate_now_ns();
        if (now >= deadline) return 1;
        uint64_t remaining = deadline - now;
        if (remaining > 10000000ull) remaining = 10000000ull;
        rate_sleep_ns(remaining, *ctx->stop, *ctx->rate_timer);
    }
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
    if (job) status_start_job((const xfer_job *)job);
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
    if (!ctx || !ctx->state_lock || !ctx->client || !ctx->peer_ready) return;
    EnterCriticalSection(ctx->state_lock);
    *ctx->peer_ready = 0;
    if (*ctx->client != INVALID_SOCKET) shutdown(*ctx->client, SD_BOTH);
    LeaveCriticalSection(ctx->state_lock);
}

void sender_context_init(void) {
    memset(&s_sender_platform, 0, sizeof(s_sender_platform));
    s_sender_platform.state_lock = &s_x.lock;
    s_sender_platform.tx_lock = &s_txlock;
    s_sender_platform.client = &s_x.client;
    s_sender_platform.peer_ready = &s_x.peer_ready;
    s_sender_platform.session = &s_x.session;
    s_sender_platform.epoch = &s_x.epoch;
    s_sender_platform.stop = &s_x.stop;
    s_sender_platform.rate_timer = &s_x.rate_timer;
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

static int rate_wait_for_chunk(const hop_xfer_sender_context *context,
                                uint32_t bytes) {
    if (!hop_xfer_sender_context_clock_valid(context)) return 0;
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(
            &s_x.rate, bytes, context->now_ns(context->user));
        if (delay == UINT64_MAX) {
            hop_xfer_sender_context_logf(context, HOP_LOG_ERROR, "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return !context->is_stopping(context->user);
        if (context->is_stopping(context->user)) return 0;
        if (!context->wait_ns(context->user, delay)) return 0;
    }
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

static int send_chunk(const hop_xfer_sender_context *context, SOCKET c,
                      uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc,
                      uint64_t read_ns, uint64_t crc_ns) {
    if (!context) context = &s_sender_context;
    if (!rate_wait_for_chunk(context, len)) {
        hop_xfer_sender_context_status_stage_time(context, batch, HOP_XFER_TIME_READ, read_ns);
        hop_xfer_sender_context_status_stage_time(context, batch, HOP_XFER_TIME_CRC, crc_ns);
        return 0;
    }
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    hop_session_id session;
    uint64_t epoch = 0;
    int current = sender_context_snapshot_session(context->user, c, &session, &epoch);
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + HOP_DATA_PREFIX + len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 5, id);
    hop_put_be64(tx + HOP_DATA_PREFIX + 9, off);
    hop_put_be32(tx + HOP_DATA_PREFIX + 17, len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 21, crc);
    if (current) hop_data_prefix(tx + HOP_HEADER_SIZE, &session, batch);
    hop_xfer_sender_frame_part parts[2];
    size_t part_count = 1;
    parts[0] = (hop_xfer_sender_frame_part){tx, CHUNK_HDR};
    if (data == tx + CHUNK_HDR) {
        parts[0].size = CHUNK_HDR + len;
    } else {
        parts[1] = (hop_xfer_sender_frame_part){data, len};
        part_count = 2;
    }
    uint64_t send_started = context->now_ns
        ? context->now_ns(context->user) : rate_now_ns();
    int ok = current && context->write_frame &&
             context->write_frame(context->user, (intptr_t)c, epoch,
                                  parts, part_count);
    uint64_t send_finished = context->now_ns
        ? context->now_ns(context->user) : rate_now_ns();
    uint64_t send_us = send_finished >= send_started
        ? (send_finished - send_started) / 1000ull : 0;
    hop_xfer_rate_note_send_time(&s_x.rate, len, send_us);
    int feedback_due = (s_x.rate.send_samples & 7u) == 0;
    if (feedback_due) rate_feedback(0);
    uint64_t send_ns = send_us * 1000ull;
    if (ok) {
        hop_xfer_sender_context_status_chunk_timed(context, batch, id, off, len,
                                  read_ns, crc_ns, send_ns, 0);
    } else {
        hop_xfer_sender_context_status_stage_time(context, batch, HOP_XFER_TIME_READ, read_ns);
        hop_xfer_sender_context_status_stage_time(context, batch, HOP_XFER_TIME_CRC, crc_ns);
        hop_xfer_sender_context_status_stage_time(context, batch, HOP_XFER_TIME_SEND, send_ns);
    }
    hop_watchdog_end(send_guard);
    return ok;
}

/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */
/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)
#define XFER_ACK_PENDING  (-6)

static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }

/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)
#define XFER_ACK_PENDING  (-6)

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
static int ack_wait(const hop_xfer_sender_context *context,
                    intptr_t socket_handle, uint64_t epoch,
                    uint32_t batch, uint64_t task_id,
                    uint32_t id, uint64_t deadline_ns, uint64_t *rtt_us) {
    if (!context) context = &s_sender_context;
    if (!context->ack_user || !context->ack_wait_take || !context->ack_unregister)
        return XFER_LOCAL_IO;
    if (rtt_us) *rtt_us = 0;
    for (;;) {
        uint64_t n = context->now_ns
            ? context->now_ns(context->user) : rate_now_ns();
        if (n >= deadline_ns) {
            ack_unregister(context, batch, id);
            return XFER_ACK_TIMEOUT;
        }
#ifdef _WIN32
        uint64_t remaining_ns = deadline_ns - n;
        uint64_t wait_ms = (remaining_ns + 999999ull) / 1000000ull;
        if (wait_ms > (uint64_t)(MAXDWORD - 1u)) wait_ms = MAXDWORD - 1u;
#else
        uint32_t wait_ms = 100;
#endif
        int status = 0, deadline_expired = 0;
        uint64_t rtt_ns = 0;
        int hit = context->ack_wait_take(context->ack_user, batch, id,
                    (uint32_t)wait_ms, &status, &rtt_ns, &deadline_expired);
        if (hit) {
            if (deadline_expired) return XFER_ACK_TIMEOUT;
            if (rtt_us) *rtt_us = rtt_ns / 1000ull;
            return status;
        }
        if (context->peer_aborted &&
            context->peer_aborted(context->user, batch)) {
            ack_unregister(context, batch, id);
            return XFER_PEER_ABORT;
        }
        if (hop_xfer_sender_context_job_cancel_requested(context, batch, task_id)) {
            ack_unregister(context, batch, id);
            return XFER_CANCELLED;
        }
        if (!context->is_stopping || context->is_stopping(context->user) ||
            socket_handle == (intptr_t)-1 || !context->session_current ||
            !context->session_current(context->user, socket_handle, epoch)) {
            ack_unregister(context, batch, id);
            return XFER_SEND_DEAD;
        }
    }
}
/* Image payloads share the same rate limiter and owned-buffer path as files. */
static int send_done_with_context(const hop_xfer_sender_context *context,
                                    SOCKET c, uint32_t batch, uint32_t ok,
                                    uint32_t total) {
    uint8_t dp[20];
    hop_put_be32(dp, batch);
    hop_put_be32(dp + 4, ok);
    hop_put_be32(dp + 8, total);
    hop_put_be64(dp + 12, 0);
    return send_small_with_context(context, c, HOP_MSG_XFER_DONE,
                                   dp, sizeof(dp), batch);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */
#define JOB_COMMIT_UNKNOWN 4 /* receiver outcome is unknown */
#define JOB_COMMIT_REJECTED 5 /* receiver explicitly rejected completion */

static int send_done(SOCKET c, uint32_t batch, uint32_t ok,
                      uint32_t total) {
    uint8_t dp[20];
    hop_put_be32(dp, batch);
    hop_put_be32(dp + 4, ok);
    hop_put_be32(dp + 8, total);
    hop_put_be64(dp + 12, 0);
    return send_small(c, HOP_MSG_XFER_DONE, dp, sizeof(dp), batch);
}

/* Re-send only DONE, never file contents, while the same control session
 * survives. Receiver caches its last receipt and never reapplies effects. */
static uint64_t sender_commit_now_ms(void *user) {
    (void)user;
    return now_ms();
}
static uint64_t sender_commit_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}
static int sender_commit_is_stopping(void *user) {
    const xfer_job *job = (const xfer_job *)user;
    return !job || !job->sender_context.is_stopping ||
           job->sender_context.is_stopping(job->sender_context.user);
}
static int sender_commit_session_matches(void *user, const uint8_t *session) {
    (void)user;
    hop_session_id id;
    memcpy(id.bytes, session, sizeof(id.bytes));
    return hop_session_matches(&id);
}
static int sender_commit_current_connection(void *user, intptr_t *socket_handle,
                                            uint64_t *epoch, uint8_t *session,
                                            size_t session_capacity) {
    xfer_job *job = (xfer_job *)user;
    if (!job || !socket_handle || !epoch || !session ||
        session_capacity < HOP_SESSION_BYTES) return 0;
    EnterCriticalSection(&s_x.lock);
    int ready = s_x.peer_ready && s_x.client != INVALID_SOCKET;
    if (ready) {
        *socket_handle = (intptr_t)s_x.client;
        *epoch = s_x.epoch;
        memcpy(session, s_x.session.bytes, HOP_SESSION_BYTES);
        job->commit_socket = *socket_handle;
        job->commit_epoch = *epoch;
    }
    LeaveCriticalSection(&s_x.lock);
    return ready;
}
static int sender_commit_send_done(void *user, intptr_t socket_handle,
                                   uint64_t epoch, uint32_t batch,
                                   uint32_t ok_count, uint32_t total_count) {
    const xfer_job *job = (const xfer_job *)user;
    if (!job) return 0;
    s_send_epoch = (unsigned long)epoch;
    return send_done_with_context(&job->sender_context, (SOCKET)socket_handle,
                                  batch, ok_count, total_count);
}
static int sender_commit_ack_register(void *user, uint32_t batch, uint32_t slot) {
    const xfer_job *job = (const xfer_job *)user;
    return job && ack_register(&job->sender_context, batch, slot);
}
static int sender_commit_ack_mark_sent(void *user, uint32_t batch, uint32_t slot) {
    const xfer_job *job = (const xfer_job *)user;
    return job && ack_mark_sent(&job->sender_context, batch, slot);
}
static int sender_commit_ack_set_deadline(void *user, uint32_t batch,
                                          uint32_t slot, uint64_t deadline_ns) {
    const xfer_job *job = (const xfer_job *)user;
    return job && ack_set_deadline(&job->sender_context, batch, slot, deadline_ns);
}
static void sender_commit_ack_unregister(void *user, uint32_t batch,
                                         uint32_t slot) {
    const xfer_job *job = (const xfer_job *)user;
    if (job) ack_unregister(&job->sender_context, batch, slot);
}
static hop_xfer_sender_commit_ack_result sender_commit_wait_ack(
    void *user, uint32_t batch, uint32_t slot, uint64_t deadline_ns) {
    const xfer_job *job = (const xfer_job *)user;
    if (!job) return HOP_XFER_SENDER_COMMIT_ACK_OTHER;
    int result = ack_wait(&job->sender_context, job->commit_socket,
                          job->commit_epoch, batch, job->task.id,
                          slot, deadline_ns, NULL);
    if (result == HOP_DONE_ACK_COMPLETE) return HOP_XFER_SENDER_COMMIT_ACK_COMPLETE;
    if (result == HOP_DONE_ACK_PARTIAL) return HOP_XFER_SENDER_COMMIT_ACK_PARTIAL;
    if (result == HOP_DONE_ACK_FAILED) return HOP_XFER_SENDER_COMMIT_ACK_FAILED;
    if (result == XFER_PEER_ABORT) return HOP_XFER_SENDER_COMMIT_ACK_PEER_ABORT;
    if (result == XFER_ACK_TIMEOUT) return HOP_XFER_SENDER_COMMIT_ACK_TIMEOUT;
    if (result == XFER_SEND_DEAD) return HOP_XFER_SENDER_COMMIT_ACK_SEND_DEAD;
    return HOP_XFER_SENDER_COMMIT_ACK_OTHER;
}
static int sender_commit_wait_ms(void *user, uint32_t duration_ms) {
    const xfer_job *job = (const xfer_job *)user;
    return job && job->sender_context.wait_ns &&
           job->sender_context.wait_ns(job->sender_context.user,
                                       (uint64_t)duration_ms * 1000000ull);
}

static int send_done_result(SOCKET fd, const xfer_job *job) {
    (void)fd;
    hop_xfer_sender_commit_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = (void *)job;
    ops.now_ms = sender_commit_now_ms;
    ops.now_ns = sender_commit_now_ns;
    ops.is_stopping = sender_commit_is_stopping;
    ops.session_matches = sender_commit_session_matches;
    ops.current_connection = sender_commit_current_connection;
    ops.send_done = sender_commit_send_done;
    ops.ack_register = sender_commit_ack_register;
    ops.ack_mark_sent = sender_commit_ack_mark_sent;
    ops.ack_set_deadline = sender_commit_ack_set_deadline;
    ops.ack_unregister = sender_commit_ack_unregister;
    ops.wait_ack = sender_commit_wait_ack;
    ops.wait_ms = sender_commit_wait_ms;
    hop_xfer_sender_commit_result result = hop_xfer_sender_run_commit(
        &ops, job->session.bytes, job->batch, job->is_image, job->n, job->ok,
        HOP_DONE_ACK_SLOT, HOP_XFER_ACK_TIMEOUT_MS,
        HOP_XFER_RECONNECT_BUDGET_MS, 100);
    if (result == HOP_XFER_SENDER_COMMIT_DONE) return JOB_DONE;
    if (result == HOP_XFER_SENDER_COMMIT_REJECTED) return JOB_COMMIT_REJECTED;
    if (result == HOP_XFER_SENDER_COMMIT_PEER_ABORT) return JOB_PEER_ABORT;
    return JOB_COMMIT_UNKNOWN;
}

/* 实际发送（send 线程持有 tx 缓冲）。 */
typedef struct {
    SOCKET socket;
    uint8_t *tx;
    xfer_job *job;
} sender_file_engine_call;

static void sender_engine_log(void *user, int level, const char *message);

static int sender_engine_file_open(void *user, const void *platform_entry,
                                   intptr_t *file_handle, uint32_t *system_error) {
    (void)user;
    const xfer_entry *entry = (const xfer_entry *)platform_entry;
    int guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
    HANDLE handle = CreateFileW(entry->full, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    hop_watchdog_end(guard);
    if (system_error) *system_error = (uint32_t)error;
    if (handle == INVALID_HANDLE_VALUE) return 0;
    *file_handle = (intptr_t)handle;
    return 1;
}

static hop_xfer_sender_read_result sender_engine_file_read(
    void *user, intptr_t file_handle, uint8_t *buffer, uint32_t request,
    uint32_t *bytes_read, uint32_t *system_error) {
    (void)user;
    DWORD got = 0;
    int guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
    BOOL ok = ReadFile((HANDLE)file_handle, buffer, (DWORD)request, &got, NULL);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    hop_watchdog_end(guard);
    if (system_error) *system_error = (uint32_t)error;
    if (!ok) return HOP_XFER_SENDER_READ_ERROR;
    if (bytes_read) *bytes_read = (uint32_t)got;
    return got == 0 ? HOP_XFER_SENDER_READ_EOF : HOP_XFER_SENDER_READ_OK;
}

static void sender_engine_file_close(void *user, intptr_t file_handle) {
    (void)user;
    CloseHandle((HANDLE)file_handle);
}

static int sender_engine_file_send_message(void *user, uint8_t type,
                                           const uint8_t *payload,
                                           uint32_t length, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_small_with_context(&call->job->sender_context,
                                   call->socket, type, payload, length, batch);
}

static int sender_engine_file_send_chunk(void *user, uint32_t batch,
                                         uint32_t id, uint64_t offset,
                                         const uint8_t *data, uint32_t length,
                                         uint32_t crc, uint64_t read_ns,
                                         uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_chunk(&call->job->sender_context, call->socket,
                      call->tx, batch, id, offset, data, length,
                      crc, read_ns, crc_ns);
}

static int sender_engine_file_ack_register(void *user, uint32_t batch, uint32_t id) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && ack_register(&call->job->sender_context, batch, id);
}
static int sender_engine_file_ack_mark_sent(void *user, uint32_t batch, uint32_t id) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && ack_mark_sent(&call->job->sender_context, batch, id);
}
static int sender_engine_file_ack_set_deadline(void *user, uint32_t batch,
                                                uint32_t id, uint64_t deadline_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && ack_set_deadline(&call->job->sender_context, batch, id, deadline_ns);
}
static void sender_engine_file_ack_unregister(void *user, uint32_t batch, uint32_t id) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    if (call && call->job) ack_unregister(&call->job->sender_context, batch, id);
}
static uint64_t sender_engine_file_now_ns(void *user) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job->sender_context.now_ns
        ? call->job->sender_context.now_ns(call->job->sender_context.user)
        : rate_now_ns();
}
static void sender_engine_file_status_stage_time(void *user, uint32_t batch,
                                                  int stage, uint64_t elapsed_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    hop_xfer_sender_context_status_stage_time(call ? &call->job->sender_context : NULL,
                             batch, stage, elapsed_ns);
}
static void sender_engine_file_yield(void *user) {
    (void)user;
    yield_cpu();
}

static const void *sender_engine_entry_at(void *user, int index) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return index >= 0 && index < call->job->n ? &call->job->e[index] : NULL;
}

static const char *sender_engine_entry_path(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xfer_entry *)entry)->rel : NULL;
}

static int sender_engine_cancel_requested(void *user, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && hop_xfer_sender_context_job_cancel_requested(
        &call->job->sender_context, batch, call->job->task.id);
}

static int sender_engine_peer_aborted(void *user, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job && call->job->sender_context.peer_aborted
        ? call->job->sender_context.peer_aborted(
              call->job->sender_context.user, batch)
        : 0;
}

static hop_xfer_sender_send_result sender_engine_send_entry(
    void *user, uint32_t batch, uint32_t id, const void *entry,
    uint64_t *ack_deadline_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    const xfer_entry *native = (const xfer_entry *)entry;
    hop_xfer_sender_file_entry view = {
        entry, native->name, native->rel, native->size,
        native->mtime, (uint32_t)native->flags
    };
    hop_xfer_sender_file_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = call;
    ops.cancel_requested = sender_engine_cancel_requested;
    ops.peer_aborted = sender_engine_peer_aborted;
    ops.open_file = sender_engine_file_open;
    ops.read_file = sender_engine_file_read;
    ops.close_file = sender_engine_file_close;
    ops.send_message = sender_engine_file_send_message;
    ops.send_chunk = sender_engine_file_send_chunk;
    ops.ack_register = sender_engine_file_ack_register;
    ops.ack_mark_sent = sender_engine_file_ack_mark_sent;
    ops.ack_set_deadline = sender_engine_file_ack_set_deadline;
    ops.ack_unregister = sender_engine_file_ack_unregister;
    ops.now_ns = sender_engine_file_now_ns;
    ops.status_stage_time = sender_engine_file_status_stage_time;
    ops.yield_cpu = sender_engine_file_yield;
    ops.log_message = sender_engine_log;
    return hop_xfer_sender_send_file_entry(
        &ops, batch, id, &view, call->tx + CHUNK_HDR, s_x.chunk,
        call->tx + CHUNK_HDR, s_x.chunk,
        (uint64_t)HOP_XFER_ACK_TIMEOUT_MS * 1000000ull,
        HOP_FILE_FLAG_DIR, HOP_FILE_OK, HOP_FILE_IO_ERROR,
        HOP_MSG_FILE_BEGIN, HOP_MSG_FILE_END, ack_deadline_ns);
}

static hop_xfer_sender_ack_result sender_engine_wait_ack(
    void *user, uint32_t batch, uint32_t id, uint64_t deadline_ns,
    uint64_t *rtt_us) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    if (!call || !call->job) return HOP_XFER_SENDER_ACK_LOCAL_FAILURE;
    int result = ack_wait(&call->job->sender_context,
                          (intptr_t)call->socket, s_send_epoch,
                          batch, call->job->task.id, id, deadline_ns, rtt_us);
    if (result == HOP_FILE_ACK_OK) return HOP_XFER_SENDER_ACK_OK;
    if (result == HOP_FILE_ACK_BADPATH) return HOP_XFER_SENDER_ACK_BADPATH;
    if (result == XFER_ACK_TIMEOUT) return HOP_XFER_SENDER_ACK_TIMEOUT;
    if (result == XFER_SEND_DEAD) return HOP_XFER_SENDER_ACK_SEND_DEAD;
    if (result == XFER_PEER_ABORT) return HOP_XFER_SENDER_ACK_PEER_ABORT;
    if (result == XFER_CANCELLED) return HOP_XFER_SENDER_ACK_CANCELLED;
    return result >= 0 ? HOP_XFER_SENDER_ACK_OTHER_REMOTE
                       : HOP_XFER_SENDER_ACK_LOCAL_FAILURE;
}

static void sender_engine_unregister_batch(void *user, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    if (call && call->job) ack_unregister_batch(&call->job->sender_context, batch);
}

static void sender_engine_rate_feedback(void *user, uint64_t rtt_us) {
    (void)user;
    rate_feedback(rtt_us);
}

static void sender_engine_status_file_phase(void *user, uint32_t batch,
                                             int index, int phase,
                                             uint32_t attempts) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    hop_xfer_sender_context_status_file_phase(call ? &call->job->sender_context : NULL,
                             batch, index, phase, attempts);
}

static unsigned sender_engine_retry_delay_ms(void *user, unsigned attempt) {
    (void)user;
    return hop_xfer_file_retry_delay_ms((int)attempt);
}

static int sender_engine_wait_ms(void *user, unsigned duration_ms) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    const hop_xfer_sender_context *context = call ? &call->job->sender_context : NULL;
    return context && context->wait_ns &&
           context->wait_ns(context->user, (uint64_t)duration_ms * 1000000ull);
}

static void sender_engine_request_reconnect(void *user) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    const hop_xfer_sender_context *context = call && call->job
        ? &call->job->sender_context : NULL;
    if (context && context->request_reconnect)
        context->request_reconnect(context->user);
}

static void sender_engine_log(void *user, int level, const char *message) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    hop_xfer_sender_context_logf(call ? &call->job->sender_context : NULL, level, "%s",
                message ? message : "");
}

static uint64_t sender_engine_now_ns(void *user) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return call && call->job->sender_context.now_ns
        ? call->job->sender_context.now_ns(call->job->sender_context.user)
        : rate_now_ns();
}

static int sender_engine_send_image_begin(void *user, uint32_t batch,
                                          uint32_t width, uint32_t height,
                                          uint32_t stride) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    uint8_t payload[20];
    hop_put_be32(payload, batch);
    hop_put_be32(payload + 4, 1);
    hop_put_be32(payload + 8, width);
    hop_put_be32(payload + 12, height);
    hop_put_be32(payload + 16, stride);
    return send_small_with_context(&call->job->sender_context, call->socket,
                                   HOP_MSG_IMAGE_BEGIN, payload,
                                   sizeof(payload), batch);
}

static int sender_engine_send_image_chunk(void *user, uint32_t batch,
                                          uint32_t id, uint64_t offset,
                                          const uint8_t *data, uint32_t length,
                                          uint32_t crc, uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_chunk(&call->job->sender_context, call->socket,
                      call->tx, batch, id, offset, data,
                      length, crc, 0, crc_ns);
}

static int sender_engine_send_image_end(void *user, uint32_t batch,
                                        uint32_t id, uint8_t status,
                                        uint32_t file_crc) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    uint8_t payload[9];
    hop_put_be32(payload, id);
    payload[4] = status;
    hop_put_be32(payload + 5, file_crc);
    return send_small_with_context(&call->job->sender_context, call->socket,
                                   HOP_MSG_FILE_END, payload,
                                   sizeof(payload), batch);
}

static void sender_engine_yield(void *user) {
    (void)user;
    yield_cpu();
}

static int run_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    ack_unregister_batch(&j->sender_context, j->batch);
    if (hop_xfer_sender_context_job_cancel_requested(&j->sender_context, j->batch, j->task.id))
        return JOB_CANCELLED;
    if (j->sender_context.peer_aborted &&
        j->sender_context.peer_aborted(j->sender_context.user, j->batch))
        return JOB_PEER_ABORT;
    sender_file_engine_call call = {c, tx, j};
    if (j->is_image) {
        hop_xfer_sender_image_ops image_ops;
        memset(&image_ops, 0, sizeof(image_ops));
        image_ops.user = &call;
        image_ops.cancel_requested = sender_engine_cancel_requested;
        image_ops.peer_aborted = sender_engine_peer_aborted;
        image_ops.now_ns = sender_engine_now_ns;
        image_ops.send_image_begin = sender_engine_send_image_begin;
        image_ops.send_image_chunk = sender_engine_send_image_chunk;
        image_ops.send_image_end = sender_engine_send_image_end;
        image_ops.yield_cpu = sender_engine_yield;
        image_ops.rate_feedback = sender_engine_rate_feedback;
        image_ops.status_file_phase = sender_engine_status_file_phase;
        return hop_xfer_sender_run_image(&image_ops, j->batch, j->pixels,
                                         (uint32_t)j->w, (uint32_t)j->h,
                                         (uint32_t)j->stride, s_x.chunk,
                                         HOP_FILE_OK, j->n, &j->next_i,
                                         &j->ok);
    }
    hop_xfer_sender_engine_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = &call;
    ops.cancel_requested = sender_engine_cancel_requested;
    ops.peer_aborted = sender_engine_peer_aborted;
    ops.entry_at = sender_engine_entry_at;
    ops.entry_path = sender_engine_entry_path;
    ops.send_entry = sender_engine_send_entry;
    ops.wait_ack = sender_engine_wait_ack;
    ops.unregister_batch = sender_engine_unregister_batch;
    ops.rate_feedback = sender_engine_rate_feedback;
    ops.status_file_phase = sender_engine_status_file_phase;
    ops.retry_delay_ms = sender_engine_retry_delay_ms;
    ops.wait_ms = sender_engine_wait_ms;
    ops.request_reconnect = sender_engine_request_reconnect;
    ops.log_message = sender_engine_log;
    return hop_xfer_sender_run_files(&ops, j->batch, j->n, &j->next_i,
                                     &j->ok, HOP_XFER_FILE_RETRY_MAX);
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg_with_context(
    const hop_xfer_sender_context *context, SOCKET c,
    uint32_t batch, uint8_t reason, uint8_t target) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = target;
    send_small_with_context(context, c, HOP_MSG_XFER_ABORT,
                            ab, sizeof(ab), batch);
}

void send_abort_msg(SOCKET c, uint32_t batch, uint8_t reason,
                           uint8_t target) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    ab[5] = target;
    send_small(c, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
void send_ack(SOCKET c, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(c, HOP_MSG_FILE_ACK, b, 9, batch);
}

/* Platform node accessors; queue ordering/removal policy lives in Core. */
static void *sender_queue_next(void *user, const void *node) {
    (void)user;
    return ((const xfer_job *)node)->next;
}
static void sender_queue_set_next(void *user, void *node, void *next) {
    (void)user;
    ((xfer_job *)node)->next = (xfer_job *)next;
}
static int sender_queue_is_cancelled(void *user, const void *node) {
    (void)user;
    const xfer_job *job = (const xfer_job *)node;
    return hop_xfer_sender_context_task_cancel_requested(&job->sender_context, job->batch, &job->task);
}
static uint32_t sender_queue_batch(void *user, const void *node) {
    (void)user;
    return ((const xfer_job *)node)->batch;
}
static uint64_t sender_queue_retry_at(void *user, const void *node) {
    (void)user;
    return ((const xfer_job *)node)->retry_at;
}
static uint64_t sender_queue_give_up_at(void *user, const void *node) {
    (void)user;
    return ((const xfer_job *)node)->give_up_at;
}
const hop_xfer_sender_queue_ops s_sender_queue_ops = {
    NULL, sender_queue_next, sender_queue_set_next, sender_queue_is_cancelled,
    sender_queue_batch, sender_queue_retry_at, sender_queue_give_up_at
};

static void job_defer_to_shutdown(xfer_job *j, xfer_job_owner expected_owner) {
    if (!j) return;
    EnterCriticalSection(&s_x.lock);
    if (j->owner == XFER_JOB_OWNER_RELEASING) {
        j->owner = XFER_JOB_OWNER_SHUTDOWN;
        /* A Job already on shutdown_jobs stays at its current list position. */
        if (expected_owner != XFER_JOB_OWNER_SHUTDOWN) {
            j->next = s_x.shutdown_jobs;
            s_x.shutdown_jobs = j;
        }
    }
    LeaveCriticalSection(&s_x.lock);
}

static int job_claim_for_release(xfer_job *j, xfer_job_owner expected_owner) {
    EnterCriticalSection(&s_x.lock);
    int actual_owner = (int)j->owner;
    int claimed = j->owner == expected_owner;
    if (claimed) j->owner = XFER_JOB_OWNER_RELEASING;
    LeaveCriticalSection(&s_x.lock);
    if (!claimed) {
        hop_log(HOP_LOG_ERROR, "xfer: refusing Job release by non-owner (batch=%u owner=%d expected=%d)",
                j->batch, actual_owner, (int)expected_owner);
    }
    return claimed;
}

static int job_release_task_resources(xfer_job *j) {
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
        return 0; /* never discard an uninitialized-but-live Task handle */
    }
    return 1;
}

int job_free(xfer_job *j, xfer_job_owner expected_owner) {
    if (!j) return 1;
    if (!job_claim_for_release(j, expected_owner)) return -1;

    /* A submitter-side rejection is a failure even if resource return must
     * be retried later. Preserve that terminal intent before any early defer. */
    if ((j->task.initialized || j->task.id) &&
        expected_owner == XFER_JOB_OWNER_SUBMITTER && !j->pending_finish_valid) {
        j->pending_finish_state = HOP_XFER_TASK_FAILED;
        j->pending_finish_valid = 1;
    }

    /* No Task may terminate while the Job still owns image-temp bytes. */
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
    EnterCriticalSection(&s_x.lock);
    if (j->owner == XFER_JOB_OWNER_RELEASING) {
        j->owner = XFER_JOB_OWNER_RELEASED;
        release_owner_ok = 1;
    }
    LeaveCriticalSection(&s_x.lock);
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

typedef struct { xfer_job *job; SOCKET socket; } sender_worker_call;
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
    EnterCriticalSection(&s_x.lock);
    int queued=call->job->owner==XFER_JOB_OWNER_WORKER &&
        !sender_worker_is_stopping(call) &&
        hop_xfer_sender_queue_push_front(&s_x.queue,&s_sender_queue_ops,call->job);
    if (queued) {
        (void)hop_xfer_sender_active_job_clear_if_matches(
            &s_x.active_job,call->job->batch,call->job->task.id);
        call->job->owner=XFER_JOB_OWNER_QUEUE;
    }
    LeaveCriticalSection(&s_x.lock);
    return queued;
}
static void sender_worker_clear_active_job(xfer_job *job) {
    if (!job) return;
    EnterCriticalSection(&s_x.lock);
    (void)hop_xfer_sender_active_job_clear_if_matches(
        &s_x.active_job,job->batch,job->task.id);
    LeaveCriticalSection(&s_x.lock);
}
static int sender_worker_peer_cancelled_and_clear_current(void *user,uint32_t batch,uint64_t task_id) {
    (void)user;
    EnterCriticalSection(&s_x.lock);
    int cancelled=hop_xfer_sender_active_job_take_cancelled_and_clear(
        &s_x.active_job,batch,task_id,HOP_ABORT_CANCEL);
    LeaveCriticalSection(&s_x.lock);
    return cancelled;
}
static void sender_worker_send_abort(void *user,uint32_t batch) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job)send_abort_msg_with_context(&call->job->sender_context,call->socket,
        batch,HOP_ABORT_CANCEL,HOP_ABORT_TO_RECEIVER);
}
static hop_xfer_sender_job_result sender_worker_send_done(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN;
    return (hop_xfer_sender_job_result)send_done_result(call->socket,call->job);
}
static void sender_worker_task_finish(void *user,hop_xfer_task_state state) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return;
    xfer_job *job=call->job;
    job->pending_finish_state=state;
    job->pending_finish_valid=1;
    job->pending_status_end=1;
    if(job_release_task_resources(job))
        hop_xfer_sender_context_task_finish(&job->sender_context,&job->task,state);
}
static void sender_worker_status_end(void *user,uint32_t batch,int phase,const char *detail) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job){
        xfer_job *job=call->job;
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
    xfer_job *job,SOCKET socket,int result,int completion_path,int peer_cancelled) {
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
    EnterCriticalSection(&s_x.lock);
    /* No worker remains. Clear any active identity left by an interrupted
     * retry before Shutdown becomes the sole owner of every queued Job. */
    memset(&s_x.active_job,0,sizeof(s_x.active_job));
    xfer_job *queued=(xfer_job *)hop_xfer_sender_queue_detach_all(&s_x.queue);
    if (queued) {
        xfer_job *tail=queued;
        for (xfer_job *p=queued;p;p=p->next) {
            p->owner=XFER_JOB_OWNER_SHUTDOWN;
            tail=p;
        }
        tail->next=s_x.shutdown_jobs;
        s_x.shutdown_jobs=queued;
    }
    LeaveCriticalSection(&s_x.lock);

    while (s_x.shutdown_jobs) {
        xfer_job *job=s_x.shutdown_jobs;
        xfer_job *next=job->next;
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

typedef struct { int unused; } sender_scheduler_state;
static int sender_scheduler_begin_pass(void *user,int *peer_ready,uint64_t *now,uint32_t *cancel_batch){
    (void)user;if(!peer_ready||!now||!cancel_batch)return 0;EnterCriticalSection(&s_x.lock);
    *peer_ready=s_x.peer_ready&&s_x.client!=INVALID_SOCKET;*now=now_ms();*cancel_batch=s_x.cancel_batch;return 1;
}
static int sender_scheduler_is_stopping(void *user) {
    (void)user;
    return sender_context_is_stopping(&s_sender_platform);
}
static int sender_scheduler_wait_locked(void *user, uint32_t timeout_ms) {
    (void)user;
    HANDLE events[2]={s_x.stop,s_x.q_evt};
    LeaveCriticalSection(&s_x.lock);
    DWORD result=WaitForMultipleObjects(2,events,FALSE,timeout_ms);
    if (result==WAIT_FAILED) {
        hop_log(HOP_LOG_WARN,"xfer: sender queue wait failed; falling back to bounded poll");
        Sleep(timeout_ms);
    }
    EnterCriticalSection(&s_x.lock);
    return 1;
}
static void sender_scheduler_claim_locked(void *user, void *node, int expired) {
    (void)user;
    xfer_job *job=(xfer_job *)node;
    job->owner=XFER_JOB_OWNER_WORKER;
    if (!expired && !hop_xfer_sender_active_job_begin(
            &s_x.active_job,job->batch,job->task.id))
        hop_log(HOP_LOG_ERROR,"xfer: failed to publish active Sender Job (batch=%u task=%llu)",
                job->batch,(unsigned long long)job->task.id);
}
static void sender_scheduler_end_pass(void *user) {
    (void)user;
    LeaveCriticalSection(&s_x.lock);
}


static int sender_worker_view_job_adapter(void *u,void *p,hop_xfer_sender_worker_job_view *v){
    (void)u;xfer_job *j=p;if(!j||!v)return 0;v->platform_job=j;v->context=&j->sender_context;
    v->task=&j->task;v->session=&j->session;v->next_index=&j->next_i;v->ok_count=&j->ok;
    v->pending_finish_state=&j->pending_finish_state;v->pending_finish_valid=&j->pending_finish_valid;
    v->pending_status_end=&j->pending_status_end;v->pending_status_phase=&j->pending_status_phase;
    v->pending_status_detail=j->pending_status_detail;v->pending_status_detail_capacity=sizeof(j->pending_status_detail);
    v->batch=j->batch;return 1;
}
static int sender_worker_snapshot_connection(void *u,intptr_t *socket,uint64_t *epoch,hop_session_id *session,int *connected){
    (void)u;if(!socket||!epoch||!session||!connected)return 0;EnterCriticalSection(&s_x.lock);
    SOCKET c=s_x.peer_ready?s_x.client:INVALID_SOCKET;*session=s_x.session;*epoch=(uint64_t)s_x.epoch;s_send_epoch=s_x.epoch;
    LeaveCriticalSection(&s_x.lock);*socket=(intptr_t)c;*connected=c!=INVALID_SOCKET;return 1;
}
static int sender_worker_status_should_start(void *u,uint32_t batch){
    (void)u;AcquireSRWLockShared(&s_status_lock);int show=!s_send_status.status.active||s_send_status.status.batch==batch;
    ReleaseSRWLockShared(&s_status_lock);return show;
}
static void sender_worker_status_reset(void *u){
    (void)u;AcquireSRWLockExclusive(&s_status_lock);memset(&s_send_status,0,sizeof(s_send_status));ReleaseSRWLockExclusive(&s_status_lock);
}
static void sender_worker_clear_active_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;EnterCriticalSection(&s_x.lock);(void)hop_xfer_sender_active_job_clear_if_matches(&s_x.active_job,batch,task);
    LeaveCriticalSection(&s_x.lock);
}
static void sender_worker_send_abort_adapter(void *u,const hop_xfer_sender_context *ctx,intptr_t socket,uint64_t epoch,uint32_t batch){
    (void)u;(void)epoch;if(socket!=(intptr_t)INVALID_SOCKET)
        send_abort_msg_with_context(ctx,(SOCKET)socket,batch,HOP_ABORT_CANCEL,HOP_ABORT_TO_RECEIVER);
}
static hop_xfer_sender_job_result sender_worker_run_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,uint8_t *tx){
    (void)u;(void)epoch;return (hop_xfer_sender_job_result)run_job((SOCKET)socket,tx,(xfer_job *)job);
}
static int sender_worker_take_peer_cancelled_adapter(void *u,uint32_t batch,uint64_t task){
    (void)u;EnterCriticalSection(&s_x.lock);int c=hop_xfer_sender_active_job_take_cancelled_and_clear(
        &s_x.active_job,batch,task,HOP_ABORT_CANCEL);LeaveCriticalSection(&s_x.lock);return c;
}
static hop_xfer_sender_worker_action sender_worker_finish_job_adapter(void *u,void *job,intptr_t socket,uint64_t epoch,
    hop_xfer_sender_job_result result,int completion,int peer_cancelled){
    (void)u;(void)epoch;return sender_worker_finish_job((xfer_job *)job,(SOCKET)socket,(int)result,completion,peer_cancelled);
}
static void sender_worker_release_job_adapter(void *u,void *job){(void)u;(void)job_free((xfer_job *)job,XFER_JOB_OWNER_WORKER);}

DWORD WINAPI send_thread(LPVOID arg){
    (void)arg;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
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
    return hop_xfer_sender_worker_run(&worker)==HOP_XFER_SENDER_WORKER_RUN_STOPPED?0:1;
}
