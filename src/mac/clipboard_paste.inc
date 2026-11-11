static void clip_send_paste_failed(uint32_t request_id,uint8_t reason) {
    uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
    uint32_t n=hop_clip_paste_failed_encode(p,request_id,reason);
    if(n)(void)net_send(HOP_MSG_CLIP_PASTE_FAILED,p,n);
}
void clipboard_on_link(int connected) {
    pthread_mutex_lock(&s_offer_lock);int stopping=s_clip_stopping;pthread_mutex_unlock(&s_offer_lock);
    if(stopping)return;
    if(connected)clip_send_local_offer_snapshot();
    else clip_clear_remote_offer_local();
}
void clipboard_on_remote_file_offer(const uint8_t *p,uint32_t len) {
    hop_clip_file_offer_msg m;
    if(!hop_clip_file_offer_decode(p,len,&m)||m.origin==clip_local_device())return;
    pthread_mutex_lock(&s_offer_lock);
    int stopping=s_clip_stopping;
    pthread_mutex_unlock(&s_offer_lock);
    if(stopping)return;
    clip_clear_local_offer(1);
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_set_remote_offer(&s_clip_txn,m.origin,m.generation);
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clipboard_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    pthread_mutex_lock(&s_offer_lock);
    int match=hop_clipboard_txn_clear_offer_if_match(&s_clip_txn,clip_local_device(),origin,generation);
    pthread_mutex_unlock(&s_offer_lock);
    if(match==1)clip_clear_local_offer(0);
}
int clipboard_request_remote_paste(void) {
    __block NSInteger current=0;
    void (^read_paste_context)(void)=^{current=[[NSPasteboard generalPasteboard] changeCount];};
    if([NSThread isMainThread])read_paste_context();
    else dispatch_sync(dispatch_get_main_queue(),read_paste_context);
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t id=0,origin=0;uint64_t generation=0,expected=0;
    pthread_mutex_lock(&s_offer_lock);
    int begin=hop_clipboard_txn_begin_paste(&s_clip_txn,(uint64_t)current,(uint64_t)s_last_change,
        HOP_XFER_PASTE_BATCH_FLAG,&id,&origin,&generation,&expected);
    pthread_mutex_unlock(&s_offer_lock);
    if(begin==0)return 0;
    if(begin==2)return 1;
    int sent=hop_clip_paste_request_encode(p,id,origin,generation,expected) &&
             net_send(HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        pthread_mutex_lock(&s_offer_lock);
        hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,id);
        pthread_mutex_unlock(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",id);
    return 1;
}
void clipboard_on_paste_failed(const uint8_t *p,uint32_t len) {
    uint32_t id=0;uint8_t reason=0;
    if(!hop_clip_paste_failed_decode(p,len,&id,&reason))return;
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,id);
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_WARN,"clip: remote paste request %u failed (reason %u)",id,(unsigned)reason);
}
static void clip_paste_worker(void *arg) {
    clip_paste_work *w=(clip_paste_work*)arg;
    int ok=xfer_submit_paths_for_paste((const char *const *)w->paths,w->count,w->request_id,w->offer_id);
    if(!ok)clip_send_paste_failed(w->request_id,
        xfer_connected()?HOP_CLIP_PASTE_FAIL_SOURCE_IO:HOP_CLIP_PASTE_FAIL_TRANSFER);
    for(int i=0;i<w->count;++i)free(w->paths[i]);
    free(w->paths);free(w);
}
void clipboard_on_paste_request(const uint8_t *p,uint32_t len) {
    hop_clip_paste_request_msg m;
    if(!hop_clip_paste_request_decode(p,len,&m))return;
    if(m.origin!=clip_local_device()){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;}
    clip_paste_work *w=(clip_paste_work*)calloc(1,sizeof(*w));
    if(!w){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;}
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_request_result decision=hop_clipboard_txn_check_request(&s_clip_txn,m.request_id,m.generation,s_paste_group!=nil);
    if(decision!=HOP_CLIPBOARD_REQUEST_ACCEPTED) {
        pthread_mutex_unlock(&s_offer_lock);free(w);
        if(decision!=HOP_CLIPBOARD_REQUEST_DUPLICATE)clip_send_paste_failed(m.request_id,decision==HOP_CLIPBOARD_REQUEST_STALE?HOP_CLIP_PASTE_FAIL_STALE:HOP_CLIP_PASTE_FAIL_BUSY);
        return;
    }
    w->count=s_local_offer_count;w->request_id=m.request_id;w->offer_id=m.generation;
    w->paths=(char**)calloc((size_t)w->count,sizeof(char*));
    if(w->paths)for(int i=0;i<w->count;++i)w->paths[i]=strdup(s_local_offer_paths[i]);
    int copy_ok=w->count>0&&w->paths!=NULL;
    if(copy_ok)for(int i=0;i<w->count;++i)if(!w->paths[i])copy_ok=0;
    if(!copy_ok){
        pthread_mutex_unlock(&s_offer_lock);
        if(w->paths){for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);}
        free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    decision=hop_clipboard_txn_accept_request(&s_clip_txn,m.request_id,m.generation,s_paste_group!=nil);
    if(decision!=HOP_CLIPBOARD_REQUEST_ACCEPTED) {
        pthread_mutex_unlock(&s_offer_lock);
        for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);free(w);
        if(decision!=HOP_CLIPBOARD_REQUEST_DUPLICATE)clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);
        return;
    }
    dispatch_group_enter(s_paste_group);
    pthread_mutex_unlock(&s_offer_lock);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        clip_paste_worker(w);
        pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_finish_paste_worker(&s_clip_txn);pthread_mutex_unlock(&s_offer_lock);
        dispatch_group_leave(s_paste_group);
    });
}
void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success) {
    uint64_t expected=0;
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping){hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,0);pthread_mutex_unlock(&s_offer_lock);return;}
    int pending=hop_clipboard_txn_get_pending_paste(&s_clip_txn,batch,&expected);
    int valid=pending&&hop_clipboard_txn_complete_paste(&s_clip_txn,batch,expected,success,NULL);
    pthread_mutex_unlock(&s_offer_lock);
    if(!valid){if(!success)hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; pasteboard unchanged",batch);return;}
    if(!paths||n<=0){pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);pthread_mutex_unlock(&s_offer_lock);return;}
    NSMutableArray<NSURL *> *urls=[NSMutableArray arrayWithCapacity:(NSUInteger)n];
    for(int i=0;i<n;++i){NSString *path=[NSString stringWithUTF8String:paths[i]];if(path)[urls addObject:[NSURL fileURLWithPath:path]];}
    if([urls count]==0){pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);pthread_mutex_unlock(&s_offer_lock);return;}
    NSArray *snapshot=[urls copy];
    dispatch_async(dispatch_get_main_queue(),^{apply_objects_retry(snapshot,@"paths-paste",0,(NSInteger)expected);});
}
