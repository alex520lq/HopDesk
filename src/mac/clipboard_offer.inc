static uint32_t clip_local_device(void) {
    return g_mac.cfg.role == HOP_ROLE_SERVER ? HOP_DEV_SERVER : HOP_DEV_CLIENT;
}
/* Link down: the peer's offer is unreachable, but our own offer survives so it
 * can be re-announced on reconnect. */
static void clip_clear_remote_offer_local(void) {
    pthread_mutex_lock(&s_offer_lock);
    (void)hop_clipboard_txn_clear_remote_offer(&s_clip_txn);
    pthread_mutex_unlock(&s_offer_lock);
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation) {
    uint8_t p[HOP_CLIP_OFFER_CLEAR_BYTES];
    if(!origin||!generation)return;
    hop_put_be32(p,origin);hop_put_be64(p+4,generation);
    (void)net_send(HOP_MSG_CLIP_FILE_OFFER_CLEAR,p,sizeof(p));
}
/* Caller holds s_offer_lock. */
static void clip_drop_local_offer_paths(void) {
    for(int i=0;i<s_local_offer_count;++i)free(s_local_offer_paths[i]);
    memset(s_local_offer_paths,0,sizeof(s_local_offer_paths));
    s_local_offer_count=0;
}
/* Wire sends for a retire. Caller must NOT hold s_offer_lock. */
static void clip_announce_retired(hop_clip_retired_offers retired) {
    if(retired.notify==HOP_CLIP_NOTIFY_BOTH&&retired.remote.origin&&retired.remote.generation)
        clip_send_offer_clear(retired.remote.origin,retired.remote.generation);
    if(retired.notify!=HOP_CLIP_NOTIFY_NONE&&retired.local_generation)
        clip_send_offer_clear(clip_local_device(),retired.local_generation);
}
/* Clipboard changed under us: both identities are dead, the peer drops both.
 * Caller must NOT hold s_offer_lock. */
static void clip_retire_offers(void) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_retired_offers retired=hop_clip_coord_retire_offers(&s_clip_txn);
    clip_drop_local_offer_paths();
    pthread_mutex_unlock(&s_offer_lock);
    clip_announce_retired(retired);
}
/* We are writing the clipboard ourselves: both identities are retired, but only
 * our local generation is announced because the peer still owns its offer.
 * Caller must NOT hold s_offer_lock. */
static void clip_consume_offers(void) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_retired_offers retired=hop_clip_coord_consume_offers(&s_clip_txn);
    clip_drop_local_offer_paths();
    pthread_mutex_unlock(&s_offer_lock);
    clip_announce_retired(retired);
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
            for(NSUInteger j=0;j<n;++j)free(copies[j]);
            /* The clipboard now holds items we cannot describe, so the previous
             * offer is stale on both sides. */
            clip_consume_offers();
            return 0;
        }
    }
    uint64_t generation=0;
    pthread_mutex_lock(&s_offer_lock);
    hop_clip_retired_offers retired=hop_clip_coord_publish_local_offer(&s_clip_txn,&generation);
    clip_drop_local_offer_paths();
    for(NSUInteger i=0;i<n;++i)s_local_offer_paths[i]=copies[i];
    s_local_offer_count=(int)n;
    pthread_mutex_unlock(&s_offer_lock);
    clip_announce_retired(retired);
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
