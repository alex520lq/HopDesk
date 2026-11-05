/* =====================================================================
 *  HopDesk - Windows 剪贴板同步
 *
 *  发送：AddClipboardFormatListener + WM_CLIPBOARDUPDATE
 *    CF_HDROP        -> 全部条目（含目录）交给数据通道，目录递归
 *    CF_DIB          -> 转成自上而下 BGRA，交给数据通道
 *    CF_UNICODETEXT  -> 控制通道（文本都很小）
 *
 *  为什么文件和图片不走控制通道：
 *    控制通道的发送队列是给鼠标键盘用的，塞一个 33 MB 的 4K 截图进去，
 *    后面几十毫秒的鼠标事件全被堵住。数据通道独立 socket、独立线程。
 *
 *  回环抑制两道：
 *    1) 协议层 origin 字段（跨设备）：收到自己发的不再写回
 *    2) 本机精确 clipboard sequence：只抑制程序刚写入的那个版本，不用时间窗吞掉用户紧随其后的复制。
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "../core/xfer_task.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

/* DROPFILES 在 MSVC 的 shellapi.h 里，MinGW 的 shellapi.h 没有（在 shlobj.h）。
 * 自己定义，省掉一个头文件版本差异 —— 布局是 ABI 固定的 20 字节。 */
typedef struct {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} HOP_DROPFILES;

/* Latest remote clipboard payload survives temporary clipboard contention.
 * Ownership of HGLOBAL transfers here; Windows owns it only after SetClipboardData. */
static int open_retry(HWND hwnd); /* used by pending remote-clipboard writer */
static int apply_global_expected(UINT format,HGLOBAL h,DWORD required_sequence,int enforce_sequence,int clear_remote);
static HGLOBAL s_pending_global;
static UINT s_pending_format;
static DWORD s_pending_sequence;
static DWORD s_remote_sequence;
static HANDLE s_image_thread;
static volatile LONG s_image_busy;

static DWORD s_last_sequence;
static SRWLOCK s_offer_lock = SRWLOCK_INIT;
static wchar_t *s_local_offer_paths[HOP_CLIP_OFFER_MAX_ITEMS];
static int s_local_offer_count;
static uint64_t s_local_offer_generation;
static uint64_t s_local_offer_sequence;
static uint32_t s_offer_request_seq;
static uint32_t s_remote_offer_origin;
static uint64_t s_remote_offer_generation;
static uint32_t s_pending_paste_request;
static DWORD s_pending_paste_sequence;
static uint32_t s_seen_paste_requests[32];
static unsigned s_seen_paste_next;
static int s_pending_clear_remote;
static int s_paste_scan_active;
static int s_clip_stopping;
static HANDLE s_paste_workers_done;

