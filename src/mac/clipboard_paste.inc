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
    pthread_mutex_lock(&s_offer_lock);int stopping=s_clip_stopping;pthread_mutex_unlock(&s_offer_lock);
    if(stopping)return;
    clip_clear_local_offer(1);
    pthread_mutex_lock(&s_offer_lock);
    s_remote_offer_origin=m.origin;s_remote_offer_generation=m.generation;
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_INFO,"clip: remote file offer received (%u item(s)); no data requested",m.count);
}
void clipboard_on_remote_file_offer_clear(const uint8_t *p,uint32_t len) {
    uint32_t origin=0;uint64_t generation=0;
    if(!hop_clip_file_offer_clear_decode(p,len,&origin,&generation))return;
    int clear_local=0;
    pthread_mutex_lock(&s_offer_lock);
    if(origin==clip_local_device()&&s_local_offer_generation==generation)clear_local=1;
    else if(s_remote_offer_origin==origin&&s_remote_offer_generation==generation){
        s_remote_offer_origin=0;s_remote_offer_generation=0;
    }
    pthread_mutex_unlock(&s_offer_lock);
    if(clear_local)clip_clear_local_offer(0);
}
int clipboard_request_remote_paste(void) {
    __block NSInteger current=0;
    void (^read_paste_context)(void)=^{
        current=[[NSPasteboard generalPasteboard] changeCount];
    };
    if([NSThread isMainThread]) read_paste_context();
    else dispatch_sync(dispatch_get_main_queue(),read_paste_context);
    uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
    uint32_t id=0,origin=0;uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping||!s_remote_offer_origin||!s_remote_offer_generation||current!=s_last_change){
        pthread_mutex_unlock(&s_offer_lock);return 0;
    }
    if(s_pending_paste_request){pthread_mutex_unlock(&s_offer_lock);return 1;}
    if(++s_offer_request_seq==0||s_offer_request_seq>0x7fffffffu)s_offer_request_seq=1;
    id=HOP_XFER_PASTE_BATCH_FLAG|s_offer_request_seq;
    origin=s_remote_offer_origin;generation=s_remote_offer_generation;
    s_pending_paste_request=id;s_pending_paste_change=current;
    pthread_mutex_unlock(&s_offer_lock);
    int sent=hop_clip_paste_request_encode(p,id,origin,generation,(uint64_t)current) &&
             net_send(HOP_MSG_CLIP_PASTE_REQUEST,p,sizeof(p));
    if(!sent) {
        pthread_mutex_lock(&s_offer_lock);
        if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_change=0;}
        pthread_mutex_unlock(&s_offer_lock);
        hop_log(HOP_LOG_WARN,"clip: paste request %u could not be sent",id);
    } else hop_log(HOP_LOG_INFO,"clip: cross-device paste requested (%u)",id);
    return 1;
}
void clipboard_on_paste_failed(const uint8_t *p,uint32_t len) {
    uint32_t id=0;uint8_t reason=0;
    if(!hop_clip_paste_failed_decode(p,len,&id,&reason))return;
    pthread_mutex_lock(&s_offer_lock);
    if(s_pending_paste_request==id){s_pending_paste_request=0;s_pending_paste_change=0;}
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
    if(s_clip_stopping||s_paste_scan_active||!s_paste_group){
        pthread_mutex_unlock(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    for(unsigned i=0;i<32;++i)if(s_seen_paste_requests[i]==m.request_id){
        pthread_mutex_unlock(&s_offer_lock);free(w);return;
    }
    if(m.generation!=s_local_offer_generation||s_local_offer_count<=0){
        pthread_mutex_unlock(&s_offer_lock);free(w);
        clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_STALE);return;
    }
    w->count=s_local_offer_count;w->request_id=m.request_id;w->offer_id=m.generation;
    w->paths=(char**)calloc((size_t)w->count,sizeof(char*));
    if(w->paths)for(int i=0;i<w->count;++i)w->paths[i]=strdup(s_local_offer_paths[i]);
    int copy_ok=w->paths!=NULL;
    if(copy_ok)for(int i=0;i<w->count;++i)if(!w->paths[i])copy_ok=0;
    if(!copy_ok){
        pthread_mutex_unlock(&s_offer_lock);
        if(w->paths){for(int i=0;i<w->count;++i)free(w->paths[i]);free(w->paths);}
        free(w);clip_send_paste_failed(m.request_id,HOP_CLIP_PASTE_FAIL_BUSY);return;
    }
    s_seen_paste_requests[s_seen_paste_next++%32]=m.request_id;
    s_paste_scan_active=1;dispatch_group_enter(s_paste_group);
    pthread_mutex_unlock(&s_offer_lock);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        clip_paste_worker(w);
        pthread_mutex_lock(&s_offer_lock);s_paste_scan_active=0;pthread_mutex_unlock(&s_offer_lock);
        dispatch_group_leave(s_paste_group);
    });
}
void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success) {
    NSInteger expected;
    pthread_mutex_lock(&s_offer_lock);
    if(s_clip_stopping){s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    if(s_pending_paste_request!=batch){pthread_mutex_unlock(&s_offer_lock);return;}
    expected=s_pending_paste_change;
    if(!success){s_pending_paste_request=0;s_pending_paste_change=0;}
    pthread_mutex_unlock(&s_offer_lock);
    if(!success){hop_log(HOP_LOG_WARN,"clip: paste transfer %u incomplete; pasteboard unchanged",batch);return;}
    if(!paths||n<=0){pthread_mutex_lock(&s_offer_lock);s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    NSMutableArray<NSURL *> *urls=[NSMutableArray arrayWithCapacity:(NSUInteger)n];
    for(int i=0;i<n;++i){
        NSString *path=[NSString stringWithUTF8String:paths[i]];
        if(path)[urls addObject:[NSURL fileURLWithPath:path]];
    }
    if([urls count]==0){pthread_mutex_lock(&s_offer_lock);s_pending_paste_request=0;s_pending_paste_change=0;pthread_mutex_unlock(&s_offer_lock);return;}
    NSArray *snapshot=[urls copy];
    dispatch_async(dispatch_get_main_queue(),^{
        apply_objects_retry(snapshot,@"paths-paste",0,expected);
    });
}
