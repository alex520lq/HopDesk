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
    AcquireSRWLockExclusive(&s_offer_lock);s_paste_scan_active=0;ReleaseSRWLockExclusive(&s_offer_lock);
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
    clip_clear_local_offer(1);
    AcquireSRWLockExclusive(&s_offer_lock);
    s_remote_offer_origin=m.origin;s_remote_offer_generation=m.generation;
    ReleaseSRWLockExclusive(&s_offer_lock);
    hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clip_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    int clear_local=0;
    AcquireSRWLockExclusive(&s_offer_lock);
    if(origin==clip_local_device()&&s_local_offer_generation==generation)clear_local=1;
    else if(s_remote_offer_origin==origin&&s_remote_offer_generation==generation){
        s_remote_offer_origin=0;s_remote_offer_generation=0;
    }
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(clear_local)clip_clear_local_offer(0);
}
int clip_request_remote_paste(void) {
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t id=0,origin=0;uint64_t generation=0;
    DWORD sequence=GetClipboardSequenceNumber();
    AcquireSRWLockExclusive(&s_offer_lock);
    if(!s_remote_offer_origin||!s_remote_offer_generation||sequence!=s_last_sequence){
        ReleaseSRWLockExclusive(&s_offer_lock);return 0;
    }
    if(s_pending_paste_request){ReleaseSRWLockExclusive(&s_offer_lock);return 1;}
    if(++s_offer_request_seq==0||s_offer_request_seq>0x7fffffffu)s_offer_request_seq=1;
    id=HOP_XFER_PASTE_BATCH_FLAG|s_offer_request_seq;
    origin=s_remote_offer_origin;generation=s_remote_offer_generation;
    s_pending_paste_request=id;s_pending_paste_sequence=sequence;
    ReleaseSRWLockExclusive(&s_offer_lock);
    int sent=hop_clip_paste_request_encode(p,id,origin,generation,sequence) &&
             net_server_send(g_app.net,HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        AcquireSRWLockExclusive(&s_offer_lock);
        if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_sequence=0;}
        ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",id);
    return 1;
}
void clip_on_paste_failed(const uint8_t *p,uint32_t len) {
    uint32_t id=0;uint8_t reason=0;
    if(!hop_clip_paste_failed_decode(p,len,&id,&reason))return;
    AcquireSRWLockExclusive(&s_offer_lock);
    if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_sequence=0;}
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
    if(s_clip_stopping || s_paste_scan_active || !s_paste_workers_done) {
        ReleaseSRWLockExclusive(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    for(unsigned i=0;i<32;++i)if(s_seen_paste_requests[i]==m.request_id){
        ReleaseSRWLockExclusive(&s_offer_lock);free(w);return;
    }
    if(m.generation!=s_local_offer_generation||s_local_offer_count<=0){
        ReleaseSRWLockExclusive(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;
    }
    w->count=s_local_offer_count;w->request_id=m.request_id;w->offer_id=m.generation;
    w->paths=(wchar_t**)calloc((size_t)w->count,sizeof(wchar_t*));
    if(w->paths)for(int i=0;i<w->count;++i)w->paths[i]=_wcsdup(s_local_offer_paths[i]);
    int copy_ok=w->paths!=NULL;
    if(copy_ok)for(int i=0;i<w->count;++i)if(!w->paths[i])copy_ok=0;
    if(!copy_ok){
        ReleaseSRWLockExclusive(&s_offer_lock);
        if(w->paths){for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);}
        free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    s_seen_paste_requests[s_seen_paste_next++%32]=m.request_id;
    s_paste_scan_active=1;ResetEvent(s_paste_workers_done);
    ReleaseSRWLockExclusive(&s_offer_lock);
    HANDLE t=CreateThread(NULL,0,clip_paste_worker,w,0,NULL);
    if(!t){
        for(int i=0;i<w->count;++i)free(w->paths[i]);
        free(w->paths);free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);
        AcquireSRWLockExclusive(&s_offer_lock);s_paste_scan_active=0;ReleaseSRWLockExclusive(&s_offer_lock);
        SetEvent(s_paste_workers_done);return;
    }
    CloseHandle(t);
}
void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success) {
    DWORD expected;
    AcquireSRWLockExclusive(&s_offer_lock);
    if(s_clip_stopping){s_pending_paste_request=0;s_pending_paste_sequence=0;ReleaseSRWLockExclusive(&s_offer_lock);return;}
    if(s_pending_paste_request!=batch){ReleaseSRWLockExclusive(&s_offer_lock);return;}
    expected=s_pending_paste_sequence;
    if(!success){s_pending_paste_request=0;s_pending_paste_sequence=0;}
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(!success){hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; clipboard unchanged",batch);return;}
    if(GetClipboardSequenceNumber()!=expected){
        AcquireSRWLockExclusive(&s_offer_lock);
        if(s_pending_paste_request==batch){s_pending_paste_request=0;s_pending_paste_sequence=0;}
        ReleaseSRWLockExclusive(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste %u finished after clipboard changed; files kept, clipboard not overwritten",batch);
        return;
    }
    HGLOBAL h=make_drop_global(paths,n);
    int posted=0;
    if(h){
        /* Serialize PostMessage with clip_shutdown: once stopping is visible,
         * no late HGLOBAL may be stranded after the message queue is drained. */
        AcquireSRWLockExclusive(&s_offer_lock);
        if(!s_clip_stopping)
            posted=PostMessageW(g_app.hwnd,WM_APP_CLIP_PASTE,(WPARAM)expected,(LPARAM)h)!=0;
        ReleaseSRWLockExclusive(&s_offer_lock);
    }
    if(!posted){
        if(h)GlobalFree(h);
        AcquireSRWLockExclusive(&s_offer_lock);
        if(s_pending_paste_request==batch){s_pending_paste_request=0;s_pending_paste_sequence=0;}
        ReleaseSRWLockExclusive(&s_offer_lock);
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