static uint32_t clip_local_device(void) {
    return g_app.cfg.role == HOP_ROLE_SERVER ? HOP_DEV_SERVER : HOP_DEV_CLIENT;
}
static void clip_send_offer_clear(uint32_t origin,uint64_t generation);
static void clip_clear_remote_offer_local(void) {
    AcquireSRWLockExclusive(&s_offer_lock);
    s_remote_offer_origin = 0; s_remote_offer_generation = 0;
    s_pending_paste_request = 0; s_pending_paste_sequence = 0;
    ReleaseSRWLockExclusive(&s_offer_lock);
}
static void clip_invalidate_remote_offer(int notify_peer) {
    uint32_t origin=0; uint64_t generation=0;
    AcquireSRWLockExclusive(&s_offer_lock);
    origin=s_remote_offer_origin; generation=s_remote_offer_generation;
    s_remote_offer_origin=0; s_remote_offer_generation=0;
    s_pending_paste_request=0; s_pending_paste_sequence=0;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if(notify_peer&&origin&&generation)clip_send_offer_clear(origin,generation);
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
    generation = s_local_offer_generation;
    for (int i=0;i<s_local_offer_count;++i) free(s_local_offer_paths[i]);
    memset(s_local_offer_paths,0,sizeof(s_local_offer_paths));
    s_local_offer_count = 0; s_local_offer_generation = 0;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if (notify_peer && generation) clip_send_offer_clear(clip_local_device(), generation);
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
    uint64_t generation = ++s_local_offer_sequence;
    if (!generation) generation = ++s_local_offer_sequence;
    s_local_offer_generation = generation;
    for(int i=0;i<count;++i) s_local_offer_paths[i]=copies[i];
    s_local_offer_count=count; s_local_offer_generation=generation;
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

static void discard_pending(void) {
    if(s_pending_global)GlobalFree(s_pending_global);
    s_pending_global=NULL;
}

static void schedule_clip_retry(void) {
    if (g_app.hwnd) SetTimer(g_app.hwnd, IDT_CLIP_RETRY, 120, NULL);
}

static int apply_global_expected(UINT format,HGLOBAL h,DWORD required_sequence,int enforce_sequence,int clear_remote) {
    if (!h) return 0;
    DWORD expected=enforce_sequence?required_sequence:GetClipboardSequenceNumber();
    if(GetClipboardSequenceNumber()!=expected){GlobalFree(h);if(clear_remote)clip_clear_remote_offer_local();return 0;}
    discard_pending();
    s_pending_clear_remote=0;
    if (open_retry(g_app.hwnd)) {
        if(GetClipboardSequenceNumber()!=expected) {
            CloseClipboard(); GlobalFree(h);
            if(clear_remote)clip_clear_remote_offer_local();
            return 0;
        }
        if (EmptyClipboard()) {
            if (SetClipboardData(format, h)) {
                s_remote_sequence = GetClipboardSequenceNumber();
                s_last_sequence = s_remote_sequence;
                if(clear_remote)clip_clear_remote_offer_local();
                CloseClipboard();
                return 1;
            }
            /* EmptyClipboard succeeded, so this sequence change is ours. Keep
             * the HGLOBAL queued against that exact empty clipboard version. */
            expected=GetClipboardSequenceNumber();
        }
        CloseClipboard();
    }
    if(GetClipboardSequenceNumber()!=expected){GlobalFree(h);if(clear_remote)clip_clear_remote_offer_local();return 0;}
    if (s_pending_global) GlobalFree(s_pending_global);
    s_pending_clear_remote=clear_remote;
    s_pending_global = h;
    s_pending_format = format;
    s_pending_sequence = expected;
    schedule_clip_retry();
    hop_log(HOP_LOG_WARN, "clip: clipboard busy; queued remote format %u for retry", format);
    return 0;
}
static int apply_global(UINT format,HGLOBAL h){return apply_global_expected(format,h,0,0,0);}

void clip_retry_pending(void) {
    if (s_pending_global) {
        if (open_retry(g_app.hwnd)) {
            if(GetClipboardSequenceNumber()!=s_pending_sequence) {
                int clear_remote=s_pending_clear_remote;
                discard_pending(); s_pending_clear_remote=0;
                if(clear_remote)clip_clear_remote_offer_local();
                CloseClipboard();
                KillTimer(g_app.hwnd,IDT_CLIP_RETRY); return;
            }
            if (EmptyClipboard()) {
                if (SetClipboardData(s_pending_format, s_pending_global)) {
                    s_pending_global = NULL; /* ownership transferred to Windows */
                    s_remote_sequence = GetClipboardSequenceNumber();
                    s_last_sequence = s_remote_sequence;
                    if(s_pending_clear_remote)clip_clear_remote_offer_local();
                    s_pending_clear_remote=0;
                    hop_log(HOP_LOG_INFO, "clip: pending remote clipboard applied");
                    CloseClipboard();
                } else {
                    /* Only our EmptyClipboard changed the sequence; retain the
                     * queued payload but reject any later user clipboard change. */
                    s_pending_sequence=GetClipboardSequenceNumber();
                    CloseClipboard();
                    schedule_clip_retry();
                    return;
                }
            } else {
                CloseClipboard();
                schedule_clip_retry();
                return;
            }
        } else {
            schedule_clip_retry();
            return;
        }
    }
    KillTimer(g_app.hwnd, IDT_CLIP_RETRY);
}

/* 图片上限：4K 截图裸 BGRA 约 33 MB，留到 256 MB 足够，再大就是异常了 */
#define CLIP_IMG_MAX_BYTES (256u * 1024u * 1024u)

/* 剪贴板是全局独占资源，别的程序可能正拿着 —— 短暂重试 */
static int open_retry(HWND hwnd) {
    /* UI-thread path: never sleep while low-level hooks need this thread. */
    return OpenClipboard(hwnd) != 0;
}

void clip_init(HWND hwnd) {
    s_clip_stopping=0;s_paste_scan_active=0;
    if(s_paste_workers_done)CloseHandle(s_paste_workers_done);
    s_paste_workers_done=CreateEventW(NULL,TRUE,TRUE,NULL);
    s_last_sequence = GetClipboardSequenceNumber();
    if (!AddClipboardFormatListener(hwnd))
        hop_log(HOP_LOG_ERROR, "AddClipboardFormatListener failed: %lu", GetLastError());
    else
        hop_log(HOP_LOG_INFO, "clipboard listener installed");
}

void clip_shutdown(HWND hwnd) {
    AcquireSRWLockExclusive(&s_offer_lock);s_clip_stopping=1;ReleaseSRWLockExclusive(&s_offer_lock);
    if(s_paste_workers_done)WaitForSingleObject(s_paste_workers_done,INFINITE);
    RemoveClipboardFormatListener(hwnd);
    if (s_image_thread) { WaitForSingleObject(s_image_thread, INFINITE); CloseHandle(s_image_thread); s_image_thread = NULL; }
    MSG pending;
    while (PeekMessageW(&pending, hwnd, WM_APP_CLIP, WM_APP_CLIP, PM_REMOVE))
        if (pending.lParam) GlobalFree((HGLOBAL)pending.lParam);
    while (PeekMessageW(&pending, hwnd, WM_APP_CLIP_PASTE, WM_APP_CLIP_PASTE, PM_REMOVE))
        if (pending.lParam) GlobalFree((HGLOBAL)pending.lParam);
    if(s_paste_workers_done){CloseHandle(s_paste_workers_done);s_paste_workers_done=NULL;}
    KillTimer(hwnd, IDT_CLIP_RETRY);
    if (s_pending_global) { GlobalFree(s_pending_global); s_pending_global = NULL; }
}

/* ---- 发送 ---- */

static int send_text(const char *utf8, uint32_t len) {
    uint8_t *p = (uint8_t *)malloc(4 + len);
    if (!p) return 0;
    hop_put_be32(p, (g_app.cfg.role==HOP_ROLE_SERVER?HOP_DEV_SERVER:HOP_DEV_CLIENT));
    memcpy(p + 4, utf8, len);
    int ok = net_server_send(g_app.net, HOP_MSG_CLIP_TEXT, p, 4 + len);
    if (ok) hop_log(HOP_LOG_INFO, "clip: text out %u bytes", len);
    else hop_log(HOP_LOG_WARN, "clip: text out failed; will retry");
    free(p);
    return ok;
}

/* CF_HDROP：全部条目一次性交给数据通道，目录由 xfer 递归展开 */
static int send_drop(HDROP drop, UINT count) {
    if (count == 0 || count > HOP_CLIP_OFFER_MAX_ITEMS) {
        hop_log(HOP_LOG_WARN, "clip: file selection exceeds Offer item limit; no partial Offer published");
        return 0;
    }
    wchar_t **paths = (wchar_t **)calloc(count, sizeof(wchar_t *));
    if (!paths) return 0;

    int n = 0, failed = 0;
    for (UINT i = 0; i < count; ++i) {
        UINT need = DragQueryFileW(drop, i, NULL, 0);
        if (need == 0 || need >= 32768) { failed=1; break; }
        wchar_t *buf = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
        if (!buf) { failed=1; break; }
        if (DragQueryFileW(drop, i, buf, need + 1) == 0) {
            free(buf); failed=1; break;
        }
        paths[n++] = buf;
    }
    if(failed) {
        for(int i=0;i<n;++i)free(paths[i]);
        free(paths);
        hop_log(HOP_LOG_WARN,"clip: unable to snapshot all selected paths; no partial Offer published");
        return 0;
    }

    int ok = 0;
    if (n > 0) {
        hop_log(HOP_LOG_INFO, "clip: %d path(s) copied; publishing lazy file Offer", n);
        ok = clip_publish_local_offer((const wchar_t *const *)paths, n);
        if (!ok) hop_log(HOP_LOG_WARN, "clip: unable to publish file offer; local clipboard remains unchanged");
    }

    for (int i = 0; i < n; ++i) free(paths[i]);
    free(paths);
    return ok;
}

static volatile LONG s_image_task_seq;

static int image_work_task_begin(hop_xfer_task *task) {
    if (!task) return 0;
    memset(task, 0, sizeof(*task));
    uint32_t transfer_id = (uint32_t)InterlockedIncrement(&s_image_task_seq);
    if (!transfer_id) transfer_id = (uint32_t)InterlockedIncrement(&s_image_task_seq);
    if (!hop_xfer_task_init(task, 0, 0, transfer_id) ||
        !hop_xfer_task_reserve_job(task) ||
        !hop_xfer_task_set_state(task, HOP_XFER_TASK_ACTIVE)) {
        if (task->initialized) {
            (void)hop_xfer_task_finish(task, HOP_XFER_TASK_FAILED);
            hop_xfer_task_dispose(task);
        }
        return 0;
    }
    return 1;
}

static void image_work_task_finish(hop_xfer_task *task, int success) {
    if (!task || !task->initialized) return;
    if (task->reservation.image_temp_bytes &&
        !hop_xfer_task_release_image_temp(task, task->reservation.image_temp_bytes)) {
        hop_log(HOP_LOG_ERROR, "clip: image temporary budget release failed (task=%llu)",
                (unsigned long long)task->id);
    }
    (void)hop_xfer_task_finish(task, success ? HOP_XFER_TASK_COMPLETED : HOP_XFER_TASK_FAILED);
    hop_xfer_task_dispose(task);
}

/* CF_DIB -> 自上而下 BGRA。只处理 BI_RGB 的 24/32 位，其余（压缩格式）跳过。 */
static int send_dib(HANDLE h, DWORD sequence, hop_xfer_task *task) {
    SIZE_T available = GlobalSize(h);
    if (available < sizeof(BITMAPINFOHEADER)) return -1;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)GlobalLock(h);
    if (!bi) return 0;

    int w = (int)bi->biWidth;
    int hgt = (int)bi->biHeight;
    int bpp = (int)bi->biBitCount;
    int topdown = 0;

    if (hgt == INT_MIN) { GlobalUnlock(h); return -1; }
    if (hgt < 0) { topdown = 1; hgt = -hgt; }
    if (w <= 0 || hgt <= 0 || w > INT_MAX / 4 ||
        bi->biCompression != BI_RGB || (bpp != 24 && bpp != 32)) {
        hop_log(HOP_LOG_INFO, "clip: dib %dx%d %dbpp comp %lu unsupported, skipped",
                w, hgt, bpp, (unsigned long)bi->biCompression);
        GlobalUnlock(h);
        return -1;
    }

    size_t stride = (size_t)w * 4;
    if ((size_t)hgt > SIZE_MAX / stride) { GlobalUnlock(h); return -1; }
    size_t total = stride * (size_t)hgt;
    if (total > CLIP_IMG_MAX_BYTES) {
        hop_log(HOP_LOG_WARN, "clip: image %.1f MB too large, skipped",
                (double)total / 1048576.0);
        GlobalUnlock(h);
        return -1;
    }

    size_t src_bits = (size_t)w * (size_t)bpp;
    if (src_bits > SIZE_MAX - 31u) { GlobalUnlock(h); return -1; }
    size_t src_stride = ((src_bits + 31u) / 32u) * 4u;
    uint64_t offset = (uint64_t)bi->biSize + (uint64_t)bi->biClrUsed * 4;
    if (bi->biSize < sizeof(*bi) || offset > available ||
        !src_stride || src_stride > available - (size_t)offset ||
        (size_t)hgt > (available - (size_t)offset) / src_stride) {
        GlobalUnlock(h); return -1;
    }
    if (!task || !hop_xfer_task_reserve_image_temp(task, (uint64_t)total)) {
        GlobalUnlock(h);
        hop_log(HOP_LOG_WARN, "clip: DIB conversion memory budget exhausted");
        return -1;
    }

    const uint8_t *src = (const uint8_t *)bi + (size_t)offset;
    uint8_t *dst = (uint8_t *)malloc(total);
    if (!dst) {
        GlobalUnlock(h);
        (void)hop_xfer_task_release_image_temp(task, (uint64_t)total);
        return 0;
    }

    for (int y = 0; y < hgt; ++y) {
        /* DIB 默认自下而上；topdown 时行序已经正确 */
        int sy = topdown ? y : (hgt - 1 - y);
        const uint8_t *s = src + (size_t)sy * src_stride;
        uint8_t *d = dst + (size_t)y * stride;
        if (bpp == 32) {
            for (int x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
                d[x * 4 + 3] = 255;          /* 源 alpha 常为 0，强制不透明 */
            }
        } else {
            for (int x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 3 + 0];
                d[x * 4 + 1] = s[x * 3 + 1];
                d[x * 4 + 2] = s[x * 3 + 2];
                d[x * 4 + 3] = 255;
            }
        }
    }

    GlobalUnlock(h);
    hop_log(HOP_LOG_INFO, "clip: image out %dx%d task=%llu", w, hgt,
            (unsigned long long)task->id);
    int ok = -1;
    if (GetClipboardSequenceNumber() == sequence)
        ok = xfer_submit_image(dst, w, hgt, (int)stride);
    free(dst);
    if (!hop_xfer_task_release_image_temp(task, (uint64_t)total)) {
        hop_log(HOP_LOG_ERROR, "clip: DIB conversion budget release failed (task=%llu)",
                (unsigned long long)task->id);
    }
    if (!ok) hop_log(HOP_LOG_WARN, "clip: image not queued; will retry");
    return ok;
}

