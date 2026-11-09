/* 从作业队列里摘掉 cur。prev 是它的前驱（cur 是头时为 NULL）。
 *
 * 尾指针不能简单地"摘到尾巴就置 NULL"：cur 是尾巴但前面还有节点时，
 * q_tail 必须指回前驱。置 NULL 之后下一次入队会走
 * `if (q_tail) ... else q_head = j` 的 else 分支，把 q_head 直接覆盖掉 ——
 * 前面排队的作业既不会被发送，也不会被释放，凭空消失。
 * 调用方必须持有 s_x.qlock。 */
static void queue_unlink(xf_job *prev, xf_job *cur) {
    if (prev) prev->next = cur->next;
    else      s_x.q_head = cur->next;
    if (s_x.q_tail == cur) s_x.q_tail = prev;
}

/* Caller holds the queue lock. Removing a cancelled later entry must not
 * change FIFO order or lose the tail of the remaining queue. */
static xf_job *queue_take_cancelled(void) {
    xf_job *prev=NULL, *cur=s_x.q_head;
    while (cur && !hop_xfer_task_cancel_requested(cur->task.id)) { prev=cur; cur=cur->next; }
    if (cur) { queue_unlink(prev,cur); cur->next=NULL; }
    return cur;
}

static void job_free(xf_job *j) {
    if (!j) return;
    for (int i = 0; i < j->n; ++i) {
        free(j->e[i].full);
        free(j->e[i].name);
        free(j->e[i].rel);
    }
    free(j->e);
    free(j->pixels);
    hop_xfer_task_dispose(&j->task);
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
            xf_job *p = s_x.q_head;
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
            xf_job *p = s_x.q_head;
            s_x.q_head = s_x.q_tail = NULL;
            pthread_mutex_unlock(&s_x.qlock);
            while (p) { xf_job *nx = p->next; job_free(p); p = nx; }
            break;
        }

        uint64_t now = now_ms();
        expired=queue_take_cancelled();
        xf_job *prev=NULL, *cur=expired ? NULL : s_x.q_head;
        while (cur) {
            /* 重连预算用完：连接在不在都放弃 */
            if (cur->batch==s_x.cancel_batch || (cur->give_up_at && now >= cur->give_up_at)) {
                expired = cur;
                queue_unlink(prev, cur);
                expired->next = NULL;
                break;
            }
            if (!waited_for_queue && peer_ready_snapshot && cur->retry_at <= now) {
                j = cur;
                queue_unlink(prev, cur);
                break;
            }
            break; /* FIFO: do not overtake a paused batch. */
        }
        if (j) { s_x.cur_batch = j->batch; s_x.cur_task_id = j->task.id; s_x.abort_batch=0; s_x.abort_reason=0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                    expired->batch, expired->ok, expired->n);
            int was_cancelled=cancel_requested(expired->batch) || hop_xfer_task_cancel_requested(expired->task.id);
            pthread_mutex_lock(&s_x.slock);
            int abort_fd=s_x.peer_ready && hop_session_id_equal(&expired->session,&s_x.session) ? s_x.sock : -1;
            s_send_epoch=s_x.epoch;
            pthread_mutex_unlock(&s_x.slock);
            if (was_cancelled && abort_fd>=0) send_abort_msg(abort_fd,expired->batch,HOP_ABORT_CANCEL);
            pthread_mutex_lock(&s_status_lock);
            int show_status=!s_send_status.status.active || s_send_status.status.batch==expired->batch;
            pthread_mutex_unlock(&s_status_lock);
            if (show_status) status_start_job(expired);
            hop_xfer_task_finish(&expired->task,was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
            if (show_status) status_end(HOP_XFER_DIR_SEND,expired->batch,was_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued");
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
        if (hop_session_id_valid(&j->session)) (void)hop_xfer_task_bind_session(&j->task,j->session.bytes);
        if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_ACTIVE)){
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);job_free(j);continue;
        }
        status_start_job(j);

        int r = (fd >= 0) ? run_job(fd, tx, j) : JOB_NET_DOWN;

        pthread_mutex_lock(&s_x.qlock);
        int peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
        int completion_path=(r==JOB_DONE);
        if (!completion_path) { s_x.cur_batch = 0; s_x.cur_task_id = 0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (r != JOB_DONE && hop_xfer_task_cancel_requested(j->task.id)) r=JOB_CANCELLED;
        if (r == JOB_DONE && !hop_xfer_task_begin_completion(&j->task)) r=JOB_CANCELLED;
        if (r == JOB_NET_DOWN) {
            if (j->give_up_at == 0)
                j->give_up_at = now_ms() + HOP_XFER_RECONNECT_BUDGET_MS;
            j->attempt++;
            if (j->attempt < HOP_XFER_JOB_RETRY_MAX && now_ms() < j->give_up_at) {
                unsigned d = hop_xfer_job_retry_delay_ms(j->attempt);
                j->retry_at = now_ms() + d;
                hop_log(HOP_LOG_WARN,
                        "xfer: batch %u interrupted at entry %d, retry %d/%d in %u ms",
                        j->batch, j->next_i, j->attempt,
                        HOP_XFER_JOB_RETRY_MAX - 1, d);
                if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_QUEUED)){
                    hop_log(HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                            (unsigned long long)j->task.id);
                    status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Invalid task retry state");
                    hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                    job_free(j);continue;
                }
                /* 退回队首，保持 FIFO，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                j->next = NULL;
                pthread_mutex_lock(&s_x.qlock);
                j->next=s_x.q_head; s_x.q_head=j;
                if(!s_x.q_tail)s_x.q_tail=j;
                pthread_cond_signal(&s_x.qcond);
                pthread_mutex_unlock(&s_x.qlock);
                continue;
            }
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned after %d attempt(s), %d/%d delivered",
                    j->batch, j->attempt, j->ok, j->n);
        } else if (r == JOB_CANCELLED) {
            if (fd >= 0) send_abort_msg(fd, j->batch, HOP_ABORT_CANCEL);
        } else if (r == JOB_DONE) {
            if (j->ok < j->n)
                hop_log(HOP_LOG_WARN, "xfer: batch %u finished %d/%d",
                        j->batch, j->ok, j->n);
            r=send_done_result(fd, j);
        }
        if (completion_path) {
            pthread_mutex_lock(&s_x.qlock);
            peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
            s_x.cur_batch=0; s_x.cur_task_id=0;
            pthread_mutex_unlock(&s_x.qlock);
        }
        if (r == JOB_DONE) {
            int complete=j->ok==(int)hop_xfer_job_total(j->is_image,j->n);
            hop_xfer_task_finish(&j->task,complete?HOP_XFER_TASK_COMPLETED:HOP_XFER_TASK_PARTIAL);
            status_end(HOP_XFER_DIR_SEND,j->batch,complete?HOP_XFER_COMPLETED:HOP_XFER_PARTIAL,
                       complete?"Transfer completed":"Some files failed");
        } else if (r == JOB_CANCELLED) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_CANCELLED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_CANCELLED,"Cancelled");
        } else if (r == JOB_PEER_ABORT) {
            hop_xfer_task_finish(&j->task,peer_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,peer_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,peer_cancelled?"Cancelled by receiver":"Peer aborted transfer");
        } else if (r == JOB_COMMIT_REJECTED) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Receiver rejected completion");
        } else if (r == JOB_COMMIT_UNKNOWN) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_INTERRUPTED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Completion delivery uncertain; peer may have received it");
        } else if (r == JOB_NET_DOWN) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Reconnect attempts exhausted");
        }
        job_free(j);
    }

    free(tx);
    return NULL;
}
