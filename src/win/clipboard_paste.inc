static void clip_send_paste_failed(uint32_t request_id,uint8_t reason) {
    uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
    uint32_t n=hop_clip_paste_failed_encode(p,request_id,reason);
    if(n)(void)net_server_send(g_app.net,HOP_MSG_CLIP_PASTE_FAILED,p,n);
}
typedef struct { wchar_t **paths; int count; uint32_t request_id; uint64_t offer_id; } clip_paste_work;
static DWORD WINAPI clip_paste_worker(LPVOID arg) {
    clip_paste_work *w=(clip_paste_work*)arg;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    int ok=xfer_submit_paths_for_paste((const wchar_t *const *)w->paths,w->count,w->request_id,w->offer_id);
    if(!ok)clip_send_paste_failed(w->request_id,
        xfer_connected()?HOP_CLIP_PASTE_FAIL_SOURCE_IO:HOP_CLIP_PASTE_FAIL_TRANSFER);
    for(int i=0;i<w->count;++i)free(w->paths[i]);
    free(w->paths);free(w);
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
    if(!hop_clip_file_offer_decode(p,len,&m)||m.origin==clip_local_device())return;
    AcquireSRWLockShared(&s_offer_lock);
    int stopping=s_clip_stopping;
    ReleaseSRWLockShared(&s_offer_lock);
    if(stopping)return;
    clip_clear_local_offer(1);
    AcquireSRWLockExclusive(&s_offer_lock);
    if(!s_clip_stopping)hop_clipboard_txn_set_remote_offer(&s_clip_txn,m.origin,m.generation);
    ReleaseSRWLockExclusive(&s_offer_lock);
    hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clip_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    AcquireSRWLockExclusive(&s_offer_lock);
    int match=hop_clipboard_txn_clear_offer_if_match(&s_clip_txn,clip_local_device(),origin,generation);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(match==1)clip_clear_local_offer(0);
}
int clip_request_remote_paste(void) {
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t id=0,origin=0;uint64_t generation=0,expected=0;
    DWORD sequence=GetClipboardSequenceNumber();
    AcquireSRWLockExclusive(&s_offer_lock);
    int begin=hop_clipboard_txn_begin_paste(&s_clip_txn,sequence,s_last_sequence,
        HOP_XFER_PASTE_BATCH_FLAG,&id,&origin,&generation,&expected);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(begin==0)return 0;
    if(begin==2)return 1;
    int sent=hop_clip_paste_request_encode(p,id,origin,generation,expected) &&
             net_server_send(g_app.net,HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        AcquireSRWLockExclusive(&s_offer_lock);
        hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,id);
        ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",id);
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
    if(m.origin!=clip_local_device()){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;}
    clip_paste_work *w=(clip_paste_work*)calloc(1,sizeof(*w));
    if(!w){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;}
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clipboard_request_result decision=hop_clipboard_txn_check_request(&s_clip_txn,m.request_id,m.generation,s_paste_workers_done!=NULL);
    if(decision!=HOP_CLIPBOARD_REQUEST_ACCEPTED) {
        ReleaseSRWLockExclusive(&s_offer_lock);free(w);
        if(decision!=HOP_CLIPBOARD_REQUEST_DUPLICATE)clip_send_paste_failed(m.request_id,decision==HOP_CLIPBOARD_REQUEST_STALE?HOP_CLIP_PASTE_FAIL_STALE:HOP_CLIP_PASTE_FAIL_BUSY);
        return;
    }
    w->count=s_local_offer_count;w->request_id=m.request_id;w->offer_id=m.generation;
    w->paths=(wchar_t**)calloc((size_t)w->count,sizeof(wchar_t*));
    if(w->paths)for(int i=0;i<w->count;++i)w->paths[i]=_wcsdup(s_local_offer_paths[i]);
    int copy_ok=w->count>0&&w->paths!=NULL;
    if(copy_ok)for(int i=0;i<w->count;++i)if(!w->paths[i])copy_ok=0;
    if(!copy_ok){
        ReleaseSRWLockExclusive(&s_offer_lock);
        if(w->paths){for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);}
        free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    decision=hop_clipboard_txn_accept_request(&s_clip_txn,m.request_id,m.generation,s_paste_workers_done!=NULL);
    if(decision!=HOP_CLIPBOARD_REQUEST_ACCEPTED) {
        ReleaseSRWLockExclusive(&s_offer_lock);
        for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);free(w);
        if(decision!=HOP_CLIPBOARD_REQUEST_DUPLICATE)clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);
        return;
    }
    ResetEvent(s_paste_workers_done);
    ReleaseSRWLockExclusive(&s_offer_lock);
    HANDLE t=CreateThread(NULL,0,clip_paste_worker,w,0,NULL);
    if(!t){
        for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);
        AcquireSRWLockExclusive(&s_offer_lock);hop_clipboard_txn_finish_paste_worker(&s_clip_txn);ReleaseSRWLockExclusive(&s_offer_lock);
        SetEvent(s_paste_workers_done);return;
    }
    CloseHandle(t);
}
void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success) {
    uint64_t expected=0;
    AcquireSRWLockExclusive(&s_offer_lock);
    if(s_clip_stopping){hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,0);ReleaseSRWLockExclusive(&s_offer_lock);return;}
    int valid=hop_clipboard_txn_complete_paste(&s_clip_txn,batch,(uint64_t)GetClipboardSequenceNumber(),success,&expected);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(!valid){
        if(!success)hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; clipboard unchanged",batch);
        else hop_log(HOP_LOG_WARN,"clip: paste %u finished after clipboard changed; files kept, clipboard not overwritten",batch);
        return;
    }
    HGLOBAL h=make_drop_global(paths,n);
    int posted=0;
    if(h){
        AcquireSRWLockExclusive(&s_offer_lock);
        if(!s_clip_stopping)posted=PostMessageW(g_app.hwnd,WM_APP_CLIP_PASTE,(WPARAM)expected,(LPARAM)h)!=0;
        ReleaseSRWLockExclusive(&s_offer_lock);
    }
    if(!posted){
        if(h)GlobalFree(h);
        AcquireSRWLockExclusive(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_ERROR,"clip: could not stage completed paste %u",batch);
    }
}
void clip_on_paste_message(HGLOBAL h,DWORD expected_sequence) {
    /* A matching foreground HWND does not prove the folder or focused control
     * stayed the same. Never inject a delayed paste into an unknown target.
     * The immediate and queued-write paths notify only after the clipboard
     * actually contains the files, so the user can choose where to paste. */
    (void)apply_global_expected(CF_HDROP,h,expected_sequence,1,1);
}