/* Snapshot and conversion run on a single worker. Closing the clipboard before
 * pixel conversion avoids holding the global clipboard lock during CPU work. */
static DWORD WINAPI image_worker(LPVOID arg) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DWORD sequence = (DWORD)(ULONG_PTR)arg;
    UINT format = CF_DIB;
    HGLOBAL copy = NULL;
    hop_xfer_task image_task;
    memset(&image_task, 0, sizeof(image_task));
    int image_task_ready = 0;
    int opened = 0, result = 0;
    for (int i = 0; i < 6 && !opened; ++i) {
        opened = OpenClipboard(NULL) != 0;
        if (!opened) Sleep(15);
    }
    if (opened) {
        if (GetClipboardSequenceNumber() == sequence) {
            if (IsClipboardFormatAvailable(CF_HDROP)) format = CF_HDROP;
            int guard = hop_watchdog_begin("clipboard delayed rendering", HOP_IO_DEADLINE_MS);
            HANDLE src = GetClipboardData(format);
            hop_watchdog_end(guard);
            SIZE_T bytes = src ? GlobalSize(src) : 0;
            if (bytes >= (format == CF_DIB ? sizeof(BITMAPINFOHEADER) : sizeof(HOP_DROPFILES)) &&
                bytes <= CLIP_IMG_MAX_BYTES) {
                if (format == CF_DIB) {
                    if (!image_work_task_begin(&image_task)) result = -1;
                    else {
                        image_task_ready = 1;
                        if (!hop_xfer_task_reserve_image_temp(&image_task, (uint64_t)bytes)) {
                            hop_log(HOP_LOG_WARN, "clip: DIB snapshot memory budget exhausted (task=%llu)",
                                    (unsigned long long)image_task.id);
                            result = -1;
                        }
                    }
                }
                if (result != -1) {
                    copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    void *from = GlobalLock(src);
                    void *to = copy ? GlobalLock(copy) : NULL;
                    if (from && to) memcpy(to, from, bytes);
                    if (from) GlobalUnlock(src);
                    if (to) GlobalUnlock(copy);
                    if (!from || !to) { if (copy) GlobalFree(copy); copy = NULL; }
                }
            } else result = -1;
        }
        CloseClipboard();
    }
    if (copy) {
        if (GetClipboardSequenceNumber() != sequence) result = -1;
        else if (format == CF_HDROP) {
            UINT count = DragQueryFileW((HDROP)copy, 0xFFFFFFFF, NULL, 0);
            result = count ? send_drop((HDROP)copy, count) : -1;
        } else result = send_dib(copy, sequence, &image_task);
        GlobalFree(copy);
    }
    if (image_task_ready)
        image_work_task_finish(&image_task, result > 0);
    InterlockedExchange(&s_image_busy, 0);
    if (!result || GetClipboardSequenceNumber() != sequence)
        PostMessageW(g_app.hwnd, WM_APP_CLIP, 0, 0);
    return 0;
}
static void queue_image(void) {
    if (InterlockedCompareExchange(&s_image_busy, 1, 0)) {
        schedule_clip_retry(); return;
    }
    if (s_image_thread) {
        if (WaitForSingleObject(s_image_thread, 0) != WAIT_OBJECT_0) {
            InterlockedExchange(&s_image_busy, 0); schedule_clip_retry(); return;
        }
        CloseHandle(s_image_thread);
    }
    DWORD sequence = GetClipboardSequenceNumber();
    s_image_thread = CreateThread(NULL, 0, image_worker, (LPVOID)(ULONG_PTR)sequence, 0, NULL);
    if (!s_image_thread) { InterlockedExchange(&s_image_busy, 0); schedule_clip_retry(); }
}
/* This is the only cross-thread entry into main-thread clipboard state. */
void clip_on_worker_message(UINT format, HGLOBAL h) {
    if (!format) schedule_clip_retry();
    else apply_global(format, h);
}
static void post_remote_global(UINT format, HGLOBAL h) {
    if (!PostMessageW(g_app.hwnd, WM_APP_CLIP, format, (LPARAM)h)) GlobalFree(h);
}

