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
    return sender_task_cancel_requested(&job->sender_context, job->batch, &job->task);
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
static const hop_xfer_sender_queue_ops s_sender_queue_ops = {
    NULL, sender_queue_next, sender_queue_set_next, sender_queue_is_cancelled,
    sender_queue_batch, sender_queue_retry_at, sender_queue_give_up_at
};

static void job_free(xfer_job *j) {
    if (!j) return;
    for (int i = 0; i < j->n; ++i) {
        free(j->e[i].full);
        free(j->e[i].name);
        free(j->e[i].rel);
    }
    free(j->e);
    free(j->pixels);
    sender_task_dispose(&j->sender_context, &j->task);
    free(j);
}

static DWORD WINAPI send_thread(LPVOID arg) {
    (void)arg;

    /* 主动降到 BELOW_NORMAL。控制通道承载的是键鼠，钩子回调跑在主线程
     * （NORMAL 优先级）—— 降级之后输入路径永远能直接抢占传输线程，
     * 不会出现"传文件时鼠标发涩"。这是"传输不影响控制"的主要保障。 */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    uint8_t *tx = (uint8_t *)malloc(CHUNK_HDR + s_x.chunk);
    if (!tx) { hop_log(HOP_LOG_ERROR, "xfer: tx buffer alloc failed"); return 1; }

    HANDLE ev[2] = { s_x.stop, s_x.q_evt };
    for (;;) {
        /* 200 ms 一次：既够快响应用户的取消，又不至于空转烧 CPU。
         * 队列里有等待重连的作业时，这个轮询就是它唯一的唤醒源。 */
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        for (;;) {
            EnterCriticalSection(&s_x.lock);
            SOCKET c = s_x.peer_ready ? s_x.client : INVALID_SOCKET;
            s_send_epoch = s_x.epoch;
            hop_session_id transport_session = s_x.session;
            xfer_job *j = NULL, *expired = NULL;
            uint64_t now = now_ms();

            /* 一次只摘一个，摘到就跳出、在锁外处理 —— 队列操作留在锁里，
             * 释放和日志留在锁外，锁的持有时间永远是最短的。 */
            expired=(xfer_job *)hop_xfer_sender_queue_take_cancelled(&s_x.queue, &s_sender_queue_ops);
            if (!expired) {
                hop_xfer_sender_queue_take_result take_result = HOP_XFER_SENDER_QUEUE_NONE;
                xfer_job *taken = (xfer_job *)hop_xfer_sender_queue_take_head(&s_x.queue, &s_sender_queue_ops, now, s_x.cancel_batch, c != INVALID_SOCKET, 1, &take_result);
                if (take_result == HOP_XFER_SENDER_QUEUE_EXPIRED) expired = taken;
                else if (take_result == HOP_XFER_SENDER_QUEUE_READY) j = taken;
            }
            if (j) { s_x.cur_batch = j->batch; s_x.cur_task_id = j->task.id; s_x.abort_batch=0; s_x.abort_reason=0; }
            LeaveCriticalSection(&s_x.lock);

            if (expired) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                        expired->batch, expired->ok, expired->n);
                int was_cancelled=cancel_requested(expired->batch) || sender_task_cancel_requested(&expired->sender_context, expired->batch, &expired->task);
                if (was_cancelled && c != INVALID_SOCKET && hop_session_id_equal(&expired->session,&transport_session))
                    send_abort_msg(c,expired->batch,HOP_ABORT_CANCEL,HOP_ABORT_TO_RECEIVER);
                AcquireSRWLockShared(&s_status_lock);
                int show_status=!s_send_status.status.active || s_send_status.status.batch==expired->batch;
                ReleaseSRWLockShared(&s_status_lock);
                if (show_status) status_start_job(expired);
                sender_task_finish(&expired->sender_context, &expired->task,was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
                if (show_status) status_end(HOP_XFER_DIR_SEND,expired->batch,was_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued");
                job_free(expired);
                continue;
            }

            if (!j) break;
            job_bind_session(j, &transport_session);
            if (hop_session_id_valid(&j->session)) (void)sender_task_bind_session(&j->sender_context, &j->task,&j->session);
            if(!sender_task_set_state(&j->sender_context, &j->task,HOP_XFER_TASK_ACTIVE)){
                sender_task_finish(&j->sender_context, &j->task,HOP_XFER_TASK_FAILED);job_free(j);continue;
            }
            status_start_job(j);

            int r = run_job(c, tx, j);

            EnterCriticalSection(&s_x.lock);
            int peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
            int completion_path=(r==JOB_DONE);
            if (!completion_path) { s_x.cur_batch = 0; s_x.cur_task_id = 0; }
            LeaveCriticalSection(&s_x.lock);

            if (r != JOB_DONE && sender_task_cancel_requested(&j->sender_context, j->batch, &j->task)) r=JOB_CANCELLED;
            if (r == JOB_DONE && !sender_task_begin_completion(&j->sender_context, &j->task)) r=JOB_CANCELLED;
            if (r == JOB_NET_DOWN) {
                uint64_t retry_now = now_ms();
                uint32_t current_attempt = j->attempt > 0
                    ? (uint32_t)j->attempt : 0u;
                uint32_t next_attempt = current_attempt + 1u;
                unsigned d = next_attempt < HOP_XFER_JOB_RETRY_MAX
                    ? hop_xfer_job_retry_delay_ms((int)next_attempt) : 0u;
                hop_xfer_sender_retry_plan retry_plan;
                hop_xfer_sender_retry_result retry_result = hop_xfer_sender_plan_retry(
                    retry_now, j->give_up_at, current_attempt, HOP_XFER_JOB_RETRY_MAX,
                    HOP_XFER_RECONNECT_BUDGET_MS, d, &retry_plan);
                if (retry_result == HOP_XFER_SENDER_RETRY_INVALID) {
                    hop_log(HOP_LOG_ERROR, "xfer: invalid retry policy input for batch %u", j->batch);
                    status_end(HOP_XFER_DIR_SEND, j->batch, HOP_XFER_FAILED, "Invalid retry policy");
                    sender_task_finish(&j->task, HOP_XFER_TASK_FAILED);
                    job_free(j);
                    continue;
                }
                j->attempt = (int)retry_plan.attempt;
                j->give_up_at = retry_plan.give_up_at_ms;
                if (retry_result == HOP_XFER_SENDER_RETRY_SCHEDULED) {
                    j->retry_at = retry_plan.retry_at_ms;
                    hop_log(HOP_LOG_WARN,
                            "xfer: batch %u interrupted at entry %d, retry %d/%d in %u ms",
                            j->batch, j->next_i, j->attempt,
                            HOP_XFER_JOB_RETRY_MAX - 1, d);
                    if(!sender_task_set_state(&j->task,HOP_XFER_TASK_QUEUED)){
                        hop_log(HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                                (unsigned long long)j->task.id);
                        status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Invalid task retry state");
                        sender_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                        job_free(j);continue;
                    }
                    /* 退回队首，保持 FIFO，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                    EnterCriticalSection(&s_x.lock);
                    int queued = hop_xfer_sender_queue_push_front(
                        &s_x.queue, &s_sender_queue_ops, j);
                    LeaveCriticalSection(&s_x.lock);
                    if (!queued) {
                        hop_log(HOP_LOG_ERROR, "xfer: failed to requeue batch %u after reconnect", j->batch);
                        status_end(HOP_XFER_DIR_SEND, j->batch, HOP_XFER_FAILED, "Retry queue admission failed");
                        sender_task_finish(&j->task, HOP_XFER_TASK_FAILED);
                        job_free(j);
                    }
                    continue;
                }
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u abandoned after %d attempt(s), %d/%d delivered",
                        j->batch, j->attempt, j->ok, j->n);
            } else if (r == JOB_CANCELLED) {
                send_abort_msg(c, j->batch, HOP_ABORT_CANCEL, HOP_ABORT_TO_RECEIVER);
            } else if (r == JOB_DONE) {
                if (j->ok < j->n)
                    hop_log(HOP_LOG_WARN, "xfer: batch %u finished %d/%d",
                            j->batch, j->ok, j->n);
                r=send_done_result(c, j);
            }
            if (completion_path) {
                EnterCriticalSection(&s_x.lock);
                peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
                s_x.cur_batch=0; s_x.cur_task_id=0;
                LeaveCriticalSection(&s_x.lock);
            }
            int complete = j->ok == (int)hop_xfer_job_total(j->is_image, j->n);
            hop_xfer_sender_terminal_decision terminal;
            if (hop_xfer_sender_get_terminal_decision(
                    (hop_xfer_sender_job_result)r, peer_cancelled, complete, &terminal)) {
                sender_task_finish(&j->task, terminal.task_state);
                status_end(HOP_XFER_DIR_SEND, j->batch,
                           terminal.status_phase, terminal.detail);
            } else {
                sender_task_finish(&j->task, HOP_XFER_TASK_FAILED);
                status_end(HOP_XFER_DIR_SEND, j->batch,
                           HOP_XFER_FAILED, "Unknown sender result");
            }
            job_free(j);
        }
    }
    free(tx);
    return 0;
}
