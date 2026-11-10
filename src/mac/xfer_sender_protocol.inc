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
    (void)user;
    return s_x.stop != 0;
}

static int sender_context_is_cancelled(void *user, uint32_t batch,
                                      uint64_t task_id) {
    (void)user;
    pthread_mutex_lock(&s_x.qlock);
    int cancelled = s_x.cancel_batch == batch;
    pthread_mutex_unlock(&s_x.qlock);
    return cancelled || (task_id && hop_xfer_task_cancel_requested(task_id));
}

static int sender_context_peer_aborted(void *user, uint32_t batch) {
    (void)user;
    pthread_mutex_lock(&s_x.qlock);
    int aborted = s_x.abort_batch == batch;
    pthread_mutex_unlock(&s_x.qlock);
    return aborted;
}

/* wait_ns is an interruptible full-duration wait. Long waits are split into
 * short slices so shutdown is observed promptly without reporting early success. */
static int sender_context_wait_ns(void *user, uint64_t duration_ns) {
    (void)user;
    uint64_t start = rate_now_ns();
    uint64_t deadline = duration_ns > UINT64_MAX - start
        ? UINT64_MAX : start + duration_ns;
    while (!s_x.stop) {
        uint64_t now = rate_now_ns();
        if (now >= deadline) return 1;
        uint64_t remaining = deadline - now;
        if (remaining > 10000000ull) remaining = 10000000ull;
        struct timespec ts = {
            (time_t)(remaining / 1000000000ull),
            (long)(remaining % 1000000000ull)
        };
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR || s_x.stop) return 0;
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

static void sender_context_log_message(void *user, int level,
                                       const char *message) {
    (void)user;
    hop_log((hop_log_level)level, "%s", message ? message : "");
}

static void sender_context_init(void) {
    memset(&s_sender_context, 0, sizeof(s_sender_context));
    s_sender_context.now_ns = sender_context_now_ns;
    s_sender_context.wait_ns = sender_context_wait_ns;
    s_sender_context.is_stopping = sender_context_is_stopping;
    s_sender_context.is_cancelled = sender_context_is_cancelled;
    s_sender_context.peer_aborted = sender_context_peer_aborted;
    s_sender_context.session_current = sender_context_session_current;
    s_sender_context.write_frame = sender_context_write_frame;
    s_sender_context.task_bind_session = sender_context_task_bind_session;
    s_sender_context.task_set_state = sender_context_task_set_state;
    s_sender_context.task_begin_completion = sender_context_task_begin_completion;
    s_sender_context.task_finish = sender_context_task_finish;
    s_sender_context.task_dispose = sender_context_task_dispose;
    s_sender_context.status_stage_time = sender_context_status_stage_time;
    s_sender_context.log_message = sender_context_log_message;
}

static int rate_wait_for_chunk(uint32_t bytes) {
    if (!hop_xfer_sender_context_valid(&s_sender_context)) return 0;
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(
            &s_x.rate, bytes, s_sender_context.now_ns(s_sender_context.user));
        if (delay == UINT64_MAX) {
            hop_log(HOP_LOG_ERROR, "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return !s_sender_context.is_stopping(s_sender_context.user);
        if (s_sender_context.is_stopping(s_sender_context.user)) return 0;
        if (!s_sender_context.wait_ns(s_sender_context.user, delay)) return 0;
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

static int send_chunk(int fd, uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc,
                      uint64_t read_ns,uint64_t crc_ns) {
    if (!rate_wait_for_chunk(len)) {
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_CRC,crc_ns);
        return 0;
    }
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    hop_session_id session;
    uint64_t epoch = 0;
    int current = sender_context_snapshot_session(fd, &session, &epoch);
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
    uint64_t send_started = rate_now_ns();
    int ok = current && s_sender_context.write_frame &&
             s_sender_context.write_frame(s_sender_context.user, fd, epoch,
                                          parts, part_count);
    uint64_t send_us = (rate_now_ns() - send_started) / 1000ull;
    hop_xfer_rate_note_send_time(&s_x.rate, len, send_us);
    int feedback_due = (s_x.rate.send_samples & 7u) == 0;
    if (feedback_due) rate_feedback(0);
    uint64_t send_ns=send_us*1000ull;
    if(ok) status_chunk_timed(HOP_XFER_DIR_SEND,batch,id,off,len,read_ns,crc_ns,send_ns,0);
    else {
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_CRC,crc_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_SEND,send_ns);
    }
    hop_watchdog_end(send_guard);
    return ok;
}

/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */
#include "xfer_ack_coordinator.h"
static hop_xfer_ack_coordinator *s_ack;

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)
#define XFER_ACK_PENDING  (-6)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static int ack_register(uint32_t batch, uint32_t id) {
    return hop_xfer_ack_coordinator_register(s_ack, batch, id);
}