/* WM_CLIPBOARDUPDATE -> 广播本机内容 */
void clip_on_update(void) {
    HANDLE h;

    DWORD sequence=GetClipboardSequenceNumber();
    if(s_remote_sequence && sequence==s_remote_sequence){s_last_sequence=sequence;return;}
    if(sequence!=s_last_sequence){
        s_last_sequence=sequence;
        clip_invalidate_remote_offer(1);
        clip_clear_local_offer(1);
    }

    if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_HDROP)) {
        queue_image(); return;
    }
    if (!open_retry(g_app.hwnd)) {
        hop_log(HOP_LOG_WARN, "clip: clipboard busy, scheduling retry");
        schedule_clip_retry();
        return;
    }

    /* Clipboard can change between availability check and OpenClipboard. */
    if (IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIB)) {
        CloseClipboard(); queue_image(); return;
    }

    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 1 && n <= HOP_TEXT_MAX) {
                char *u = (char *)malloc((size_t)n);
                if (u) {
                    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL) > 0)
                        if (!send_text(u, (uint32_t)(n - 1))) schedule_clip_retry(); /* 不含结尾 NUL */
                    free(u);
                }
            } else if (n > HOP_TEXT_MAX) {
                hop_log(HOP_LOG_WARN, "clip: text %d bytes > limit, skipped", n);
            }
            GlobalUnlock(h);
        }
        CloseClipboard();
        return;
    }

    CloseClipboard();
    clip_clear_local_offer(1);
    clip_clear_remote_offer_local();
    hop_log(HOP_LOG_INFO, "clip: unsupported clipboard format, ignored");
}

