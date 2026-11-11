static uint32_t clip_local_device(void) {
    return g_mac.cfg.role == HOP_ROLE_SERVER ? HOP_DEV_SERVER : HOP_DEV_CLIENT;
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation);
static void clip_clear_remote_offer_local(void) {
    pthread_mutex_lock(&s_offer_lock);
    s_remote_offer_origin=0; s_remote_offer_generation=0;
    s_pending_paste_request=0; s_pending_paste_change=0;
    pthread_mutex_unlock(&s_offer_lock);
}
static void clip_invalidate_remote_offer(int notify_peer) {
    uint32_t origin=0;uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    origin=s_remote_offer_origin;generation=s_remote_offer_generation;
    s_remote_offer_origin=0;s_remote_offer_generation=0;
    s_pending_paste_request=0;s_pending_paste_change=0;
    pthread_mutex_unlock(&s_offer_lock);
    if(notify_peer&&origin&&generation)clip_send_offer_clear(origin,generation);
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation) {
    uint8_t p[HOP_CLIP_OFFER_CLEAR_BYTES];
    if(!origin||!generation)return;
    hop_put_be32(p,origin);hop_put_be64(p+4,generation);
    (void)net_send(HOP_MSG_CLIP_FILE_OFFER_CLEAR,p,sizeof(p));
}
static void clip_clear_local_offer(int notify_peer) {
    uint64_t generation;
    pthread_mutex_lock(&s_offer_lock);
    generation=s_local_offer_generation;
    for(int i=0;i<s_local_offer_count;++i)free(s_local_offer_paths[i]);
    memset(s_local_offer_paths,0,sizeof(s_local_offer_paths));
    s_local_offer_count=0;s_local_offer_generation=0;
    pthread_mutex_unlock(&s_offer_lock);
    if(notify_peer&&generation)clip_send_offer_clear(clip_local_device(),generation);
}
static int clip_send_offer(const char *const *paths,int count,uint64_t generation) {
    char *names[HOP_CLIP_OFFER_MAX_ITEMS]={0};
    uint8_t *payload=NULL;uint32_t len=0;int ok=0;
    if(!paths||count<=0||count>(int)HOP_CLIP_OFFER_MAX_ITEMS)return 0;
    for(int i=0;i<count;++i){
        const char *base=strrchr(paths[i],'/');base=base?base+1:paths[i];
        size_t n=strlen(base);
        if(!n||n>HOP_NAME_LEN_MAX)goto done;
        names[i]=strdup(base);if(!names[i])goto done;
    }
    payload=(uint8_t*)malloc(HOP_CLIP_OFFER_MAX_PAYLOAD);if(!payload)goto done;
    len=hop_clip_file_offer_encode(payload,HOP_CLIP_OFFER_MAX_PAYLOAD,
        clip_local_device(),generation,(const char *const *)names,count);
    if(len)ok=net_send(HOP_MSG_CLIP_FILE_OFFER,payload,len);
done:
    for(int i=0;i<count;++i)free(names[i]);free(payload);return ok;
}
static int clip_publish_local_paths(NSArray<NSURL *> *urls) {
    NSUInteger n=[urls count];
    if(!n||n>HOP_CLIP_OFFER_MAX_ITEMS)return 0;
    char *copies[HOP_CLIP_OFFER_MAX_ITEMS]={0};
    for(NSUInteger i=0;i<n;++i){
        const char *path=[[urls objectAtIndex:i] fileSystemRepresentation];
        if(!path||!*path||!(copies[i]=strdup(path))){
            for(NSUInteger j=0;j<n;++j)free(copies[j]);return 0;
        }
    }
    clip_invalidate_remote_offer(1);clip_clear_local_offer(1);
    pthread_mutex_lock(&s_offer_lock);
    uint64_t generation=++s_local_offer_sequence;
    if(!generation)generation=++s_local_offer_sequence;
    s_local_offer_generation=generation;
    for(NSUInteger i=0;i<n;++i)s_local_offer_paths[i]=copies[i];
    s_local_offer_count=(int)n;
    pthread_mutex_unlock(&s_offer_lock);
    if(!clip_send_offer((const char *const *)copies,(int)n,generation))
        hop_log(HOP_LOG_WARN,"clip: file offer stored; peer will be updated on reconnect");
    else hop_log(HOP_LOG_INFO,"clip: file offer published (%lu top-level item(s), no file data sent)",(unsigned long)n);
    return 1;
}
static void clip_send_local_offer_snapshot(void) {
    char *copies[HOP_CLIP_OFFER_MAX_ITEMS]={0};int count;uint64_t generation;
    pthread_mutex_lock(&s_offer_lock);
    count=s_local_offer_count;generation=s_local_offer_generation;
    for(int i=0;i<count;++i)if(s_local_offer_paths[i])copies[i]=strdup(s_local_offer_paths[i]);
    pthread_mutex_unlock(&s_offer_lock);
    int valid=count>0;for(int i=0;i<count;++i)if(!copies[i])valid=0;
    if(valid&&!clip_send_offer((const char *const *)copies,count,generation))
        hop_log(HOP_LOG_WARN,"clip: file offer resend failed");
    for(int i=0;i<count;++i)free(copies[i]);
}

/* ---- 发送 ---- */

static BOOL send_paths(NSArray<NSURL *> *urls) {
    /* Only store local paths and publish a lightweight Offer. */
    return clip_publish_local_paths(urls) ? YES : NO;
}

static uint32_t s_image_work_seq;
