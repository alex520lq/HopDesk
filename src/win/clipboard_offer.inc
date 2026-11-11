static uint32_t clip_local_device(void) {
    return g_app.cfg.role == HOP_ROLE_SERVER ? HOP_DEV_SERVER : HOP_DEV_CLIENT;
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation);
static void clip_clear_remote_offer_local(void) {
    AcquireSRWLockExclusive(&s_offer_lock);
    (void)hop_clipboard_txn_clear_remote_offer(&s_clip_txn);
    ReleaseSRWLockExclusive(&s_offer_lock);
}
static void clip_invalidate_remote_offer(int notify_peer) {
    hop_clipboard_offer_key old;
    AcquireSRWLockExclusive(&s_offer_lock);
    old=hop_clipboard_txn_clear_remote_offer(&s_clip_txn);
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(notify_peer&&old.origin&&old.generation)clip_send_offer_clear(old.origin,old.generation);
}
static void clip_send_offer_clear(uint32_t origin, uint64_t generation) {
    uint8_t p[HOP_CLIP_OFFER_CLEAR_BYTES];
    if (!origin || !generation) return;
    hop_put_be32(p, origin); hop_put_be64(p + 4, generation);
    (void)net_server_send(g_app.net, HOP_MSG_CLIP_FILE_OFFER_CLEAR, p, sizeof(p));
}
static void clip_clear_local_offer(int notify_peer) {
    uint64_t generation;
    AcquireSRWLockExclusive(&s_offer_lock);
    generation=hop_clipboard_txn_clear_local_offer(&s_clip_txn);
    for(int i=0;i<s_local_offer_count;++i)free(s_local_offer_paths[i]);
    memset(s_local_offer_paths,0,sizeof(s_local_offer_paths));
    s_local_offer_count=0;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(notify_peer&&generation)clip_send_offer_clear(clip_local_device(),generation);
}
static int clip_offer_names(const wchar_t *const *paths, int count, char **names) {
    if (!paths || !names || count <= 0 || count > (int)HOP_CLIP_OFFER_MAX_ITEMS) return 0;
    for (int i=0;i<count;++i) {
        const wchar_t *base = paths[i] ? paths[i] : L"";
        const wchar_t *a = wcsrchr(base,L'\\'), *b = wcsrchr(base,L'/');
        if (a && (!b || a>b)) base=a+1; else if (b) base=b+1;
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, base, -1, NULL, 0, NULL, NULL);
        if (bytes <= 1 || bytes > (int)HOP_NAME_LEN_MAX + 1) return 0;
        names[i] = (char*)malloc((size_t)bytes);
        if (!names[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, base, -1,
                                               names[i], bytes, NULL, NULL)) return 0;
    }
    return 1;
}
static int clip_send_offer(const wchar_t *const *paths, int count, uint32_t origin, uint64_t generation) {
    char *names[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
    uint8_t *payload = NULL; uint32_t len = 0; int ok = 0;
    if (!clip_offer_names(paths,count,names)) goto done;
    payload = (uint8_t*)malloc(HOP_CLIP_OFFER_MAX_PAYLOAD);
    if (!payload) goto done;
    len = hop_clip_file_offer_encode(payload,HOP_CLIP_OFFER_MAX_PAYLOAD,origin,generation,
                                     (const char *const *)names,count);
    if (len) ok = net_server_send(g_app.net,HOP_MSG_CLIP_FILE_OFFER,payload,len);
done:
    for(int i=0;i<count && i<(int)HOP_CLIP_OFFER_MAX_ITEMS;++i) free(names[i]);
    free(payload);
    return ok;
}
static int clip_publish_local_offer(const wchar_t *const *paths, int count) {
    wchar_t *copies[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
    if (!paths || count <= 0 || count > (int)HOP_CLIP_OFFER_MAX_ITEMS) return 0;
    char *names[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
    if (!clip_offer_names(paths,count,names)) {
        for(int i=0;i<count;++i) free(names[i]);
        clip_clear_remote_offer_local(); clip_clear_local_offer(1); return 0;
    }
    for(int i=0;i<count;++i) free(names[i]);
    for(int i=0;i<count;++i) {
        if(!paths[i] || !(copies[i]=_wcsdup(paths[i]))) {
            for(int j=0;j<count;++j) free(copies[j]);
            return 0;
        }
    }
    clip_invalidate_remote_offer(1);
    clip_clear_local_offer(1);
    AcquireSRWLockExclusive(&s_offer_lock);
    uint64_t generation = hop_clipboard_txn_publish_local_offer(&s_clip_txn);
    for(int i=0;i<count;++i) s_local_offer_paths[i]=copies[i];
    s_local_offer_count=count;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if (!clip_send_offer(paths,count,clip_local_device(),generation))
        hop_log(HOP_LOG_WARN,"clip: file offer stored locally; peer will be updated on reconnect");
    else
        hop_log(HOP_LOG_INFO,"clip: file offer published (%d top-level item(s), no file data sent)",count);
    return 1;
}
static void clip_send_local_offer_snapshot(void) {
    wchar_t *copies[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
    int count; uint64_t generation;
    AcquireSRWLockShared(&s_offer_lock);
    count=s_local_offer_count; generation=s_local_offer_generation;
    for(int i=0;i<count;++i) if(s_local_offer_paths[i]) copies[i]=_wcsdup(s_local_offer_paths[i]);
    ReleaseSRWLockShared(&s_offer_lock);
    int valid=count>0;
    for(int i=0;i<count;++i) if(!copies[i]) valid=0;
    if(valid && !clip_send_offer((const wchar_t *const *)copies,count,clip_local_device(),generation))
        hop_log(HOP_LOG_WARN,"clip: file offer resend failed");
    for(int i=0;i<count;++i) free(copies[i]);
}