/* ---- 接收 ---- */

/* 收到对端文本：payload = origin(4) + UTF-8 */
void clip_apply_remote(const uint8_t *p, uint32_t len) {
    char *u;
    int wn;
    HGLOBAL h;

    if (len < 5) return;
    if (!hop_clip_should_apply_remote(hop_get_be32(p), (g_app.cfg.role==HOP_ROLE_SERVER?HOP_DEV_SERVER:HOP_DEV_CLIENT))) return; /* 自己发的，回环 */
    clip_clear_local_offer(1);
    clip_clear_remote_offer_local();

    u = (char *)malloc((size_t)(len - 3));           /* 文本 + NUL */
    if (!u) return;
    memcpy(u, p + 4, len - 4);
    u[len - 4] = '\0';

    wn = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    if (wn <= 0) { free(u); return; }

    h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wn * sizeof(wchar_t));
    if (!h) { free(u); return; }

    {
        void *d = GlobalLock(h);
        if (!d) { GlobalFree(h); free(u); return; }
        MultiByteToWideChar(CP_UTF8, 0, u, -1, (wchar_t *)d, wn);
        GlobalUnlock(h);
    }

    if (apply_global(CF_UNICODETEXT, h)) {
        h = NULL; /* ownership transferred to Windows */
        hop_log(HOP_LOG_INFO, "clip: text in %u bytes", len - 4);
    } else {
        h = NULL; /* apply_global retained ownership for retry */
    }
    if (h) GlobalFree(h);
    free(u);
}