static int ack_mark_sent(uint32_t batch, uint32_t id) {
    return hop_xfer_ack_coordinator_mark_sent(s_ack, batch, id, rate_now_ns());
}

static int ack_set_deadline(uint32_t batch, uint32_t id, uint64_t deadline_ns) {
    return hop_xfer_ack_coordinator_set_deadline(s_ack, batch, id, deadline_ns);
}

static void ack_unregister(uint32_t batch, uint32_t id) {
    hop_xfer_ack_coordinator_unregister(s_ack, batch, id);
}

static void ack_unregister_batch(uint32_t batch) {
    hop_xfer_ack_coordinator_unregister_batch(s_ack, batch);
}

/* Receiver writer thread stores ACKs only in their pre-registered batch/id slot. */
static void ack_deliver(uint32_t batch, uint32_t id, int status) {
    (void)hop_xfer_ack_coordinator_deliver(s_ack, batch, id, status, rate_now_ns());
}

/* 用户取消了这个批次？每块之后查一次，锁开销可以忽略。 */
static int cancel_requested(uint32_t batch) {
    pthread_mutex_lock(&s_x.qlock);
    hop_xfer_task_id id = s_x.cur_batch == batch ? s_x.cur_task_id : 0;
    pthread_mutex_unlock(&s_x.qlock);
    return s_sender_context.is_cancelled
        ? s_sender_context.is_cancelled(s_sender_context.user, batch, id)
        : (id && hop_xfer_task_cancel_requested(id));
}

/* 对端中止过这个批次？统一经 Sender Context 查询。 */
static int peer_aborted(uint32_t batch) {
    return s_sender_context.peer_aborted
        ? s_sender_context.peer_aborted(s_sender_context.user, batch) : 0;
}

/* 发送线程调用。返回 status，超时返回 XFER_ACK_TIMEOUT。
 *
 * 用"先查槽再等条件变量"的循环：ACK 可能在任何时刻到达，
 * 只等一次会漏掉"信号早于等待"的情况。
 * 等待切片 100 ms，是为了让取消 / 对端中止能在 100 ms 内被看到 ——
 * 不然就要去折腾 condattr_setclock 让条件变量走单调时钟。 */
static int ack_wait(uint32_t batch, uint32_t id, uint64_t deadline_ns, uint64_t *rtt_us) {
    if (rtt_us) *rtt_us = 0;
    for (;;) {
        uint64_t n = rate_now_ns();
        if (n >= deadline_ns) { ack_unregister(batch,id); return XFER_ACK_TIMEOUT; }

        int status = 0, deadline_expired = 0;
        uint64_t rtt_ns = 0;
        int hit = hop_xfer_ack_coordinator_wait_take(s_ack, batch, id, 100,
                    &status, &rtt_ns, &deadline_expired);
        if (hit) {
            if (deadline_expired) return XFER_ACK_TIMEOUT;
            if (rtt_us) *rtt_us = rtt_ns / 1000ull;
            return status;
        }
        if (peer_aborted(batch)) { ack_unregister(batch,id); return XFER_PEER_ABORT; }
        if (cancel_requested(batch)) { ack_unregister(batch,id); return XFER_CANCELLED; }
        if (s_x.stop) { ack_unregister(batch,id); return XFER_SEND_DEAD; }
        pthread_mutex_lock(&s_x.slock);
        int peer_ready = s_x.peer_ready;
        pthread_mutex_unlock(&s_x.slock);
        if (!peer_ready) { ack_unregister(batch,id); return XFER_SEND_DEAD; }
    }
}

