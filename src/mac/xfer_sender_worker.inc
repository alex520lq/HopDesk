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

static void job_defer_to_shutdown(xf_job *j, xfer_job_owner expected_owner) {
    if (!j || expected_owner == XFER_JOB_OWNER_SHUTDOWN) return;
    pthread_mutex_lock(&s_x.qlock);
    if (j->owner == expected_owner) {
        j->owner = XFER_JOB_OWNER_SHUTDOWN;
        j->next = s_x.shutdown_jobs;
        s_x.shutdown_jobs = j;
    }
    pthread_mutex_unlock(&s_x.qlock);
}

static int job_release_task_resources(xf_job *j) {
    if (!j) return 0;
    /* The Job owns the pixel allocation and the Task owns its image-temp
     * reservation. Drop the allocation first, then return the exact ledger
     * bytes before Task terminalization (which rejects live temp bytes). */
    if (j->pixels) {
        free(j->pixels);
        j->pixels = NULL;
    }
    if (j->task.initialized) {
        uint64_t bytes = hop_xfer_task_image_temp_bytes(&j->task);
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

static int job_free(xf_job *j, xfer_job_owner expected_owner) {
    if (!j) return 1;
    if (j->owner != expected_owner) {
        hop_log(HOP_LOG_ERROR, "xfer: refusing Job release by non-owner (batch=%u owner=%d expected=%d)",
                j->batch, (int)j->owner, (int)expected_owner);
        return -1;
    }

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
                sender_job_cancel_requested(&j->sender_context,j->batch,j->task.id))
                state = HOP_XFER_TASK_CANCELLED;
            sender_task_finish(&j->sender_context,&j->task,state);
            if (!hop_xfer_task_is_terminal(&j->task)) {
                job_defer_to_shutdown(j, expected_owner);
                hop_log(HOP_LOG_ERROR, "xfer: Job Task terminalization deferred (batch=%u task=%llu)",
                        j->batch, (unsigned long long)j->task.id);
                return 0;
            }
        }
        if (!sender_task_dispose(&j->sender_context, &j->task) ||
            j->task.initialized || j->task.id) {
            job_defer_to_shutdown(j, expected_owner);
            hop_log(HOP_LOG_ERROR, "xfer: Job Task disposal deferred (batch=%u task=%llu)",
                    j->batch, (unsigned long long)j->task.id);
            return 0;
        }
    }

    j->owner = XFER_JOB_OWNER_RELEASED;
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
    return call&&call->job&&sender_job_cancel_requested(&call->job->sender_context,batch,task_id);
}
static int sender_worker_is_stopping(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    return !call || !call->job || !call->job->sender_context.is_stopping ||
        call->job->sender_context.is_stopping(call->job->sender_context.user);
}
static int sender_worker_begin_completion(void *user, uint64_t task_id) {
    sender_worker_call *call=(sender_worker_call *)user;
    return call&&call->job&&call->job->task.id==task_id&&
        sender_task_begin_completion(&call->job->sender_context,&call->job->task);
}
static int sender_worker_set_queued(void *user, uint64_t task_id) {
    sender_worker_call *call=(sender_worker_call *)user;
    return call&&call->job&&call->job->task.id==task_id&&
        sender_task_set_state(&call->job->sender_context,&call->job->task,HOP_XFER_TASK_QUEUED);
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
    if(!call||!call->job||call->job->owner!=XFER_JOB_OWNER_WORKER)return 0;
    pthread_mutex_lock(&s_x.qlock);
    int queued=call->job->owner==XFER_JOB_OWNER_WORKER &&
        !sender_worker_is_stopping(call) &&
        hop_xfer_sender_queue_push_front(&s_x.queue,&s_sender_queue_ops,call->job);
    if(queued){call->job->owner=XFER_JOB_OWNER_QUEUE;pthread_cond_signal(&s_x.qcond);}
    pthread_mutex_unlock(&s_x.qlock);
    return queued;
}
static int sender_worker_peer_cancelled_and_clear_current(void *user,uint32_t batch,uint64_t task_id) {
    (void)user;(void)task_id;
    pthread_mutex_lock(&s_x.qlock);
    int cancelled=s_x.abort_batch==batch&&s_x.abort_reason==HOP_ABORT_CANCEL;
    s_x.cur_batch=0;s_x.cur_task_id=0;
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
    return (hop_xfer_sender_job_result)send_done_result(call->socket,call->job);
}
static void sender_worker_task_finish(void *user,hop_xfer_task_state state) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job)return;
    xf_job *job=call->job;
    job->pending_finish_state=state;
    job->pending_finish_valid=1;
    job->pending_status_end=1;
    if(job_release_task_resources(job))
        sender_task_finish(&job->sender_context,&job->task,state);
}
static void sender_worker_status_end(void *user,uint32_t batch,int phase,const char *detail) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job){
        sender_status_end(&call->job->sender_context,batch,phase,detail);
        call->job->pending_status_end=0;
    }
}
static void sender_worker_dispose_job(void *user) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(call&&call->job){
        int released=job_free(call->job,XFER_JOB_OWNER_WORKER);
        if(released>=0)call->job=NULL; /* 0 transfers ownership to shutdown_jobs */
    }
}
static void sender_worker_log_event(void *user,hop_xfer_sender_worker_log_event event,
    const hop_xfer_sender_worker_job *state,unsigned delay_ms) {
    sender_worker_call *call=(sender_worker_call *)user;
    if(!call||!call->job||!state)return;
    const hop_xfer_sender_context *context=&call->job->sender_context;
    switch(event) {
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_INVALID:
            sender_logf(context,HOP_LOG_ERROR,"xfer: invalid retry policy input for batch %u",state->batch);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_SCHEDULED:
            sender_logf(context,HOP_LOG_WARN,"xfer: batch %u interrupted at entry %d, retry %u/%u in %u ms",
                state->batch,state->next_index,state->attempt,HOP_XFER_JOB_RETRY_MAX-1,delay_ms);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_STATE_FAILED:
            sender_logf(context,HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                (unsigned long long)state->task_id);break;
        case HOP_XFER_SENDER_WORKER_LOG_REQUEUE_FAILED:
            sender_logf(context,HOP_LOG_ERROR,"xfer: failed to requeue batch %u after reconnect",state->batch);break;
        case HOP_XFER_SENDER_WORKER_LOG_RETRY_EXHAUSTED:
            sender_logf(context,HOP_LOG_ERROR,"xfer: batch %u abandoned after %u attempt(s), %d/%d delivered",
                state->batch,state->attempt,state->ok_count,state->entry_count);break;
        case HOP_XFER_SENDER_WORKER_LOG_PARTIAL_DONE:
            sender_logf(context,HOP_LOG_WARN,"xfer: batch %u finished %d/%d",
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
        job->pending_finish_state=HOP_XFER_TASK_FAILED;
        job->pending_finish_valid=1;
        sender_status_end(&job->sender_context,job->batch,HOP_XFER_FAILED,
                          "Invalid sender worker context");
        (void)job_free(job,XFER_JOB_OWNER_WORKER);
        return HOP_XFER_SENDER_WORKER_FINISHED;
    }
    return action;
}
static int sender_shutdown_drain_jobs(void) {
    pthread_mutex_lock(&s_x.qlock);
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
        int was_cancelled=sender_job_cancel_requested(&job->sender_context,
                                                       job->batch,job->task.id);
        if (terminal_was_pending && !job->pending_finish_valid) {
            job->pending_finish_state=was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_INTERRUPTED;
            job->pending_finish_valid=1;
            job->pending_status_end=1;
        }
        hop_xfer_sender_context status_context=job->sender_context;
        uint32_t status_batch=job->batch;
        int emit_status=job->pending_status_end;
        hop_xfer_task_state finish_state=job->pending_finish_state;
        int released=job_free(job,XFER_JOB_OWNER_SHUTDOWN);
        if (released!=1) return 0; /* retain the list head and its Reservation */
        if (emit_status)
            sender_status_end(&status_context,status_batch,
                finish_state==HOP_XFER_TASK_CANCELLED?HOP_XFER_CANCELLED:HOP_XFER_FAILED,
                finish_state==HOP_XFER_TASK_CANCELLED?"Cancelled":"Transfer interrupted during shutdown");
        s_x.shutdown_jobs=next;
    }
    return 1;
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
            /* Queue ownership is transferred to Shutdown after both workers join. */
            pthread_mutex_unlock(&s_x.qlock);
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
        if (expired) expired->owner=XFER_JOB_OWNER_WORKER;
        if (j) { j->owner=XFER_JOB_OWNER_WORKER; s_x.cur_batch = j->batch; s_x.cur_task_id = j->task.id; s_x.abort_batch=0; s_x.abort_reason=0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            sender_logf(&expired->sender_context, HOP_LOG_ERROR,
                        "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                        expired->batch, expired->ok, expired->n);
            int was_cancelled = sender_job_cancel_requested(
                &expired->sender_context, expired->batch, expired->task.id);
            pthread_mutex_lock(&s_x.slock);
            int abort_fd=s_x.peer_ready && hop_session_id_equal(&expired->session,&s_x.session) ? s_x.sock : -1;
            s_send_epoch=s_x.epoch;
            pthread_mutex_unlock(&s_x.slock);
            if (was_cancelled && abort_fd>=0)
                send_abort_msg_with_context(&expired->sender_context, abort_fd,
                                            expired->batch, HOP_ABORT_CANCEL);
            pthread_mutex_lock(&s_status_lock);
            int show_status=!s_send_status.status.active || s_send_status.status.batch==expired->batch;
            pthread_mutex_unlock(&s_status_lock);
            if (show_status) sender_status_start_job(&expired->sender_context, expired);
            expired->pending_finish_state=was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED;
            expired->pending_finish_valid=1;
            if (show_status) sender_status_end(&expired->sender_context, expired->batch,
                was_cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED,
                "Transfer stopped while queued");
            (void)job_free(expired,XFER_JOB_OWNER_WORKER);
            continue;
        }
        if (!j) continue;
        if (sender_context_is_stopping(&s_sender_platform)) {
            (void)job_free(j,XFER_JOB_OWNER_WORKER);
            continue;
        }
        pthread_mutex_lock(&s_x.slock);
        fd = s_x.peer_ready ? s_x.sock : -1;
        s_send_epoch = s_x.epoch;
        hop_session_id transport_session = s_x.session;
        pthread_mutex_unlock(&s_x.slock);
        job_bind_session(j, &transport_session);
        if (hop_session_id_valid(&j->session)) (void)sender_task_bind_session(&j->sender_context, &j->task,&j->session);
        if(!sender_task_set_state(&j->sender_context, &j->task,HOP_XFER_TASK_ACTIVE)){
            j->pending_finish_state=HOP_XFER_TASK_FAILED;j->pending_finish_valid=1;(void)job_free(j,XFER_JOB_OWNER_WORKER);continue;
        }
        sender_status_start_job(&j->sender_context, j);

        int r = (fd >= 0) ? run_job(fd, tx, j) : JOB_NET_DOWN;

        pthread_mutex_lock(&s_x.qlock);
        int peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
        int completion_path=(r==JOB_DONE);
        if (!completion_path) { s_x.cur_batch = 0; s_x.cur_task_id = 0; }
        pthread_mutex_unlock(&s_x.qlock);

        hop_xfer_sender_worker_action action = sender_worker_finish_job(
            j, fd, r, completion_path, peer_cancelled);
        if (action == HOP_XFER_SENDER_WORKER_REQUEUED) continue;
    }

    free(tx);
    return NULL;
}