/* 一批文件/目录落盘完毕：把顶层条目挂到剪贴板，用户直接 Ctrl+V */
static HGLOBAL make_drop_global(const wchar_t *const *paths,int n) {
    HOP_DROPFILES *df; HGLOBAL h; SIZE_T bytes,off;
    if(!paths||n<=0||n>(int)HOP_CLIP_OFFER_MAX_ITEMS)return NULL;
    bytes=sizeof(HOP_DROPFILES);
    for(int i=0;i<n;++i){
        if(!paths[i])return NULL;
        SIZE_T chars=wcslen(paths[i]);
        if(chars>(SIZE_MAX/sizeof(wchar_t))-1 || bytes>SIZE_MAX-(chars+1)*sizeof(wchar_t))return NULL;
        bytes+=(chars+1)*sizeof(wchar_t);
    }
    bytes+=sizeof(wchar_t);
    h=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);
    if(!h)return NULL;
    df=(HOP_DROPFILES*)GlobalLock(h);
    if(!df){GlobalFree(h);return NULL;}
    df->pFiles=sizeof(HOP_DROPFILES);df->fWide=TRUE;off=sizeof(HOP_DROPFILES);
    for(int i=0;i<n;++i){
        SIZE_T cb=wcslen(paths[i])*sizeof(wchar_t);
        memcpy((BYTE*)df+off,paths[i],cb);off+=cb+sizeof(wchar_t);
    }
    GlobalUnlock(h);return h;
}
void clip_apply_remote_paths(const wchar_t *const *paths,int n) {
    HGLOBAL h=make_drop_global(paths,n);
    if(h)post_remote_global(CF_HDROP,h);
}