/* 发送一个条目。成功写出 FILE_END 后返回 XFER_ACK_PENDING；
 * run_job 在最多 4 个已发送文件组成的窗口中统一收取 ACK。 */
static int send_file_entry(int fd, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xf_entry *e, uint64_t *ack_deadline_ns) {
    if (!ack_deadline_ns || !tx) return XFER_LOCAL_IO;
    *ack_deadline_ns = 0;
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);
    uint32_t blen = 4 + 4 + 8 + 8 + 1 + 2 + nlen + 2 + plen;

    /* FILE_BEGIN is small and send_small is synchronous. Reuse the sender's
     * fixed TX buffer instead of malloc/free once per file (notably 10k-file
     * batches); send_chunk overwrites this region only after FILE_BEGIN ends. */
    if (blen > s_x.chunk) return XFER_LOCAL_IO;
    if (!ack_register(batch,id)) return XFER_LOCAL_IO;
    uint8_t *bp = tx + CHUNK_HDR;
    uint32_t o = 0;
    hop_put_be32(bp + o, batch); o += 4;
    hop_put_be32(bp + o, id);    o += 4;
    hop_put_be64(bp + o, e->size); o += 8;
    hop_put_be64(bp + o, e->mtime); o += 8;
    bp[o++] = (uint8_t)e->flags;
    hop_put_be16(bp + o, (uint16_t)nlen); o += 2;
    memcpy(bp + o, e->name, nlen); o += nlen;
    hop_put_be16(bp + o, (uint16_t)plen); o += 2;
    memcpy(bp + o, e->rel, plen); o += plen;

    int ok = send_small(fd, HOP_MSG_FILE_BEGIN, bp, blen, batch);
    /* TX buffer remains owned by the sender thread. */
    if (!ok) { ack_unregister(batch,id); return XFER_SEND_DEAD; }

    uint32_t fcrc = 0;
    uint8_t end_status = HOP_FILE_OK;
    int local_fail = 0;
    if (!(e->flags & HOP_FILE_FLAG_DIR)) {
        int open_guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
        int f = open(e->full, O_RDONLY);
        hop_watchdog_end(open_guard);
        if (f < 0) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (%s)", e->rel, strerror(errno));
            end_status = HOP_FILE_IO_ERROR;
            local_fail = 1;
        } else {
            uint64_t off = 0;
            while (off < e->size) {
                uint64_t remaining = e->size - off;
                size_t request = (size_t)(remaining < s_x.chunk ? remaining : s_x.chunk);
                ssize_t got;
                uint64_t read_started_ns=rate_now_ns();
                do {
                    int read_guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
                    got = read(f, tx + CHUNK_HDR, request);
                    hop_watchdog_end(read_guard);
                } while (got < 0 && errno == EINTR);
                uint64_t read_elapsed_ns=rate_now_ns()-read_started_ns;
                if (got < 0) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: read failed, %s (%s)", e->rel, strerror(errno));
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                if (got == 0) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: unexpected EOF, %s (%llu/%llu bytes)",
                            e->rel, (unsigned long long)off, (unsigned long long)e->size);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                uint64_t crc_started_ns=rate_now_ns();
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, (size_t)got);
                uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
                if (!send_chunk(fd,tx,batch,id,off,tx+CHUNK_HDR,(uint32_t)got,ccrc,
                                read_elapsed_ns,crc_elapsed_ns)) {
                    close(f); ack_unregister(batch,id); return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, off, ccrc);
                off += (uint64_t)got;
                yield_cpu();
                if (cancel_requested(batch)) {
                    close(f); ack_unregister(batch,id); return XFER_CANCELLED;
                }
            }
            if (!local_fail && off == e->size) {
                uint8_t extra = 0;
                ssize_t probe;
                do {
                    int read_guard = hop_watchdog_begin("file size probe", HOP_IO_DEADLINE_MS);
                    probe = read(f, &extra, 1);
                    hop_watchdog_end(read_guard);
                } while (probe < 0 && errno == EINTR);
                if (probe != 0) {
                    hop_log(HOP_LOG_WARN, "xfer: source changed while queued, %s", e->rel);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1;
                }
            }
            close(f);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    /* Mark before FILE_END so an immediate peer ACK cannot race registration. */
    if (!ack_mark_sent(batch,id)) { ack_unregister(batch,id); return XFER_LOCAL_IO; }
    if (!send_small(fd, HOP_MSG_FILE_END, endp, 9, batch)) {
        ack_unregister(batch,id); return XFER_SEND_DEAD;
    }
    (void)local_fail;
    /* The ACK slot was armed before FILE_END to avoid a fast-ACK race. Set its
     * independent deadline now that the complete frame has been written. */
    uint64_t deadline_ns = rate_now_ns() +
        (uint64_t)HOP_XFER_ACK_TIMEOUT_MS * 1000000ull;
    *ack_deadline_ns = deadline_ns;
    if (!ack_set_deadline(batch,id,deadline_ns)) {
        ack_unregister(batch,id); return XFER_LOCAL_IO;
    }
    return XFER_ACK_PENDING;
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
static int send_done(int fd, const xf_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp,      j->batch);
    hop_put_be32(dp + 4,  (uint32_t)j->ok);
    hop_put_be32(dp + 8,  hop_xfer_job_total(j->is_image,j->n));
    hop_put_be64(dp + 12, 0);
    return send_small(fd, HOP_MSG_XFER_DONE, dp, 20, j->batch);
}

