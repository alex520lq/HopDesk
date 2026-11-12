static void clip_send_paste_failed(uint32_t request_id, uint8_t reason) {
  uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
  uint32_t n = hop_clip_paste_failed_encode(p, request_id, reason);
  if (n)
    (void)net_send(HOP_MSG_CLIP_PASTE_FAILED, p, n);
}
void clipboard_on_link(int connected) {
  char *copies[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
  int count = 0, valid = 1;
  uint64_t generation = 0;
  pthread_mutex_lock(&s_offer_lock);
  int replay = hop_clipboard_link_state(&s_clip_txn, connected,
                                        s_local_offer_count, &generation);
  if (replay) {
    count = s_local_offer_count;
    for (int i = 0; i < count; ++i)
      if (s_local_offer_paths[i])
        copies[i] = strdup(s_local_offer_paths[i]);
  }
  pthread_mutex_unlock(&s_offer_lock);
  if (!replay)
    return;
  for (int i = 0; i < count; ++i)
    if (!copies[i])
      valid = 0;
  if (valid && !clip_send_offer((const char *const *)copies, count, generation))
    hop_log(HOP_LOG_WARN, "clip: file offer resend failed");
  for (int i = 0; i < count; ++i)
    free(copies[i]);
}
void clipboard_on_remote_file_offer(const uint8_t *p, uint32_t len) {
  hop_clip_file_offer_msg m;
  if (!hop_clip_file_offer_decode(p, len, &m))
    return;
  uint64_t cleared = 0;
  pthread_mutex_lock(&s_offer_lock);
  int accepted = hop_clipboard_remote_offer(&s_clip_txn, clip_local_device(),
                                            m.origin, m.generation, &cleared);
  if (accepted)
    clip_drop_local_offer_paths();
  pthread_mutex_unlock(&s_offer_lock);
  if (cleared)
    clip_send_offer_clear(clip_local_device(), cleared);
  if (accepted)
    hop_log(HOP_LOG_INFO,
            "clip: remote file offer received (%u item(s)); no data requested",
            m.count);
}
void clipboard_on_remote_file_offer_clear(const uint8_t *p, uint32_t len) {
  uint32_t origin = 0;
  uint64_t generation = 0;
  if (!hop_clip_file_offer_clear_decode(p, len, &origin, &generation))
    return;
  pthread_mutex_lock(&s_offer_lock);
  if (hop_clipboard_offer_clear(&s_clip_txn, clip_local_device(), origin,
                                generation))
    clip_drop_local_offer_paths();
  pthread_mutex_unlock(&s_offer_lock);
}
int clipboard_request_remote_paste(void) {
  __block NSInteger current = 0;
  void (^read_paste_context)(void) = ^{
    current = [[NSPasteboard generalPasteboard] changeCount];
  };
  if ([NSThread isMainThread])
    read_paste_context();
  else
    dispatch_sync(dispatch_get_main_queue(), read_paste_context);
  uint32_t id = 0, origin = 0;
  uint64_t generation = 0, expected = 0;
  pthread_mutex_lock(&s_offer_lock);
  int result = hop_clipboard_begin_paste(&s_clip_txn, current, s_last_change,
                                         HOP_XFER_PASTE_BATCH_FLAG, &id,
                                         &origin, &generation, &expected);
  pthread_mutex_unlock(&s_offer_lock);
  if (!result)
    return 0;
  if (result == 2)
    return 1;
  uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
  int sent =
      hop_clip_paste_request_encode(p, id, origin, generation, expected) &&
      net_send(HOP_MSG_CLIP_PASTE_REQUEST, p, sizeof(p));
  if (!sent) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, id);
    pthread_mutex_unlock(&s_offer_lock);
    hop_log(HOP_LOG_WARN, "clip: paste request %u could not be sent", id);
  } else
    hop_log(HOP_LOG_INFO, "clip: cross-device paste requested (%u)", id);
  return 1;
}
void clipboard_on_paste_failed(const uint8_t *p, uint32_t len) {
  uint32_t id = 0;
  uint8_t reason = 0;
  if (!hop_clip_paste_failed_decode(p, len, &id, &reason))
    return;
  pthread_mutex_lock(&s_offer_lock);
  hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, id);
  pthread_mutex_unlock(&s_offer_lock);
  hop_log(HOP_LOG_WARN, "clip: remote paste request %u failed (reason %u)", id,
          (unsigned)reason);
}
static void clip_free_paste_work(clip_paste_work *work) {
  if (!work)
    return;
  for (int i = 0; i < work->count; ++i)
    free(work->paths[i]);
  free(work->paths);
  free(work);
}