/* 收到图片：裸 BGRA 自上而下 -> CF_DIB（32 位，自下而上，BI_RGB） */
void clip_apply_remote_image(const uint8_t *bgra, int w, int h, int stride) {
    clip_clear_local_offer(1);
    clip_clear_remote_offer_local();
    if (!bgra || w <= 0 || h <= 0 || w > INT_MAX / 4 || stride < w * 4) return;

    size_t out_stride = (size_t)w * 4;
    size_t need = sizeof(BITMAPINFOHEADER) + out_stride * (size_t)h;
    if (need > CLIP_IMG_MAX_BYTES) {
        hop_log(HOP_LOG_WARN, "clip: incoming image too large, skipped");
        return;
    }

    HGLOBAL gh = GlobalAlloc(GMEM_MOVEABLE, need);
    if (!gh) return;
    uint8_t *base = (uint8_t *)GlobalLock(gh);
    if (!base) { GlobalFree(gh); return; }

    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)base;
    memset(bi, 0, sizeof(*bi));
    bi->biSize = sizeof(*bi);
    bi->biWidth = w;
    bi->biHeight = h;              /* 正值 = 自下而上，与 DIB 约定一致 */
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(out_stride * (size_t)h);

    uint8_t *dst = base + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; ++y) {
        memcpy(dst + (size_t)y * out_stride,
               bgra + (size_t)(h - 1 - y) * stride, out_stride);
    }
    GlobalUnlock(gh);

    post_remote_global(CF_DIB, gh);
}

/* Lazy file clipboard request/response handling. */
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
    (void)apply_global_expected(CF_HDROP,h,expected_sequence,1,1);
}
