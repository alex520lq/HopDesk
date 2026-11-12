static void clip_send_paste_failed(uint32_t request_id, uint8_t reason) {
  uint8_t p[HOP_CLIP_PASTE_FAILED_BYTES];
  uint32_t n = hop_clip_paste_failed_encode(p, request_id, reason);
  if (n)
    (void)net_server_send(g_app.net, HOP_MSG_CLIP_PASTE_FAILED, p, n);
}
typedef struct {
  wchar_t **paths;
  int count;
  uint32_t request_id;
  uint64_t offer_id;
} clip_paste_work;

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
  work->paths = calloc((size_t)s_local_offer_count, sizeof(wchar_t *));
  if (!work->paths)
    return 0;
  for (int i = 0; i < s_local_offer_count; ++i) {
    wchar_t *path = _wcsdup(s_local_offer_paths[i]);
    if (!path)
      return 0;
    work->paths[work->count++] = path;
  }
  return 1;
}

static void clip_finish_paste_worker(void) {
  AcquireSRWLockExclusive(&s_offer_lock);
  hop_clipboard_txn_finish_paste_worker(&s_clip_txn);
  /* Admission resets this event under the same lock. */
  SetEvent(s_paste_workers_done);
  ReleaseSRWLockExclusive(&s_offer_lock);
}

