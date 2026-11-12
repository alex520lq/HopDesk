static void clip_send_paste_failed(uint32_t request_id,uint8_t reason) {
    uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
    uint32_t n=hop_clip_paste_failed_encode(p,request_id,reason);
    if(n)(void)net_server_send(g_app.net,HOP_MSG_CLIP_PASTE_FAILED,p,n);
}
typedef struct { wchar_t **paths; int count; uint32_t request_id; uint64_t offer_id; } clip_paste_work;

static void clip_free_paste_work(clip_paste_work *work) {
    for (int i = 0; i < work->count; ++i) free(work->paths[i]);
    free(work->paths);
    free(work);
}

static int clip_snapshot_paste_paths(void *context) {
    clip_paste_work *work = context;
    if (s_local_offer_count <= 0) return 0;
    work->paths = calloc((size_t)s_local_offer_count, sizeof(wchar_t *));
    if (!work->paths) return 0;
    for (int i = 0; i < s_local_offer_count; ++i) {
        wchar_t *path = _wcsdup(s_local_offer_paths[i]);
        if (!path) return 0;
        work->paths[work->count++] = path;
    }
    return 1;
}

static DWORD WINAPI clip_paste_worker(LPVOID arg) {
    clip_paste_work *w=(clip_paste_work*)arg;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    int ok=xfer_submit_paths_for_paste((const wchar_t *const *)w->paths,w->count,w->request_id,w->offer_id);
    if(!ok)clip_send_paste_failed(w->request_id,
        xfer_connected()?HOP_CLIP_PASTE_FAIL_SOURCE_IO:HOP_CLIP_PASTE_FAIL_TRANSFER);
    clip_free_paste_work(w);
    AcquireSRWLockExclusive(&s_offer_lock);hop_clipboard_txn_finish_paste_worker(&s_clip_txn);ReleaseSRWLockExclusive(&s_offer_lock);
    if(s_paste_workers_done)SetEvent(s_paste_workers_done);
    return 0;
}
void clip_on_link(int connected) {
    if(connected)clip_send_local_offer_snapshot();
    else clip_clear_remote_offer_local();
}
void clip_on_remote_file_offer(const uint8_t *p,uint32_t len) {
    hop_clip_file_offer_msg m;
    if(!hop_clip_file_offer_decode(p,len,&m))return;
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_remote_offer(&s_clip_txn,clip_local_device(),m.origin,m.generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)clip_drop_local_offer_paths();
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(out.notify_peer)clip_send_offer_clear(clip_local_device(),out.cleared_generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)
        hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clip_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_offer_clear(&s_clip_txn,clip_local_device(),origin,generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)clip_drop_local_offer_paths();
    ReleaseSRWLockExclusive(&s_offer_lock);
}
int clip_request_remote_paste(void) {
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    DWORD sequence=GetClipboardSequenceNumber();
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_begin_paste(&s_clip_txn,(uint64_t)sequence,(uint64_t)s_last_sequence,
        HOP_XFER_PASTE_BATCH_FLAG);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(out.action==HOP_CLIP_ACT_NONE)return 0;
    if(out.action==HOP_CLIP_ACT_RESEND_PENDING)return 1;
    int sent=hop_clip_paste_request_encode(p,out.request_id,out.origin,out.generation,out.expected_version) &&
             net_server_send(g_app.net,HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        AcquireSRWLockExclusive(&s_offer_lock);
        hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,out.request_id);
        ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",out.request_id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",out.request_id);
    return 1;
}
void clip_on_paste_failed(const uint8_t *p,uint32_t len) {
    uint32_t id=0;uint8_t reason=0;
    if(!hop_clip_paste_failed_decode(p,len,&id,&reason))return;
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,id);
    ReleaseSRWLockExclusive(&s_offer_lock);
    hop_log(HOP_LOG_WARN,"clip: remote paste request %u failed (reason %u)",id,(unsigned)reason);
}
void clip_on_paste_request(const uint8_t *p,uint32_t len) {
    hop_clip_paste_request_msg m;
    if(!hop_clip_paste_request_decode(p,len,&m))return;
    clip_paste_work *w=(clip_paste_work*)calloc(1,sizeof(*w));
    if(!w){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;}
    w->request_id = m.request_id;
    w->offer_id = m.generation;
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_paste_request(&s_clip_txn,m.request_id,m.origin,m.generation,
        clip_local_device(),s_paste_workers_done != NULL,clip_snapshot_paste_paths,w);
    if(out.action==HOP_CLIP_ACT_SPAWN_PASTE_WORKER)ResetEvent(s_paste_workers_done);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(out.action!=HOP_CLIP_ACT_SPAWN_PASTE_WORKER){
        clip_free_paste_work(w);
        if(out.action==HOP_CLIP_ACT_SEND_PASTE_FAILED)
            clip_send_paste_failed(out.request_id,out.fail_reason);
        return;
    }
    HANDLE t=CreateThread(NULL,0,clip_paste_worker,w,0,NULL);
    if(!t){
        clip_free_paste_work(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);
        AcquireSRWLockExclusive(&s_offer_lock);hop_clipboard_txn_finish_paste_worker(&s_clip_txn);ReleaseSRWLockExclusive(&s_offer_lock);
        SetEvent(s_paste_workers_done);return;
    }
    CloseHandle(t);
}
void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success) {
    uint64_t expected=0;
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_complete_paste(&s_clip_txn,batch,
        (uint64_t)GetClipboardSequenceNumber(),success);
    if(out.action==HOP_CLIP_ACT_CANCEL_PENDING)hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,0);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(out.action==HOP_CLIP_ACT_CANCEL_PENDING)return;
    if(out.action!=HOP_CLIP_ACT_APPLY_PASTE){
        if(!success)hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; clipboard unchanged",batch);
        else hop_log(HOP_LOG_WARN,"clip: paste %u finished after clipboard changed; files kept, clipboard not overwritten",batch);
        return;
    }
    expected=out.expected_version;
    HGLOBAL h=make_drop_global(paths,n);
    int posted=0;
    if(h){
        AcquireSRWLockExclusive(&s_offer_lock);
        if(!s_clip_stopping)posted=PostMessageW(g_app.hwnd,WM_APP_CLIP_PASTE,(WPARAM)batch,(LPARAM)h)!=0;
        ReleaseSRWLockExclusive(&s_offer_lock);
    }
    if(!posted){
        if(h)GlobalFree(h);
        AcquireSRWLockExclusive(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_ERROR,"clip: could not stage completed paste %u",batch);
    }
}
void clip_on_paste_message(HGLOBAL h,uint32_t request_id) {
    /* A matching foreground HWND does not prove the folder or focused control
     * stayed the same. Never inject a delayed paste into an unknown target.
     * The immediate and queued-write paths notify only after the clipboard
     * actually contains the files, so the user can choose where to paste. */
    uint64_t expected=0;
    AcquireSRWLockExclusive(&s_offer_lock);
    int pending=hop_clipboard_txn_get_pending_paste(&s_clip_txn,request_id,&expected);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(!pending){if(h)GlobalFree(h);return;}
    (void)apply_global_expected(CF_HDROP,h,(DWORD)expected,1,request_id);
}
