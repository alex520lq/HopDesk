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
    return sender_task_cancel_requested(&job->sender_context, job->batch, &job->task);
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
static const hop_xfer_sender_queue_ops s_sender_queue_ops = {
    NULL, sender_queue_next, sender_queue_set_next, sender_queue_is_cancelled,
    sender_queue_batch, sender_queue_retry_at, sender_queue_give_up_at
};

static void job_free(xf_job *j) {
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

static void *send_thread(void *arg) {
    (void)arg;

    /* 主动降到 UTILITY。控制通道承载的是键鼠，主队列是 USER_INTERACTIVE
     * 级别 —— 降级之后输入路径永远能直接抢占传输线程，
     * 不会出现"传文件时鼠标发涩"。 */
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    uint8_t *tx = (uint8_t *)malloc(CHUNK_HDR + s_x.chunk);
    if (!tx) { hop_log(HOP_LOG_ERROR, "xfer: tx alloc failed"); return NULL; }

    for (;;) {
        xf_job *j = NULL, *expired = NULL;
        int fd;

        /* Do not consume job retry attempts while the data channel is down.
         * Snapshot outside qlock to keep the slock/qlock order non-nested. */
        pthread_mutex_lock(&s_x.slock);
        int peer_ready_snapshot = s_x.peer_ready;
        pthread_mutex_unlock(&s_x.slock);

        pthread_mutex_lock(&s_x.qlock);
        int waited_for_queue = 0;
        /* Wait for an expired/cancelled job even offline, but only dequeue a
         * retry when the data channel was ready at the start of this pass.
         * A 200 ms poll refreshes the snapshot after reconnect. */
        for (;;) {
            if (s_x.stop) break;
            uint64_t n = now_ms();
            xf_job *p = (xf_job *)s_x.queue.head;
            if (p && (p->batch == s_x.cancel_batch ||
                      (p->give_up_at && n >= p->give_up_at) ||
                      (peer_ready_snapshot && p->retry_at <= n)))
                break;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s_x.qcond, &s_x.qlock, &ts);
            waited_for_queue = 1;
            break; /* refresh connection state outside qlock after every poll */
        }

        if (s_x.stop) {
            /* 退出时不再发新东西：队列直接释放，避免在这里空转 */
            xf_job *p = (xf_job *)hop_xfer_sender_queue_detach_all(&s_x.queue);
            pthread_mutex_unlock(&s_x.qlock);
            while (p) { xf_job *nx = p->next; p->next = NULL; job_free(p); p = nx; }
            break;
        }

        uint64_t now = now_ms();
        expired=(xf_job *)hop_xfer_sender_queue_take_cancelled(&s_x.queue, &s_sender_queue_ops);
        if (!expired) {
            hop_xfer_sender_queue_take_result take_result = HOP_XFER_SENDER_QUEUE_NONE;
            xf_job *taken = (xf_job *)hop_xfer_sender_queue_take_head(
                &s_x.queue, &s_sender_queue_ops, now, s_x.cancel_batch,
                peer_ready_snapshot, !waited_for_queue, &take_result);
            if (take_result == HOP_XFER_SENDER_QUEUE_EXPIRED) expired = taken;
            else if (take_result == HOP_XFER_SENDER_QUEUE_READY) j = taken;
        }
        if (j) { s_x.cur_batch = j->batch; s_x.cur_task_id = j->task.id; s_x.abort_batch=0; s_x.abort_reason=0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            sender_logf(&expired->sender_context, HOP_LOG_ERROR,
                        "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                        expired->batch, expired->ok, expired->n);
            int was_cancelled=cancel_requested(expired->batch) || sender_task_cancel_requested(&expired->sender_context, expired->batch, &expired->task);
            pthread_mutex_lock(&s_x.slock);
            int abort_fd=s_x.peer_ready && hop_session_id_equal(&expired->session,&s_x.session) ? s_x.sock : -1;
            s_send_epoch=s_x.epoch;
            pthread_mutex_unlock(&s_x.slock);
            if (was_cancelled && abort_fd>=0) send_abort_msg(abort_fd,expired->batch,HOP_ABORT_CANCEL);
            pthread_mutex_lock(&s_status_lock);
            int show_status=!s_send_status.status.active || s_send_status.status.batch==expired->batch;
            pthread_mutex_unlock(&s_status_lock);
            if (show_status) sender_status_start_job(&expired->sender_context, expired);
            sender_task_finish(&expired->sender_context, &expired->task,was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
            if (show_status) sender_status_end(&expired->sender_context, expired->batch,
                was_cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED,
                "Transfer stopped while queued");
            job_free(expired);
            continue;
        }
        if (!j) continue;
        pthread_mutex_lock(&s_x.slock);
        fd = s_x.peer_ready ? s_x.sock : -1;
        s_send_epoch = s_x.epoch;
        hop_session_id transport_session = s_x.session;
        pthread_mutex_unlock(&s_x.slock);
        job_bind_session(j, &transport_session);
        if (hop_session_id_valid(&j->session)) (void)sender_task_bind_session(&j->sender_context, &j->task,&j->session);
        if(!sender_task_set_state(&j->sender_context, &j->task,HOP_XFER_TASK_ACTIVE)){
            sender_task_finish(&j->sender_context, &j->task,HOP_XFER_TASK_FAILED);job_free(j);continue;
        }
        sender_status_start_job(&j->sender_context, j);

        int r = (fd >= 0) ? run_job(fd, tx, j) : JOB_NET_DOWN;

        pthread_mutex_lock(&s_x.qlock);
        int peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
        int completion_path=(r==JOB_DONE);
        if (!completion_path) { s_x.cur_batch = 0; s_x.cur_task_id = 0; }
        pthread_mutex_unlock(&s_x.qlock);

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
                sender_logf(&j->sender_context, HOP_LOG_ERROR,
                            "xfer: invalid retry policy input for batch %u", j->batch);
                sender_status_end(&j->sender_context, j->batch,
                                  HOP_XFER_FAILED, "Invalid retry policy");
                sender_task_finish(&j->sender_context, &j->task, HOP_XFER_TASK_FAILED);
                job_free(j);
                continue;
            }
            j->attempt = (int)retry_plan.attempt;
            j->give_up_at = retry_plan.give_up_at_ms;
            if (retry_result == HOP_XFER_SENDER_RETRY_SCHEDULED) {
                j->retry_at = retry_plan.retry_at_ms;
                sender_logf(&j->sender_context, HOP_LOG_WARN,
                            "xfer: batch %u interrupted at entry %d, retry %d/%d in %u ms",
                            j->batch, j->next_i, j->attempt,
                            HOP_XFER_JOB_RETRY_MAX - 1, d);
                if(!sender_task_set_state(&j->sender_context, &j->task,HOP_XFER_TASK_QUEUED)){
                    sender_logf(&j->sender_context, HOP_LOG_ERROR,
                                "xfer: task=%llu could not return to queued state",
                                (unsigned long long)j->task.id);
                    sender_status_end(&j->sender_context, j->batch,
                                      HOP_XFER_FAILED, "Invalid task retry state");
                    sender_task_finish(&j->sender_context, &j->task,HOP_XFER_TASK_FAILED);
                    job_free(j);continue;
                }
                /* 退回队首，保持 FIFO，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                pthread_mutex_lock(&s_x.qlock);
                int queued = hop_xfer_sender_queue_push_front(
                    &s_x.queue, &s_sender_queue_ops, j);
                if (queued) pthread_cond_signal(&s_x.qcond);
                pthread_mutex_unlock(&s_x.qlock);
                if (!queued) {
                    sender_logf(&j->sender_context, HOP_LOG_ERROR,
                                "xfer: failed to requeue batch %u after reconnect", j->batch);
                    sender_status_end(&j->sender_context, j->batch,
                                      HOP_XFER_FAILED, "Retry queue admission failed");
                    sender_task_finish(&j->sender_context, &j->task, HOP_XFER_TASK_FAILED);
                    job_free(j);
                }
                continue;
            }
            sender_logf(&j->sender_context, HOP_LOG_ERROR,
                        "xfer: batch %u abandoned after %d attempt(s), %d/%d delivered",
                        j->batch, j->attempt, j->ok, j->n);
        } else if (r == JOB_CANCELLED) {
            if (fd >= 0) send_abort_msg(fd, j->batch, HOP_ABORT_CANCEL);
        } else if (r == JOB_DONE) {
            if (j->ok < j->n)
                sender_logf(&j->sender_context, HOP_LOG_WARN,
                            "xfer: batch %u finished %d/%d",
                            j->batch, j->ok, j->n);
            r=send_done_result(fd, j);
        }
        if (completion_path) {
            pthread_mutex_lock(&s_x.qlock);
            peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
            s_x.cur_batch=0; s_x.cur_task_id=0;
            pthread_mutex_unlock(&s_x.qlock);
        }
        int complete = j->ok == (int)hop_xfer_job_total(j->is_image, j->n);
        hop_xfer_sender_terminal_decision terminal;
        if (hop_xfer_sender_get_terminal_decision(
                (hop_xfer_sender_job_result)r, peer_cancelled, complete, &terminal)) {
            sender_task_finish(&j->sender_context, &j->task, terminal.task_state);
            sender_status_end(&j->sender_context, j->batch,
                              terminal.status_phase, terminal.detail);
        } else {
            sender_task_finish(&j->sender_context, &j->task, HOP_XFER_TASK_FAILED);
            sender_status_end(&j->sender_context, j->batch,
                              HOP_XFER_FAILED, "Unknown sender result");
        }
        job_free(j);
    }

    free(tx);
    return NULL;
}
