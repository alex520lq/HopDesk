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
  if (!connected) {
    s_direct_paste_id = 0;
    s_direct_paste_path[0] = '\0';
  }
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
static id clip_ax_value(AXUIElementRef element, CFStringRef attribute) {
  CFTypeRef value = NULL;
  AXUIElementCopyAttributeValue(element, attribute, &value);
  return CFBridgingRelease(value);
}

static BOOL clip_ax_is_element(id value) {
  return value && CFGetTypeID((__bridge CFTypeRef)value) == AXUIElementGetTypeID();
}

/* Finder exposes the desktop's scroll area as a direct child of its app,
 * separately from normal windows. Start at the actual keyboard focus. */
static id clip_finder_paste_surface(AXUIElementRef finder) {
  id node = clip_ax_value(finder, kAXFocusedUIElementAttribute);
  for (unsigned depth = 0; depth < 16 && clip_ax_is_element(node); ++depth) {
    AXUIElementRef element = (__bridge AXUIElementRef)node;
    id role = clip_ax_value(element, kAXRoleAttribute);
    if ([role isEqual:(__bridge NSString *)kAXWindowRole]) {
      id subrole = clip_ax_value(element, kAXSubroleAttribute);
      return [subrole isEqual:(__bridge NSString *)kAXStandardWindowSubrole] ? node : nil;
    }
    id parent = clip_ax_value(element, kAXParentAttribute);
    if (clip_ax_is_element(parent) && CFEqual((__bridge CFTypeRef)parent, finder))
      return [role isEqual:(__bridge NSString *)kAXScrollAreaRole] ? node : nil;
    node = parent;
  }
  return nil;
}

static NSString *clip_focused_finder_path(AXUIElementRef finder) {
  id surface = clip_finder_paste_surface(finder);
  if (!surface) return nil;

  AXUIElementRef element = (__bridge AXUIElementRef)surface;
  id role = clip_ax_value(element, kAXRoleAttribute);
  if ([role isEqual:(__bridge NSString *)kAXScrollAreaRole])
    return NSSearchPathForDirectoriesInDomains(NSDesktopDirectory, NSUserDomainMask, YES).firstObject;

  if (!atomic_load(&s_finder_automation_granted)) return nil;
  NSAppleScript *script = [[NSAppleScript alloc] initWithSource:
      @"tell application \"Finder\" to get POSIX path of (insertion location as alias)"];
  NSString *path = [[script executeAndReturnError:NULL] stringValue];
  id current = clip_finder_paste_surface(finder);
  return current && CFEqual((__bridge CFTypeRef)surface, (__bridge CFTypeRef)current) ? path : nil;
}

static NSString *clip_finder_destination(void) {
  NSRunningApplication *front = NSWorkspace.sharedWorkspace.frontmostApplication;
  if (![front.bundleIdentifier isEqualToString:@"com.apple.finder"]) return nil;

  AXUIElementRef finder = AXUIElementCreateApplication(front.processIdentifier);
  if (!finder) return nil;
  NSString *path = clip_focused_finder_path(finder);
  CFRelease(finder);
  if (NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != front.processIdentifier)
    return nil;

  BOOL directory = NO;
  if (!path || ![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] ||
      !directory || access(path.fileSystemRepresentation, W_OK) != 0) return nil;
  return path;
}

int clipboard_direct_paste_target(uint32_t batch, char *out, size_t capacity) {
  if (!out || !capacity) return 0;
  pthread_mutex_lock(&s_offer_lock);
  size_t length = batch == s_direct_paste_id ? strlen(s_direct_paste_path) : 0;
  int found = length > 0 && length < capacity;
  if (found) memcpy(out, s_direct_paste_path, length + 1);
  pthread_mutex_unlock(&s_offer_lock);
  return found;
}

void clipboard_complete_remote_direct_paste(uint32_t batch, int success) {
  pthread_mutex_lock(&s_offer_lock);
  hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, batch);
  if (s_direct_paste_id == batch) {
    s_direct_paste_id = 0;
    s_direct_paste_path[0] = '\0';
  }
  pthread_mutex_unlock(&s_offer_lock);
  hop_log(success ? HOP_LOG_INFO : HOP_LOG_WARN,
          success ? "clip: files placed in original Finder destination" :
                    "clip: direct paste incomplete");
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
  pthread_mutex_lock(&s_offer_lock);
  int has_remote_offer = s_remote_offer_generation != 0;
  pthread_mutex_unlock(&s_offer_lock);
  NSString *destination = has_remote_offer ? clip_finder_destination() : nil;
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
  pthread_mutex_lock(&s_offer_lock);
  s_direct_paste_id = 0;
  s_direct_paste_path[0] = '\0';
  if (destination) {
    const char *path = destination.fileSystemRepresentation;
    size_t length = path ? strlen(path) : 0;
    if (length && length < sizeof(s_direct_paste_path)) {
      memcpy(s_direct_paste_path, path, length + 1);
      s_direct_paste_id = id;
      hop_log(HOP_LOG_INFO, "clip: direct Finder paste target %s", path);
    }
  }
  pthread_mutex_unlock(&s_offer_lock);
  uint8_t p[HOP_CLIP_PASTE_REQUEST_BYTES];
  int sent =
      hop_clip_paste_request_encode(p, id, origin, generation, expected) &&
      net_send(HOP_MSG_CLIP_PASTE_REQUEST, p, sizeof(p));
  if (!sent) {
    pthread_mutex_lock(&s_offer_lock);
    hop_clipboard_txn_cancel_pending_paste(&s_clip_txn, id);
    if (s_direct_paste_id == id) { s_direct_paste_id = 0; s_direct_paste_path[0] = '\0'; }
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
  if (s_direct_paste_id == id) { s_direct_paste_id = 0; s_direct_paste_path[0] = '\0'; }
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
