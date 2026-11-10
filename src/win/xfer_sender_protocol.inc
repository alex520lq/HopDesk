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

static void rate_sleep_ns(uint64_t ns) {
    if (s_x.rate_timer) {
        LARGE_INTEGER due;
        uint64_t units = (ns + 99ull) / 100ull; /* negative 100 ns relative due time */
        if (!units) units = 1;
        due.QuadPart = -(LONGLONG)units;
        if (SetWaitableTimer(s_x.rate_timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE waits[2] = {s_x.stop, s_x.rate_timer};
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

static int rate_wait_for_chunk(uint32_t bytes) {
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(&s_x.rate, bytes, rate_now_ns());
        if (delay == UINT64_MAX) {
            hop_log(HOP_LOG_ERROR, "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return 1;
        if (WaitForSingleObject(s_x.stop, 0) == WAIT_OBJECT_0) return 0;
        rate_sleep_ns(delay);
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

static int send_chunk(SOCKET c, uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc,
                      uint64_t read_ns,uint64_t crc_ns) {
    if (!rate_wait_for_chunk(len)) {
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_CRC,crc_ns);
        return 0;
    }
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + HOP_DATA_PREFIX + len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 5, id);
    hop_put_be64(tx + HOP_DATA_PREFIX + 9, off);
    hop_put_be32(tx + HOP_DATA_PREFIX + 17, len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 21, crc);
    EnterCriticalSection(&s_txlock);
    EnterCriticalSection(&s_x.lock);
    hop_data_prefix(tx + HOP_HEADER_SIZE, &s_x.session, batch);
    LeaveCriticalSection(&s_x.lock);
    uint64_t send_started = rate_now_ns();
    int ok = send_session_current(c) &&
             (data == tx + CHUNK_HDR ?
              send_all(c, (const char *)tx, (int)(CHUNK_HDR + len)) :
              (send_all(c, (const char *)tx, (int)CHUNK_HDR) &&
               send_all(c, (const char *)data, (int)len)));
    uint64_t send_us = (rate_now_ns() - send_started) / 1000ull;
    hop_xfer_rate_note_send_time(&s_x.rate, len, send_us);
    int feedback_due = (s_x.rate.send_samples & 7u) == 0;
    LeaveCriticalSection(&s_txlock);
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

static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }

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
    EnterCriticalSection(&s_x.lock);
    int c = (s_x.cancel_batch == batch);
    hop_xfer_task_id id = s_x.cur_batch == batch ? s_x.cur_task_id : 0;
    LeaveCriticalSection(&s_x.lock);
    return c || (id && hop_xfer_task_cancel_requested(id));
}

/* 对端中止过这个批次？（ABORT 是批量级的，比如图片超限、接收端建不了目录） */
static int peer_aborted(uint32_t batch) {
    EnterCriticalSection(&s_x.lock);
    int a = (s_x.abort_batch == batch);
    LeaveCriticalSection(&s_x.lock);
    return a;
}

/* 发送线程调用。返回 status，超时返回 XFER_ACK_TIMEOUT。
 *
 * 用"先查槽再等事件"的循环，而不是"先 ResetEvent 再等"：
 * ACK 可能在两者之间到达，后者会漏掉信号然后白等满 10 秒。 */
static int ack_wait(uint32_t batch, uint32_t id, uint64_t deadline_ns, uint64_t *rtt_us) {
    if (rtt_us) *rtt_us = 0;
    for (;;) {
        uint64_t n = rate_now_ns();
        if (n >= deadline_ns) { ack_unregister(batch, id); return XFER_ACK_TIMEOUT; }
        uint64_t remaining_ns = deadline_ns - n;
        uint64_t wait_ms = (remaining_ns + 999999ull) / 1000000ull;
        if (wait_ms > (uint64_t)(MAXDWORD - 1u)) wait_ms = MAXDWORD - 1u;

        int status = 0, deadline_expired = 0;
        uint64_t rtt_ns = 0;
        int hit = hop_xfer_ack_coordinator_wait_take(s_ack, batch, id,
                    (uint32_t)wait_ms, &status, &rtt_ns, &deadline_expired);
        if (hit) {
            if (deadline_expired) return XFER_ACK_TIMEOUT;
            if (rtt_us) *rtt_us = rtt_ns / 1000ull;
            return status;
        }
        if (peer_aborted(batch)) { ack_unregister(batch,id); return XFER_PEER_ABORT; }
        if (cancel_requested(batch)) { ack_unregister(batch,id); return XFER_CANCELLED; }
        if (WaitForSingleObject(s_x.stop, 0) == WAIT_OBJECT_0) {
            ack_unregister(batch,id);
            return XFER_SEND_DEAD;
        }
        EnterCriticalSection(&s_x.lock);
        int peer_ready = s_x.peer_ready;
        LeaveCriticalSection(&s_x.lock);
        if (!peer_ready) { ack_unregister(batch,id); return XFER_SEND_DEAD; }
    }
}

/* 发送一个条目。成功写出 FILE_END 后返回 XFER_ACK_PENDING；
 * run_job 在最多 4 个已发送文件组成的窗口中统一收取 ACK。 */
static int send_file_entry(SOCKET c, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xfer_entry *e, uint64_t *ack_deadline_ns) {
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
    hop_put_be32(bp + o, id); o += 4;
    hop_put_be64(bp + o, e->size); o += 8;
    hop_put_be64(bp + o, e->mtime); o += 8;
    bp[o++] = (uint8_t)e->flags;
    hop_put_be16(bp + o, (uint16_t)nlen); o += 2;
    memcpy(bp + o, e->name, nlen); o += nlen;
    hop_put_be16(bp + o, (uint16_t)plen); o += 2;
    memcpy(bp + o, e->rel, plen); o += plen;

    int ok = send_small(c, HOP_MSG_FILE_BEGIN, bp, blen, batch);
    /* TX buffer remains owned by the sender thread. */
    if (!ok) { ack_unregister(batch,id); return XFER_SEND_DEAD; }

    uint32_t fcrc = 0;
    uint8_t end_status = HOP_FILE_OK;
    int local_fail = 0;
    if (!(e->flags & HOP_FILE_FLAG_DIR)) {
        int open_guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
        HANDLE h = CreateFileW(e->full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        DWORD open_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        hop_watchdog_end(open_guard);
        if (h == INVALID_HANDLE_VALUE) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (err %lu)", e->rel, open_error);
            end_status = HOP_FILE_IO_ERROR; local_fail = 1;
        } else {
            uint64_t off = 0;
            while (off < e->size) {
                uint64_t remaining = e->size - off;
                DWORD request = (DWORD)(remaining < s_x.chunk ? remaining : s_x.chunk);
                DWORD got = 0;
                uint64_t read_started_ns=rate_now_ns();
                int read_guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
                int read_ok = ReadFile(h, tx + CHUNK_HDR, request, &got, NULL);
                DWORD read_error = read_ok ? ERROR_SUCCESS : GetLastError();
                hop_watchdog_end(read_guard);
                uint64_t read_elapsed_ns=rate_now_ns()-read_started_ns;
                if (!read_ok) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: read failed, %s (err %lu)", e->rel, read_error);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                if (got == 0) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: unexpected EOF, %s (%llu/%llu bytes)",
                            e->rel, (unsigned long long)off, (unsigned long long)e->size);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                uint64_t crc_started_ns=rate_now_ns();
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, got);
                uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
                if (!send_chunk(c,tx,batch,id,off,tx+CHUNK_HDR,got,ccrc,
                                read_elapsed_ns,crc_elapsed_ns)) {
                    CloseHandle(h); ack_unregister(batch,id); return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, off, ccrc);
                off += got;
                yield_cpu();
                if (cancel_requested(batch)) {
                    CloseHandle(h); ack_unregister(batch,id); return XFER_CANCELLED;
                }
            }
            if (!local_fail && off == e->size) {
                BYTE extra = 0; DWORD extra_got = 0;
                int read_guard = hop_watchdog_begin("file size probe", HOP_IO_DEADLINE_MS);
                int read_ok = ReadFile(h, &extra, 1, &extra_got, NULL);
                DWORD probe_error = read_ok ? ERROR_SUCCESS : GetLastError();
                hop_watchdog_end(read_guard);
                if (!read_ok || extra_got != 0) {
                    if (!read_ok) SetLastError(probe_error);
                    hop_log(HOP_LOG_WARN, "xfer: source changed while queued, %s", e->rel);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1;
                }
            }
            CloseHandle(h);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    if (!ack_mark_sent(batch,id)) { ack_unregister(batch,id); return XFER_LOCAL_IO; }
    if (!send_small(c, HOP_MSG_FILE_END, endp, 9, batch)) {
        ack_unregister(batch,id); return XFER_SEND_DEAD;
    }
    (void)local_fail;
    /* Arm before FILE_END to catch an immediate ACK; anchor expiry only after
     * the complete frame has been written. */
    uint64_t deadline_ns = rate_now_ns() +
        (uint64_t)HOP_XFER_ACK_TIMEOUT_MS * 1000000ull;
    *ack_deadline_ns = deadline_ns;
    if (!ack_set_deadline(batch,id,deadline_ns)) {
        ack_unregister(batch,id); return XFER_LOCAL_IO;
    }
    return XFER_ACK_PENDING;
}

/* Image payloads share the same rate limiter and owned-buffer path as files. */
static int send_image_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    uint8_t bp[20];
    hop_put_be32(bp, j->batch);
    hop_put_be32(bp + 4, 1);
    hop_put_be32(bp + 8, (uint32_t)j->w);
    hop_put_be32(bp + 12, (uint32_t)j->h);
    hop_put_be32(bp + 16, (uint32_t)j->stride);
    if (!send_small(c, HOP_MSG_IMAGE_BEGIN, bp, 20, j->batch)) return 0;

    uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
    uint64_t off = 0;
    while (off < total) {
        uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
        uint64_t crc_started_ns=rate_now_ns();
        uint32_t crc = hop_crc32(0, j->pixels + off, n);
        uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
        if (!send_chunk(c,tx,j->batch,1,off,j->pixels+off,n,crc,0,crc_elapsed_ns)) return 0;
        off += n;
        yield_cpu();
        if (cancel_requested(j->batch)) return -1;
        if (peer_aborted(j->batch)) return -2;
    }

    uint8_t endp[9];
    hop_put_be32(endp, 1);
    endp[4] = HOP_FILE_OK;
    hop_put_be32(endp + 5, 0);
    if (!send_small(c, HOP_MSG_FILE_END, endp, 9, j->batch)) return 0;
    rate_feedback(0);
    return 1;
}

/* 收尾：报告本批实际送达数，保留部分成功语义。 */
static int send_done(SOCKET c, const xfer_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp, j->batch);
    hop_put_be32(dp + 4, (uint32_t)j->ok);
    hop_put_be32(dp + 8, hop_xfer_job_total(j->is_image,j->n));
    hop_put_be64(dp + 12, 0);
    return send_small(c, HOP_MSG_XFER_DONE, dp, 20, j->batch);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */
#define JOB_COMMIT_UNKNOWN 4 /* receiver outcome is unknown */
#define JOB_COMMIT_REJECTED 5 /* receiver explicitly rejected completion */

/* Re-send only DONE, never file contents, while the same control session
 * survives. Receiver caches its last receipt and never reapplies effects. */
static int send_done_result(SOCKET fd, const xfer_job *job) {
    uint64_t until=now_ms()+HOP_XFER_RECONNECT_BUDGET_MS;
    while (now_ms()<until) {
        if (!hop_session_matches(&job->session)) return JOB_COMMIT_UNKNOWN;
        if (WaitForSingleObject(s_x.stop,0)==WAIT_OBJECT_0) return JOB_COMMIT_UNKNOWN;
        EnterCriticalSection(&s_x.lock);
        fd=s_x.peer_ready?s_x.client:INVALID_SOCKET;
        hop_session_id session=s_x.session;
        s_send_epoch=s_x.epoch;
        LeaveCriticalSection(&s_x.lock);
        if (fd==INVALID_SOCKET) { Sleep(100); continue; }
        if (!hop_session_id_equal(&session,&job->session)) return JOB_COMMIT_UNKNOWN;
        if (!ack_register(job->batch,HOP_DONE_ACK_SLOT)) return JOB_COMMIT_UNKNOWN;
        if (!ack_mark_sent(job->batch,HOP_DONE_ACK_SLOT)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN;
        }
        if (!send_done(fd,job)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); Sleep(100); continue;
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
        Sleep(100);
    }
    return JOB_COMMIT_UNKNOWN;
}

/* 实际发送（send 线程持有 tx 缓冲）。 */
static int run_job(SOCKET c, uint8_t *tx, xfer_job *j) {

    ack_unregister_batch(j->batch);
    if(cancel_requested(j->batch))return JOB_CANCELLED;
    if(peer_aborted(j->batch))return JOB_PEER_ABORT;
    if (j->is_image) {
        int image_result = send_image_job(c, tx, j);
        if (image_result == -1) return JOB_CANCELLED;
        if (image_result == -2) return JOB_PEER_ABORT;
        if (!image_result) return JOB_NET_DOWN;
        j->ok = 1;
        j->next_i = j->n;
        status_file_phase(HOP_XFER_DIR_SEND,j->batch,0,HOP_XFER_FILE_COMPLETED,1);
        return JOB_DONE;
    }

    typedef struct {
        const xfer_entry *entry;
        uint32_t id;
        int index;
        int attempts;
        int pending;
        int acknowledged;
        uint64_t deadline_ns;
    } window_item;

    while (j->next_i < j->n) {
        window_item window[HOP_XFER_ACK_WINDOW_CAPACITY];
        uint32_t window_count = 0;
        int send_index = j->next_i;
        int fill_result = JOB_DONE;

        /* Fill a bounded window using a local cursor. j->next_i is the
         * durable resume point and must advance only after each entry has a
         * final ACK/result; a disconnect during fill must not skip sent files. */
        while (window_count < HOP_XFER_ACK_WINDOW_CAPACITY && send_index < j->n) {
            if (cancel_requested(j->batch)) {
                ack_unregister_batch(j->batch);
                return JOB_CANCELLED;
            }
            if (peer_aborted(j->batch)) {
                ack_unregister_batch(j->batch);
                return JOB_PEER_ABORT;
            }

            int index = send_index;
            const xfer_entry *e = &j->e[index];
            uint32_t id = (uint32_t)(index + 1);
            window_item *item = &window[window_count];
            memset(item,0,sizeof(*item));
            item->entry = e;
            item->id = id;
            item->index = index;
            status_file_phase(HOP_XFER_DIR_SEND,j->batch,index,HOP_XFER_FILE_SENDING,0);

            int attempts = 0;
            int st = XFER_LOCAL_IO;
            uint64_t deadline_ns = 0;
            for (;;) {
                ++attempts;
                st = send_file_entry(c,tx,j->batch,id,e,&deadline_ns);
                if (st == XFER_ACK_PENDING) break;
                if (st == XFER_SEND_DEAD || st == XFER_PEER_ABORT ||
                    st == XFER_CANCELLED || st == HOP_FILE_ACK_BADPATH) break;
                if (attempts >= HOP_XFER_FILE_RETRY_MAX) break;
                unsigned d = hop_xfer_file_retry_delay_ms((unsigned)attempts);
                hop_log(HOP_LOG_WARN, "xfer: retry %d/%d for %s in %u ms (send status %d)",
                        attempts,HOP_XFER_FILE_RETRY_MAX,e->rel,d,st);
                Sleep(d);
            }

            if (st == XFER_SEND_DEAD) {
                /* Do not discard earlier ACKs already in this partially-filled
                 * window. Drain the contiguous prefix before reconnecting. */
                fill_result = JOB_NET_DOWN;
                break;
            }
            if (st == XFER_PEER_ABORT) {
                ack_unregister_batch(j->batch);
                return JOB_PEER_ABORT;
            }
            if (st == XFER_CANCELLED) {
                ack_unregister_batch(j->batch);
                return JOB_CANCELLED;
            }

            if (st != XFER_ACK_PENDING) {
                /* No remote FILE_ACK exists for this entry. Stop filling here;
                 * drain only earlier in-flight entries, then reconnect from
                 * this exact checkpoint instead of treating local failure as
                 * a completed result. */
                status_file_phase(HOP_XFER_DIR_SEND,j->batch,index,HOP_XFER_FILE_FAILED,(uint32_t)attempts);
                hop_log(HOP_LOG_ERROR, "xfer: no ACK for %s after %d attempt(s) (status %d); keep checkpoint",
                        e->rel,attempts,st);
                fill_result = JOB_NET_DOWN;
                break;
            }
            item->attempts = attempts;
            item->pending = 1;
            item->deadline_ns = deadline_ns;
            ++window_count;
            ++send_index;
        }

        /* Drain in file order. If the link dies, j->next_i still points at
         * the first unresolved entry, while earlier confirmed entries remain
         * committed and are not retransmitted after reconnect. */
        for (uint32_t wi = 0; wi < window_count; ++wi) {
            window_item *item = &window[wi];
            if (item->pending) {
                uint64_t rtt_us = 0;
                int st = ack_wait(j->batch,item->id,item->deadline_ns,&rtt_us);
                if (st >= 0) item->acknowledged = 1;
                if (rtt_us) rate_feedback(rtt_us);
                int attempts = item->attempts;

                while (st != HOP_FILE_ACK_OK && st != HOP_FILE_ACK_BADPATH) {
                    if (st == XFER_ACK_TIMEOUT) {
                        /* A timeout is not a peer result. Reset this data
                         * connection before retrying the same batch/id so a
                         * delayed ACK from the old attempt cannot satisfy the
                         * new one. The contiguous checkpoint stays unchanged. */
                        hop_log(HOP_LOG_WARN,
                                "xfer: ACK timeout for %s; reconnect before retrying entry %d",
                                item->entry->rel,item->index);
                        ack_unregister_batch(j->batch);
                        xfer_kick();
                        return JOB_NET_DOWN;
                    }
                    if (st == XFER_SEND_DEAD) {
                        ack_unregister_batch(j->batch);
                        return JOB_NET_DOWN;
                    }
                    if (st == XFER_PEER_ABORT) {
                        ack_unregister_batch(j->batch);
                        return JOB_PEER_ABORT;
                    }
                    if (st == XFER_CANCELLED) {
                        ack_unregister_batch(j->batch);
                        return JOB_CANCELLED;
                    }
                    if (attempts >= HOP_XFER_FILE_RETRY_MAX) break;

                    unsigned d = hop_xfer_file_retry_delay_ms((unsigned)attempts);
                    hop_log(HOP_LOG_WARN, "xfer: retry %d/%d for %s in %u ms (ACK status %d)",
                            attempts,HOP_XFER_FILE_RETRY_MAX,item->entry->rel,d,st);
                    Sleep(d);

                    ++attempts;
                    uint64_t retry_deadline_ns = 0;
                    st = send_file_entry(c,tx,j->batch,item->id,item->entry,&retry_deadline_ns);
                    if (st == XFER_ACK_PENDING) {
                        rtt_us = 0;
                        st = ack_wait(j->batch,item->id,retry_deadline_ns,&rtt_us);
                        if (st >= 0) item->acknowledged = 1;
                        if (rtt_us) rate_feedback(rtt_us);
                    }
                }

                if (st == HOP_FILE_ACK_OK) {
                    j->ok++;
                    status_file_phase(HOP_XFER_DIR_SEND,j->batch,item->index,
                                      HOP_XFER_FILE_COMPLETED,(uint32_t)attempts);
                } else {
                    status_file_phase(HOP_XFER_DIR_SEND,j->batch,item->index,
                                      HOP_XFER_FILE_FAILED,(uint32_t)attempts);
                    hop_log(HOP_LOG_ERROR, "xfer: giving up on %s after %d attempt(s) (status %d)",
                            item->entry->rel,attempts,st);
                }
            }
            /* This is the only place that advances the reconnect checkpoint. */
            if (!item->acknowledged) {
                hop_log(HOP_LOG_ERROR, "xfer: entry %d has no remote ACK; checkpoint remains %d",
                        item->index,j->next_i);
                ack_unregister_batch(j->batch);
                return JOB_NET_DOWN;
            }
            if (!hop_xfer_ack_window_advance_checkpoint(&j->next_i,item->index,1)) {
                hop_log(HOP_LOG_ERROR, "xfer: non-contiguous checkpoint batch=%u current=%d resolved=%d",
                        j->batch,j->next_i,item->index);
                ack_unregister_batch(j->batch);
                return JOB_NET_DOWN;
            }
        }
        if (fill_result != JOB_DONE) {
            ack_unregister_batch(j->batch);
            return fill_result;
        }
    }
    ack_unregister_batch(j->batch);
    return JOB_DONE;
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(SOCKET c, uint32_t batch, uint8_t reason, uint8_t target) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = target;
    send_small(c, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
static void send_ack(SOCKET c, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(c, HOP_MSG_FILE_ACK, b, 9, batch);
}