/* Re-send only DONE, never file contents, while the same control session
 * survives. Receiver caches its last receipt and never reapplies effects. */
static int send_done_result(int fd, const xf_job *job) {
    uint64_t until=now_ms()+HOP_XFER_RECONNECT_BUDGET_MS;
    while (now_ms()<until) {
        if (!hop_session_matches(&job->session)) return JOB_COMMIT_UNKNOWN;
        if (s_x.stop) return JOB_COMMIT_UNKNOWN;
        pthread_mutex_lock(&s_x.slock);
        fd=s_x.peer_ready?s_x.sock:-1;
        hop_session_id session=s_x.session;
        s_send_epoch=s_x.epoch;
        pthread_mutex_unlock(&s_x.slock);
        if (fd<0) { usleep(100000); continue; }
        if (!hop_session_id_equal(&session,&job->session)) return JOB_COMMIT_UNKNOWN;
        if (!ack_register(job->batch,HOP_DONE_ACK_SLOT)) return JOB_COMMIT_UNKNOWN;
        if (!ack_mark_sent(job->batch,HOP_DONE_ACK_SLOT)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN;
        }
        if (!send_done(fd,job)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); usleep(100000); continue;
        }
        uint64_t now=now_ms();
        if (now>=until) { ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN; }
        uint64_t wait=until-now;
        if (wait>HOP_XFER_ACK_TIMEOUT_MS) wait=HOP_XFER_ACK_TIMEOUT_MS;
        uint64_t deadline=rate_now_ns()+wait*1000000ull;
        if (!ack_set_deadline(job->batch,HOP_DONE_ACK_SLOT,deadline)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN;
        }
        int result=ack_wait(job->batch,HOP_DONE_ACK_SLOT,deadline,NULL);
        if (result==HOP_DONE_ACK_FAILED) return JOB_COMMIT_REJECTED;
        int expected=job->ok==(int)hop_xfer_job_total(job->is_image,job->n)
            ? HOP_DONE_ACK_COMPLETE : HOP_DONE_ACK_PARTIAL;
        if (result==expected) return JOB_DONE;
        if (result==XFER_PEER_ABORT) return JOB_PEER_ABORT;
        if (result!=XFER_ACK_TIMEOUT && result!=XFER_SEND_DEAD) return JOB_COMMIT_UNKNOWN;
        usleep(100000);
    }
    return JOB_COMMIT_UNKNOWN;
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(int fd, uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = HOP_ABORT_TO_RECEIVER;
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

static const void *sender_engine_entry_at(void *user, int index) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return index >= 0 && index < call->job->n ? &call->job->e[index] : NULL;
}

