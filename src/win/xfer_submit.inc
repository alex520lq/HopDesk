static int scanner_reserve_entry(void *context, uint64_t bytes) {
    return hop_xfer_task_reserve_entry((hop_xfer_task *)context, bytes);
}
static int scanner_is_cancelled(void *context) {
    hop_xfer_task *task = (hop_xfer_task *)context;
    return task && hop_xfer_task_cancel_requested(task->id);
}
static void *scanner_realloc_entries(void *context, void *memory, uint64_t bytes) {
    (void)context;
    if (bytes > (uint64_t)SIZE_MAX) return NULL;
    return realloc(memory, (size_t)bytes);
}
static int scanner_watchdog_begin(void *context, const char *operation, unsigned timeout_ms) {
    (void)context;
    return hop_watchdog_begin(operation, timeout_ms);
}
static void scanner_watchdog_end(void *context, int ticket) {
    (void)context;
    hop_watchdog_end(ticket);
}

static int submit(xfer_job *j) {
    if (!j) return 0;
    j->sender_context = s_sender_context;
    int ok = 0, duplicate = 0;
    EnterCriticalSection(&s_x.lock);
    if (s_x.cur_batch == j->batch ||
        hop_xfer_sender_queue_contains_batch(&s_x.queue, &s_sender_queue_ops, j->batch))
        duplicate = 1;
    if (!duplicate && s_x.client != INVALID_SOCKET && s_x.peer_ready)
        ok = hop_xfer_sender_queue_push_back(&s_x.queue, &s_sender_queue_ops, j);
    LeaveCriticalSection(&s_x.lock);
    if (ok) SetEvent(s_x.q_evt);
    return ok;
}