static int clip_snapshot_paste_paths(void *context) {
  if (s_local_offer_count <= 0)
    return 0;
  clip_paste_work **slot = context;
  clip_paste_work *work = calloc(1, sizeof(*work));
  if (!work)
    return 0;
  *slot = work;
  work->paths = calloc((size_t)s_local_offer_count, sizeof(char *));
  if (!work->paths)
    return 0;
  for (int i = 0; i < s_local_offer_count; ++i) {
    char *path = strdup(s_local_offer_paths[i]);
    if (!path)
      return 0;
    work->paths[work->count++] = path;
  }
  return 1;
}

static void clip_paste_worker(void *arg) {
  clip_paste_work *w = (clip_paste_work *)arg;
  int ok = xfer_submit_paths_for_paste((const char *const *)w->paths, w->count,
                                       w->request_id, w->offer_id);
  if (!ok)
    clip_send_paste_failed(w->request_id, xfer_connected()
                                              ? HOP_CLIP_PASTE_FAIL_SOURCE_IO
                                              : HOP_CLIP_PASTE_FAIL_TRANSFER);
  clip_free_paste_work(w);
}
void clipboard_on_paste_request(const uint8_t *p, uint32_t len) {
  hop_clip_paste_request_msg m;
  if (!hop_clip_paste_request_decode(p, len, &m))
    return;
  clip_paste_work *w = NULL;
  pthread_mutex_lock(&s_offer_lock);
  hop_clipboard_request_result result = hop_clipboard_paste_request(
      &s_clip_txn, m.request_id, m.origin, m.generation, clip_local_device(),
      s_paste_group != nil, clip_snapshot_paste_paths, &w);
  if (result == HOP_CLIPBOARD_REQUEST_ACCEPTED)
    dispatch_group_enter(s_paste_group);
  pthread_mutex_unlock(&s_offer_lock);
  if (result != HOP_CLIPBOARD_REQUEST_ACCEPTED) {
    clip_free_paste_work(w);
    if (result != HOP_CLIPBOARD_REQUEST_DUPLICATE)
      clip_send_paste_failed(m.request_id, result == HOP_CLIPBOARD_REQUEST_STALE
                                               ? HOP_CLIP_PASTE_FAIL_STALE
                                               : HOP_CLIP_PASTE_FAIL_BUSY);
    return;
  }
  w->request_id = m.request_id;
  w->offer_id = m.generation;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    clip_paste_worker(w);
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_finish_paste_worker(&s_clip_txn);
    pthread_mutex_unlock(&s_offer_lock);
    dispatch_group_leave(s_paste_group);
  });
}
void clipboard_complete_remote_paste(uint32_t batch, const char *const *paths,
                                     int n, int success) {
  uint64_t expected = 0;
  pthread_mutex_lock(&s_offer_lock);
  /* Existing behavior supplies the transaction's armed version here, not a
   * freshly sampled NSPasteboard changeCount. Native version validation still
   * occurs in apply_objects_retry; this callback alone is not that proof. */
  (void)hop_clipboard_txn_get_pending_paste(&s_clip_txn, batch, &expected);
  int apply = hop_clipboard_complete_paste(&s_clip_txn, batch, expected,
                                           success, &expected);
  pthread_mutex_unlock(&s_offer_lock);
  if (!apply) {
    if (!success)
      hop_log(HOP_LOG_WARN,
              "clip: paste transfer %u incomplete; pasteboard unchanged",
              batch);
    return;
  }
  if (!paths || n <= 0) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, batch);
    pthread_mutex_unlock(&s_offer_lock);
    return;
  }
  NSMutableArray<NSURL *> *urls =
      [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (int i = 0; i < n; ++i) {
    NSString *path = [NSString stringWithUTF8String:paths[i]];
    if (path)
      [urls addObject:[NSURL fileURLWithPath:path]];
  }
  if ([urls count] == 0) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, batch);
    pthread_mutex_unlock(&s_offer_lock);
    return;
  }
  NSArray *snapshot = [urls copy];
  dispatch_async(dispatch_get_main_queue(), ^{
    apply_objects_retry(snapshot, @"paths-paste", 0, (NSInteger)expected,
                        batch);
  });
}