static DWORD WINAPI clip_paste_worker(LPVOID arg) {
  clip_paste_work *w = (clip_paste_work *)arg;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  int ok = xfer_submit_paths_for_paste((const wchar_t *const *)w->paths,
                                       w->count, w->request_id, w->offer_id);
  if (!ok)
    clip_send_paste_failed(w->request_id, xfer_connected()
                                              ? HOP_CLIP_PASTE_FAIL_SOURCE_IO
                                              : HOP_CLIP_PASTE_FAIL_TRANSFER);
  clip_free_paste_work(w);
  clip_finish_paste_worker();
  return 0;
}
void clip_on_link(int connected) {
  wchar_t *copies[HOP_CLIP_OFFER_MAX_ITEMS] = {0};
  int count = 0, valid = 1;
  uint64_t generation = 0;
  AcquireSRWLockExclusive(&s_offer_lock);
  int replay = hop_clipboard_link_state(&s_clip_txn, connected,
                                        s_local_offer_count, &generation);
  if (replay) {
    count = s_local_offer_count;
    for (int i = 0; i < count; ++i)
      if (s_local_offer_paths[i])
        copies[i] = _wcsdup(s_local_offer_paths[i]);
  }
  ReleaseSRWLockExclusive(&s_offer_lock);
  if (!replay)
    return;
  for (int i = 0; i < count; ++i)
    if (!copies[i])
      valid = 0;
  if (valid && !clip_send_offer((const wchar_t *const *)copies, count,
                                clip_local_device(), generation))
    hop_log(HOP_LOG_WARN, "clip: file offer resend failed");
  for (int i = 0; i < count; ++i)
    free(copies[i]);
}
void clip_on_remote_file_offer(const uint8_t *p, uint32_t len) {
  hop_clip_file_offer_msg m;
  if (!hop_clip_file_offer_decode(p, len, &m))
    return;
  uint64_t cleared = 0;
  AcquireSRWLockExclusive(&s_offer_lock);
  int accepted = hop_clipboard_remote_offer(&s_clip_txn, clip_local_device(),
                                            m.origin, m.generation, &cleared);
  if (accepted)
    clip_drop_local_offer_paths();
  ReleaseSRWLockExclusive(&s_offer_lock);
  if (cleared)
    clip_send_offer_clear(clip_local_device(), cleared);
  if (accepted)
    hop_log(HOP_LOG_INFO,
            "clip: remote file offer received (%u item(s)); no data requested",
            m.count);
}
void clip_on_remote_file_offer_clear(const uint8_t *p, uint32_t len) {
  uint32_t origin = 0;
  uint64_t generation = 0;
  if (!hop_clip_file_offer_clear_decode(p, len, &origin, &generation))
    return;
  AcquireSRWLockExclusive(&s_offer_lock);
  if (hop_clipboard_offer_clear(&s_clip_txn, clip_local_device(), origin,
                                generation))
    clip_drop_local_offer_paths();
  ReleaseSRWLockExclusive(&s_offer_lock);
}
int clip_request_remote_paste(void) {
  uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
  DWORD sequence = GetClipboardSequenceNumber();
  uint32_t id = 0, origin = 0;
  uint64_t generation = 0, expected = 0;
  AcquireSRWLockExclusive(&s_offer_lock);
  int result = hop_clipboard_begin_paste(&s_clip_txn, sequence, s_last_sequence,
                                         HOP_XFER_PASTE_BATCH_FLAG, &id,
                                         &origin, &generation, &expected);
  ReleaseSRWLockExclusive(&s_offer_lock);
  if (!result)
    return 0;
  if (result == 2)
    return 1;
  int sent =
      hop_clip_paste_request_encode(p, id, origin, generation, expected) &&
      net_server_send(g_app.net, HOP_MSG_CLIP_PASTE_REQUEST, p, sizeof(p));
  if (!sent) {
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, id);
    ReleaseSRWLockExclusive(&s_offer_lock);
    hop_log(HOP_LOG_WARN, "clip: paste request %u could not be sent", id);
  } else
    hop_log(HOP_LOG_INFO, "clip: cross-device paste requested (%u)", id);
  return 1;
}
void clip_on_paste_failed(const uint8_t *p, uint32_t len) {
  uint32_t id = 0;
  uint8_t reason = 0;
  if (!hop_clip_paste_failed_decode(p, len, &id, &reason))
    return;
  AcquireSRWLockExclusive(&s_offer_lock);
  hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, id);
  ReleaseSRWLockExclusive(&s_offer_lock);
  hop_log(HOP_LOG_WARN, "clip: remote paste request %u failed (reason %u)", id,
          (unsigned)reason);
}
void clip_on_paste_request(const uint8_t *p, uint32_t len) {
  hop_clip_paste_request_msg m;
  if (!hop_clip_paste_request_decode(p, len, &m))
    return;
  clip_paste_work *w = NULL;
  AcquireSRWLockExclusive(&s_offer_lock);
  hop_clipboard_request_result result = hop_clipboard_paste_request(
      &s_clip_txn, m.request_id, m.origin, m.generation, clip_local_device(),
      s_paste_workers_done != NULL, clip_snapshot_paste_paths, &w);
  if (result == HOP_CLIPBOARD_REQUEST_ACCEPTED)
    ResetEvent(s_paste_workers_done);
  ReleaseSRWLockExclusive(&s_offer_lock);
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
  HANDLE t = CreateThread(NULL, 0, clip_paste_worker, w, 0, NULL);
  if (!t) {
    clip_free_paste_work(w);
    clip_send_paste_failed(m.request_id, HOP_CLIP_PASTE_FAIL_BUSY);
    clip_finish_paste_worker();
    return;
  }
  CloseHandle(t);
}
void clip_complete_remote_paste(uint32_t batch, const wchar_t *const *paths,
                                int n, int success) {
  uint64_t expected = 0;
  AcquireSRWLockExclusive(&s_offer_lock);
  int apply = hop_clipboard_complete_paste(
      &s_clip_txn, batch, GetClipboardSequenceNumber(), success, &expected);
  ReleaseSRWLockExclusive(&s_offer_lock);
  if (!apply) {
    if (!success)
      hop_log(HOP_LOG_WARN,
              "clip: paste transfer %u incomplete; clipboard unchanged", batch);
    else
      hop_log(HOP_LOG_WARN,
              "clip: paste %u finished after clipboard changed; files kept, "
              "clipboard not overwritten",
              batch);
    return;
  }
  HGLOBAL h = make_drop_global(paths, n);
  int posted = 0;
  if (h) {
    AcquireSRWLockExclusive(&s_offer_lock);
    if (!s_clip_stopping)
      posted = PostMessageW(g_app.hwnd, WM_APP_CLIP_PASTE, (WPARAM)batch,
                            (LPARAM)h) != 0;
    ReleaseSRWLockExclusive(&s_offer_lock);
  }
  if (!posted) {
    if (h)
      GlobalFree(h);
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, batch);
    ReleaseSRWLockExclusive(&s_offer_lock);
    hop_log(HOP_LOG_ERROR, "clip: could not stage completed paste %u", batch);
  }
}
void clip_on_paste_message(HGLOBAL h, uint32_t request_id) {
  /* A matching foreground HWND does not prove the folder or focused control
   * stayed the same. Never inject a delayed paste into an unknown target.
   * The immediate and queued-write paths notify only after the clipboard
   * actually contains the files, so the user can choose where to paste. */
  uint64_t expected = 0;
  AcquireSRWLockExclusive(&s_offer_lock);
  int pending =
      hop_clipboard_txn_get_pending_paste(&s_clip_txn, request_id, &expected);
  ReleaseSRWLockExclusive(&s_offer_lock);
  if (!pending) {
    if (h)
      GlobalFree(h);
    return;
  }
  (void)apply_global_expected(CF_HDROP, h, (DWORD)expected, 1, request_id);
}