static int xfer_submit_paths_impl(const wchar_t *const *paths, int n, uint32_t requested_batch, uint64_t offer_id) {
    if (!paths || n <= 0) return 0;
    if (requested_batch && !(requested_batch & HOP_XFER_PASTE_BATCH_FLAG)) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop %d path(s)", n);
        return 0;
    }

    uint32_t batch = requested_batch;
    if (!batch) { do { batch = InterlockedIncrement((LONG *)&s_x.batch_seq); } while (!batch); }

    elist l;
    hop_xfer_task task;
    memset(&l, 0, sizeof(l));
    memset(&task, 0, sizeof(task));
    if (!hop_xfer_task_init_classified(&task,offer_id,requested_batch,batch,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES)) return 0;
    if (!hop_xfer_task_reserve_job(&task)) {
        uint64_t task_id=task.id;
        hop_xfer_task_finish(&task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_xfer_task_dispose(&task);
        hop_log(HOP_LOG_WARN, "xfer: global job budget exhausted (task=%llu)",
                (unsigned long long)task_id);
        return 0;
    }
    if (!hop_xfer_task_set_state(&task, HOP_XFER_TASK_SCANNING)) {
        hop_xfer_task_finish(&task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_xfer_task_dispose(&task);
        return 0;
    }

    hop_xfer_scan_services scan_services = {
        &task, scanner_reserve_entry, scanner_is_cancelled, scanner_realloc_entries,
        scanner_watchdog_begin, scanner_watchdog_end
    };
    uint64_t scan_started_ns=rate_now_ns();
    hop_xfer_scan_paths(&l, paths, n, &scan_services);
    uint64_t scan_elapsed_ns=rate_now_ns()-scan_started_ns;

    if (l.failed || l.n == 0 || hop_xfer_task_cancel_requested(task.id)) {
        if (l.budget_rejected)
            hop_log(HOP_LOG_WARN, "xfer: global entries/pending-bytes budget exceeded; batch rejected (task=%llu)",
                    (unsigned long long)task.id);
        else
            hop_log(HOP_LOG_WARN, "xfer: source tree rejected (scan_error=%d); no partial transfer queued (task=%llu)",
                    (int)l.error, (unsigned long long)task.id);
        hop_xfer_task_finish(&task, hop_xfer_task_cancel_requested(task.id) ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_xfer_task_dispose(&task);
        return 0;
    }

    xfer_job *job = (xfer_job *)calloc(1, sizeof(*job));
    if (!job) {
        hop_xfer_task_finish(&task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_xfer_task_dispose(&task);
        return 0;
    }

    job->batch = batch;
    job->scan_ns = scan_elapsed_ns;
    job->n = l.n;
    job->e = l.v;
    if (!hop_xfer_task_set_state(&task, HOP_XFER_TASK_QUEUED) ||
        !hop_xfer_task_move(&job->task, &task)) {
        free(job);
        hop_xfer_task_finish(&task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    l.v = NULL; l.n = l.cap = 0;

    uint64_t total = 0;
    for (int i = 0; i < job->n; ++i) total += job->e[i].size;

    hop_log(HOP_LOG_INFO, "xfer: queued batch %u task=%llu, %d entries, %.1f MB",
            job->batch, (unsigned long long)job->task.id, job->n, (double)total / 1048576.0);

    if (!submit(job)) {
        hop_xfer_task_finish(&job->task, HOP_XFER_TASK_FAILED);
        job_free(job);
        return 0;
    }
    return 1;
}

static void release_image_source_reservation(hop_xfer_task *source_task, uint64_t bytes) {
    if (source_task && bytes &&
        !hop_xfer_task_release_image_temp(source_task, bytes)) {
        hop_log(HOP_LOG_ERROR, "xfer: source image reservation release failed (task=%llu, bytes=%llu)",
                (unsigned long long)source_task->id, (unsigned long long)bytes);
    }
}

static int xfer_submit_image_impl(const uint8_t *bgra, uint8_t *owned, int w, int h, int stride,
                                  hop_xfer_task *source_task, uint64_t source_reserved_bytes) {
    uint64_t min_stride,total64;
    if (!bgra || w <= 0 || h <= 0 || stride <= 0) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    min_stride=(uint64_t)(uint32_t)w*4u;
    if ((uint64_t)(uint32_t)stride<min_stride ||
        (uint64_t)(uint32_t)stride>UINT64_MAX/(uint64_t)(uint32_t)h) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    total64=(uint64_t)(uint32_t)stride*(uint64_t)(uint32_t)h;
    if (source_task && (!owned || source_reserved_bytes != total64)) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    if(total64>(uint64_t)SIZE_MAX||!xfer_connected()){
        if(owned)free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }

    uint32_t batch;
    do { batch = InterlockedIncrement((LONG *)&s_x.batch_seq); } while (!batch);
    hop_xfer_task task;
    memset(&task,0,sizeof(task));
    if(!hop_xfer_task_init_classified(&task,0,0,batch,HOP_XFER_DIR_SEND,HOP_TASK_KIND_IMAGE)){
        if(owned)free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    int job_reserved = hop_xfer_task_reserve_job(&task);
    int image_reserved = job_reserved &&
        (source_task ?
         hop_xfer_task_transfer_image_temp(source_task, &task, total64) :
         hop_xfer_task_reserve_image(&task, total64));
    if (!image_reserved) {
        uint64_t task_id=task.id;
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        if(owned)free(owned);
        /* A successful transfer moves the source reservation; a failed one
         * leaves it untouched, so this is safe only on the failure path. */
        release_image_source_reservation(source_task, source_reserved_bytes);
        hop_log(HOP_LOG_WARN,"xfer: global image memory/queue budget exhausted (task=%llu)",
                (unsigned long long)task_id);
        return 0;
    }

    xfer_job *job=(xfer_job*)calloc(1,sizeof(*job));
    if(!job){
        if(owned)free(owned);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    size_t total=(size_t)total64;
    uint8_t *copy=owned ? owned : (uint8_t*)malloc(total);
    if(!copy){
        free(job);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    if(!owned)memcpy(copy,bgra,total);

    if(!hop_xfer_task_set_state(&task,HOP_XFER_TASK_QUEUED)||
       !hop_xfer_task_move(&job->task,&task)){
        free(copy);free(job);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    job->batch=batch;
    job->is_image=1;
    job->pixels=copy;
    job->w=w;job->h=h;job->stride=stride;

    hop_log(HOP_LOG_INFO, "xfer: queued image %dx%d batch=%u task=%llu (%.1f MB)",
            w, h, job->batch, (unsigned long long)job->task.id, (double)total / 1048576.0);

    if (!submit(job)) {
        hop_xfer_task_finish(&job->task,HOP_XFER_TASK_FAILED);
        job_free(job);
        return 0;
    }
    return 1;
}

void xfer_kick(void) {
    sender_context_request_reconnect(&s_sender_platform);
}
int xfer_listening(void) { return InterlockedCompareExchange(&s_x.listening, 0, 0) != 0; }

int xfer_submit_paths(const wchar_t *const *paths, int n) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, 0, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_paths_for_paste(const wchar_t *const *paths, int n, uint32_t request_id, uint64_t offer_id) {
    if (!(request_id & HOP_XFER_PASTE_BATCH_FLAG) || !hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, request_id, offer_id);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_image_impl(bgra, NULL, w, h, stride, NULL, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image_owned(uint8_t *bgra, int w, int h, int stride) {
    if (!bgra) return 0;
    if (!hop_update_work_enter()) { free(bgra); return 0; }
    int result = xfer_submit_image_impl(bgra, bgra, w, h, stride, NULL, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image_owned_from_task(uint8_t *bgra, int w, int h, int stride,
                                       hop_xfer_task *source_task, uint64_t reserved_bytes) {
    if (!bgra) {
        release_image_source_reservation(source_task, reserved_bytes);
        return 0;
    }
    if (!hop_update_work_enter()) {
        free(bgra);
        release_image_source_reservation(source_task, reserved_bytes);
        return 0;
    }
    int result = xfer_submit_image_impl(bgra, bgra, w, h, stride, source_task, reserved_bytes);
    hop_update_work_leave();
    return result;
}
