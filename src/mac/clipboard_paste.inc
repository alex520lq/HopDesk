static void clip_send_paste_failed(uint32_t request_id,uint8_t reason) {
    uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
    uint32_t n=hop_clip_paste_failed_encode(p,request_id,reason);
    if(n)(void)net_send(HOP_MSG_CLIP_PASTE_FAILED,p,n);
}
void clipboard_on_link(int connected) {
    char *copies[HOP_CLIP_OFFER_MAX_ITEMS]={0};
    int count=0,valid=1;
    uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_link_state(&s_clip_txn,connected,s_local_offer_count);
    if(out.action==HOP_CLIP_ACT_RESEND_LOCAL_OFFER){
        count=s_local_offer_count;
        generation=out.generation;
        for(int i=0;i<count;++i)
            if(s_local_offer_paths[i])copies[i]=strdup(s_local_offer_paths[i]);
    }
    pthread_mutex_unlock(&s_offer_lock);
    if(out.action!=HOP_CLIP_ACT_RESEND_LOCAL_OFFER)return;
    /* A partially duplicated snapshot would announce a shorter offer than the
     * clipboard really holds, so a single failure cancels the replay. */
    for(int i=0;i<count;++i)if(!copies[i])valid=0;
    if(valid&&!clip_send_offer((const char *const *)copies,count,generation))
        hop_log(HOP_LOG_WARN,"clip: file offer resend failed");
    for(int i=0;i<count;++i)free(copies[i]);
}
void clipboard_on_remote_file_offer(const uint8_t *p,uint32_t len) {
    hop_clip_file_offer_msg m;
    if(!hop_clip_file_offer_decode(p,len,&m))return;
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_remote_offer(&s_clip_txn,clip_local_device(),m.origin,m.generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)clip_drop_local_offer_paths();
    pthread_mutex_unlock(&s_offer_lock);
    if(out.notify_peer)clip_send_offer_clear(clip_local_device(),out.cleared_generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)
        hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clipboard_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_offer_clear(&s_clip_txn,clip_local_device(),origin,generation);
    if(out.action==HOP_CLIP_ACT_CLEAR_LOCAL_OFFER)clip_drop_local_offer_paths();
    pthread_mutex_unlock(&s_offer_lock);
}
int clipboard_request_remote_paste(void) {
    __block NSInteger current=0;
    void (^read_paste_context)(void)=^{current=[[NSPasteboard generalPasteboard] changeCount];};
    if([NSThread isMainThread])read_paste_context();
    else dispatch_sync(dispatch_get_main_queue(),read_paste_context);
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_begin_paste(&s_clip_txn,(uint64_t)current,(uint64_t)s_last_change,
        HOP_XFER_PASTE_BATCH_FLAG);
    pthread_mutex_unlock(&s_offer_lock);
    if(out.action==HOP_CLIP_ACT_NONE)return 0;
    if(out.action==HOP_CLIP_ACT_RESEND_PENDING)return 1;
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    int sent=hop_clip_paste_request_encode(p,out.request_id,out.origin,out.generation,out.expected_version) &&
             net_send(HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        pthread_mutex_lock(&s_offer_lock);
        hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,out.request_id);
        pthread_mutex_unlock(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",out.request_id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",out.request_id);
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
static void clip_free_paste_work(clip_paste_work *work) {
    for (int i = 0; i < work->count; ++i) free(work->paths[i]);
    free(work->paths);
    free(work);
}

static int clip_snapshot_paste_paths(void *context) {
    clip_paste_work *work = context;
    if (s_local_offer_count <= 0) return 0;
    work->paths = calloc((size_t)s_local_offer_count, sizeof(char *));
    if (!work->paths) return 0;
    for (int i = 0; i < s_local_offer_count; ++i) {
        char *path = strdup(s_local_offer_paths[i]);
        if (!path) return 0;
        work->paths[work->count++] = path;
    }
    return 1;
}

static void clip_paste_worker(void *arg) {
    clip_paste_work *w=(clip_paste_work*)arg;
    int ok=xfer_submit_paths_for_paste((const char *const *)w->paths,w->count,w->request_id,w->offer_id);
    if(!ok)clip_send_paste_failed(w->request_id,
        xfer_connected()?HOP_CLIP_PASTE_FAIL_SOURCE_IO:HOP_CLIP_PASTE_FAIL_TRANSFER);
    clip_free_paste_work(w);
}
void clipboard_on_paste_request(const uint8_t *p,uint32_t len) {
    hop_clip_paste_request_msg m;
    if(!hop_clip_paste_request_decode(p,len,&m))return;
    clip_paste_work *w=(clip_paste_work*)calloc(1,sizeof(*w));
    if(!w){clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;}
    w->request_id = m.request_id;
    w->offer_id = m.generation;
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_outcome out=hop_clip_coord_paste_request(&s_clip_txn,m.request_id,m.origin,m.generation,
        clip_local_device(),s_paste_group != nil,clip_snapshot_paste_paths,w);
    if(out.action==HOP_CLIP_ACT_SPAWN_PASTE_WORKER)dispatch_group_enter(s_paste_group);
    pthread_mutex_unlock(&s_offer_lock);
    if(out.action!=HOP_CLIP_ACT_SPAWN_PASTE_WORKER){
        clip_free_paste_work(w);
        if(out.action==HOP_CLIP_ACT_SEND_PASTE_FAILED)
            clip_send_paste_failed(out.request_id,out.fail_reason);
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        clip_paste_worker(w);
        pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_finish_paste_worker(&s_clip_txn);pthread_mutex_unlock(&s_offer_lock);
        dispatch_group_leave(s_paste_group);
    });
}
void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success) {
    uint64_t expected=0;
    pthread_mutex_lock(&s_offer_lock);
    (void)hop_clipboard_txn_get_pending_paste(&s_clip_txn,batch,&expected);
    hop_clip_outcome out=hop_clip_coord_complete_paste(&s_clip_txn,batch,expected,success);
    if(out.action==HOP_CLIP_ACT_CANCEL_PENDING)hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,0);
    pthread_mutex_unlock(&s_offer_lock);
    if(out.action==HOP_CLIP_ACT_CANCEL_PENDING)return;
    if(out.action!=HOP_CLIP_ACT_APPLY_PASTE){
        if(!success)hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; pasteboard unchanged",batch);
        return;
    }
    expected=out.expected_version;
    if(!paths||n<=0){pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);pthread_mutex_unlock(&s_offer_lock);return;}
    NSMutableArray<NSURL *> *urls=[NSMutableArray arrayWithCapacity:(NSUInteger)n];
    for(int i=0;i<n;++i){NSString *path=[NSString stringWithUTF8String:paths[i]];if(path)[urls addObject:[NSURL fileURLWithPath:path]];}
    if([urls count]==0){pthread_mutex_lock(&s_offer_lock);hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,batch);pthread_mutex_unlock(&s_offer_lock);return;}
    NSArray *snapshot=[urls copy];
    dispatch_async(dispatch_get_main_queue(),^{apply_objects_retry(snapshot,@"paths-paste",0,(NSInteger)expected,batch);});
}
