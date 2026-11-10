#include "../core/xfer_sender_ack_adapter.h"
#include "../core/xfer_sender_context_adapter.h"

static int sender_context_ack_create(void);
static void sender_context_ack_destroy(void);
static void sender_context_wake_ack(void);

static void rate_init_from_config(hop_xfer_rate_limiter *rate, int configured_max_mib,
                                  uint64_t burst_bytes, uint64_t now_ns) {
    uint64_t max_mib = configured_max_mib < 16 ? 16u :
                       configured_max_mib > 4096 ? 4096u :
                       (uint64_t)configured_max_mib;
    uint64_t max_bps = max_mib * XFER_RATE_MIB_BYTES;
    uint64_t min_bps = XFER_RATE_MIN_MIB * XFER_RATE_MIB_BYTES;
    uint64_t initial_bps = XFER_RATE_INITIAL_MIB * XFER_RATE_MIB_BYTES;
    if (min_bps > max_bps) min_bps = max_bps;
    if (initial_bps > max_bps) initial_bps = max_bps;
    hop_xfer_rate_init(rate, initial_bps, min_bps, max_bps, burst_bytes, now_ns);
}

static uint64_t rate_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t sender_context_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}

static int sender_context_is_stopping(void *user) {
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

static void sender_context_request_reconnect(void *user) {
    sender_platform_context *ctx = (sender_platform_context *)user;
    if (!ctx || !ctx->session_lock || !ctx->sock || !ctx->peer_ready) return;
    pthread_mutex_lock(ctx->session_lock);
    *ctx->peer_ready = 0;
    if (*ctx->sock >= 0) shutdown(*ctx->sock, SHUT_RDWR);
    pthread_mutex_unlock(ctx->session_lock);
}

static void sender_context_init(void) {
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

static uint32_t rate_queue_pressure_permille(void) {
    hop_xfer_budget_snapshot snap;
    hop_xfer_budget_get_snapshot(&snap);
    uint64_t bytes = snap.rx_queue_bytes * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_BYTES;
    uint64_t slots = (uint64_t)snap.rx_queue_slots * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_SLOTS;
    uint64_t pressure = bytes > slots ? bytes : slots;
    return (uint32_t)(pressure > 1000ull ? 1000ull : pressure);
}

static void rate_feedback(uint64_t ack_rtt_us) {
    uint64_t before = s_x.rate.current_rate_bytes_per_sec;
    uint32_t pressure = rate_queue_pressure_permille();
    hop_xfer_rate_feedback(&s_x.rate, ack_rtt_us, pressure);
    if (s_x.rate.current_rate_bytes_per_sec < before) {
        hop_log(HOP_LOG_INFO, "xfer: adaptive send rate %.1f MB/s (ack %.1f ms, local rx queue %u/1000)",
                (double)s_x.rate.current_rate_bytes_per_sec / 1048576.0,
                (double)ack_rtt_us / 1000.0, pressure);
    }
}

static int send_chunk(const hop_xfer_sender_context *context, int fd,
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
    int current = sender_context_snapshot_session(context->user, fd, &session, &epoch);
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
             context->write_frame(context->user, (intptr_t)fd, epoch,
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

static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L); }

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

static int sender_context_ack_create(void) {
    if (s_sender_platform.ack_adapter) return 0;
    hop_xfer_sender_ack_adapter *adapter =
        hop_xfer_sender_ack_adapter_create(sender_context_now_ns, NULL);
    if (!adapter) return 0;
    s_sender_platform.ack_adapter = adapter;
    s_sender_context.ack_user = adapter;
    return 1;
}
static void sender_context_ack_destroy(void) {
    hop_xfer_sender_ack_adapter_destroy(
        (hop_xfer_sender_ack_adapter *)s_sender_platform.ack_adapter);
    s_sender_platform.ack_adapter = NULL;
    s_sender_context.ack_user = NULL;
}
static void sender_context_wake_ack(void) {
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
static void ack_deliver(uint32_t batch, uint32_t id, int status) {
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
/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */
#define JOB_COMMIT_UNKNOWN 4 /* receiver outcome is unknown */
#define JOB_COMMIT_REJECTED 5 /* receiver explicitly rejected completion */

/* 收尾：告诉接收端本批实际送达了多少。
 * ok < total 时接收端会报"部分成功" —— 这是用户唯一能拿到准确结论的地方，
 * 所以 count 必须是真实值，不能像以前那样写死 0。 */
static int send_done_with_context(const hop_xfer_sender_context *context,
                                    int fd, uint32_t batch, uint32_t ok,
                                    uint32_t total) {
    uint8_t dp[20];
    hop_put_be32(dp, batch);
    hop_put_be32(dp + 4, ok);
    hop_put_be32(dp + 8, total);
    hop_put_be64(dp + 12, 0);
    return send_small_with_context(context, fd, HOP_MSG_XFER_DONE,
                                   dp, sizeof(dp), batch);
}

static int send_done(int fd, uint32_t batch, uint32_t ok,
                      uint32_t total) {
    uint8_t dp[20];
    hop_put_be32(dp, batch);
    hop_put_be32(dp + 4, ok);
    hop_put_be32(dp + 8, total);
    hop_put_be64(dp + 12, 0);
    return send_small(fd, HOP_MSG_XFER_DONE, dp, sizeof(dp), batch);
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
    const xf_job *job = (const xf_job *)user;
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
    xf_job *job = (xf_job *)user;
    if (!job || !socket_handle || !epoch || !session ||
        session_capacity < HOP_SESSION_BYTES) return 0;
    pthread_mutex_lock(&s_x.slock);
    int ready = s_x.peer_ready && s_x.sock >= 0;
    if (ready) {
        *socket_handle = (intptr_t)s_x.sock;
        *epoch = s_x.epoch;
        memcpy(session, s_x.session.bytes, HOP_SESSION_BYTES);
        job->commit_socket = *socket_handle;
        job->commit_epoch = *epoch;
    }
    pthread_mutex_unlock(&s_x.slock);
    return ready;
}
static int sender_commit_send_done(void *user, intptr_t socket_handle,
                                   uint64_t epoch, uint32_t batch,
                                   uint32_t ok_count, uint32_t total_count) {
    const xf_job *job = (const xf_job *)user;
    if (!job) return 0;
    s_send_epoch = (unsigned long)epoch;
    return send_done_with_context(&job->sender_context, (int)socket_handle,
                                  batch, ok_count, total_count);
}
static int sender_commit_ack_register(void *user, uint32_t batch, uint32_t slot) {
    const xf_job *job = (const xf_job *)user;
    return job && ack_register(&job->sender_context, batch, slot);
}
static int sender_commit_ack_mark_sent(void *user, uint32_t batch, uint32_t slot) {
    const xf_job *job = (const xf_job *)user;
    return job && ack_mark_sent(&job->sender_context, batch, slot);
}
static int sender_commit_ack_set_deadline(void *user, uint32_t batch,
                                          uint32_t slot, uint64_t deadline_ns) {
    const xf_job *job = (const xf_job *)user;
    return job && ack_set_deadline(&job->sender_context, batch, slot, deadline_ns);
}
static void sender_commit_ack_unregister(void *user, uint32_t batch,
                                         uint32_t slot) {
    const xf_job *job = (const xf_job *)user;
    if (job) ack_unregister(&job->sender_context, batch, slot);
}
static hop_xfer_sender_commit_ack_result sender_commit_wait_ack(
    void *user, uint32_t batch, uint32_t slot, uint64_t deadline_ns) {
    const xf_job *job = (const xf_job *)user;
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
    const xf_job *job = (const xf_job *)user;
    return job && job->sender_context.wait_ns &&
           job->sender_context.wait_ns(job->sender_context.user,
                                       (uint64_t)duration_ms * 1000000ull);
}

static int send_done_result(int fd, const xf_job *job) {
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

static void send_abort_msg(int fd, uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    ab[5] = HOP_ABORT_TO_RECEIVER;
    send_small(fd, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 取当前连接再发 ABORT。sock 由 slock 保护，不能裸读。
 * 锁序固定 slock -> txlock，不会和发送线程反序。 */
static void send_abort(uint32_t batch, uint8_t reason) {
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
static void send_ack(int fd, uint32_t batch, uint32_t id, int status) {
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

static void sender_engine_log(void *user, int level, const char *message);

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

static int sender_engine_file_send_message(void *user, uint8_t type,
                                           const uint8_t *payload,
                                           uint32_t length, uint32_t batch) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_small_with_context(&call->job->sender_context,
                                   call->fd, type, payload, length, batch);
}

static int sender_engine_file_send_chunk(void *user, uint32_t batch,
                                         uint32_t id, uint64_t offset,
                                         const uint8_t *data, uint32_t length,
                                         uint32_t crc, uint64_t read_ns,
                                         uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_chunk(&call->job->sender_context, call->fd,
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
    return entry ? ((const xf_entry *)entry)->rel : NULL;
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
    const xf_entry *native = (const xf_entry *)entry;
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
                          (intptr_t)call->fd, s_send_epoch,
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
    return send_small_with_context(&call->job->sender_context, call->fd,
                                   HOP_MSG_IMAGE_BEGIN, payload,
                                   sizeof(payload), batch);
}

static int sender_engine_send_image_chunk(void *user, uint32_t batch,
                                          uint32_t id, uint64_t offset,
                                          const uint8_t *data, uint32_t length,
                                          uint32_t crc, uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_chunk(&call->job->sender_context, call->fd,
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
    return send_small_with_context(&call->job->sender_context, call->fd,
                                   HOP_MSG_FILE_END, payload,
                                   sizeof(payload), batch);
}

static void sender_engine_yield(void *user) {
    (void)user;
    yield_cpu();
}

static int run_job(int fd, uint8_t *tx, xf_job *j) {
    ack_unregister_batch(&j->sender_context, j->batch);
    if (hop_xfer_sender_context_job_cancel_requested(&j->sender_context, j->batch, j->task.id))
        return JOB_CANCELLED;
    if (j->sender_context.peer_aborted &&
        j->sender_context.peer_aborted(j->sender_context.user, j->batch))
        return JOB_PEER_ABORT;
    sender_file_engine_call call = {fd, tx, j};
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