static const char *sender_engine_entry_path(void *user, const void *entry) {
    (void)user;
    return entry ? ((const xf_entry *)entry)->rel : NULL;
}

static int sender_engine_cancel_requested(void *user, uint32_t batch) {
    (void)user;
    return cancel_requested(batch);
}

static int sender_engine_peer_aborted(void *user, uint32_t batch) {
    (void)user;
    return peer_aborted(batch);
}

static hop_xfer_sender_send_result sender_engine_send_entry(
    void *user, uint32_t batch, uint32_t id, const void *entry,
    uint64_t *ack_deadline_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    int result = send_file_entry(call->fd, call->tx, batch, id,
                                 (const xf_entry *)entry, ack_deadline_ns);
    if (result == XFER_ACK_PENDING) return HOP_XFER_SENDER_SEND_PENDING;
    if (result == XFER_SEND_DEAD) return HOP_XFER_SENDER_SEND_DEAD;
    if (result == XFER_PEER_ABORT) return HOP_XFER_SENDER_SEND_PEER_ABORT;
    if (result == XFER_CANCELLED) return HOP_XFER_SENDER_SEND_CANCELLED;
    return HOP_XFER_SENDER_SEND_RETRYABLE;
}

static hop_xfer_sender_ack_result sender_engine_wait_ack(
    void *user, uint32_t batch, uint32_t id, uint64_t deadline_ns,
    uint64_t *rtt_us) {
    (void)user;
    int result = ack_wait(batch, id, deadline_ns, rtt_us);
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
    (void)user;
    ack_unregister_batch(batch);
}

static void sender_engine_rate_feedback(void *user, uint64_t rtt_us) {
    (void)user;
    rate_feedback(rtt_us);
}

static void sender_engine_status_file_phase(void *user, uint32_t batch,
                                             int index, int phase,
                                             uint32_t attempts) {
    (void)user;
    status_file_phase(HOP_XFER_DIR_SEND, batch, index, phase, attempts);
}

static unsigned sender_engine_retry_delay_ms(void *user, unsigned attempt) {
    (void)user;
    return hop_xfer_file_retry_delay_ms((int)attempt);
}

static int sender_engine_wait_ms(void *user, unsigned duration_ms) {
    (void)user;
    return s_sender_context.wait_ns &&
           s_sender_context.wait_ns(s_sender_context.user,
                                    (uint64_t)duration_ms * 1000000ull);
}

static void sender_engine_request_reconnect(void *user) {
    (void)user;
    xfer_kick();
}

static void sender_engine_log(void *user, int level, const char *message) {
    (void)user;
    hop_log((hop_log_level)level, "%s", message ? message : "");
}

static uint64_t sender_engine_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
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
    return send_small(call->fd, HOP_MSG_IMAGE_BEGIN, payload,
                      sizeof(payload), batch);
}

static int sender_engine_send_image_chunk(void *user, uint32_t batch,
                                          uint32_t id, uint64_t offset,
                                          const uint8_t *data, uint32_t length,
                                          uint32_t crc, uint64_t crc_ns) {
    sender_file_engine_call *call = (sender_file_engine_call *)user;
    return send_chunk(call->fd, call->tx, batch, id, offset, data,
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
    return send_small(call->fd, HOP_MSG_FILE_END, payload,
                      sizeof(payload), batch);
}

static void sender_engine_yield(void *user) {
    (void)user;
    yield_cpu();
}

static int run_job(int fd, uint8_t *tx, xf_job *j) {
    ack_unregister_batch(j->batch);
    if (cancel_requested(j->batch)) return JOB_CANCELLED;
    if (peer_aborted(j->batch)) return JOB_PEER_ABORT;
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
