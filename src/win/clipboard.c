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
#include "../core/xfer_clipboard_txn.h"
#include "../core/xfer_clipboard_coord.h"
#include "../core/xfer_budget.h"

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
static int apply_global_expected(UINT format,HGLOBAL h,DWORD required_sequence,int enforce_sequence,uint32_t request_id);
static HGLOBAL s_pending_global;
static UINT s_pending_format;
static DWORD s_pending_sequence;
static uint32_t s_queued_paste_request;
static DWORD s_remote_sequence;
static HANDLE s_image_thread;
static volatile LONG s_image_busy;
typedef struct clip_image_task_owner {
    struct clip_image_task_owner *next;
    hop_xfer_task task;
} clip_image_task_owner;
static clip_image_task_owner *s_pending_image_tasks;

static DWORD s_last_sequence;
static SRWLOCK s_offer_lock = SRWLOCK_INIT;
static hop_clipboard_txn s_clip_txn;
static wchar_t *s_local_offer_paths[HOP_CLIP_OFFER_MAX_ITEMS];
static int s_local_offer_count;
#define s_local_offer_sequence (s_clip_txn.local_offer_sequence)
#define s_offer_request_seq (s_clip_txn.request_sequence)
#define s_remote_offer_origin (s_clip_txn.remote_offer_origin)
#define s_remote_offer_generation (s_clip_txn.remote_offer_generation)
#define s_pending_paste_sequence (s_clip_txn.pending_clipboard_version)
#define s_seen_paste_requests (s_clip_txn.seen_requests)
#define s_seen_paste_next (s_clip_txn.seen_next)
#define s_paste_scan_active (s_clip_txn.paste_scan_active)
#define s_clip_stopping (s_clip_txn.stopping)
static HANDLE s_paste_workers_done;

#include "clipboard_offer.inc"

static int image_work_retry_pending(void);

static void discard_pending(void) {
    if(s_pending_global) GlobalFree(s_pending_global);
    s_pending_global=NULL;
}


static void schedule_clip_retry(void) {
    if (g_app.hwnd) SetTimer(g_app.hwnd, IDT_CLIP_RETRY, 120, NULL);
}

static int clip_staged_request_matches(uint32_t request_id, uint64_t expected) {
    uint64_t staged_version=0;
    return hop_clipboard_txn_get_staged_paste(&s_clip_txn,request_id,&staged_version) &&
           staged_version==expected;
}
static void clip_cancel_staged_if_match(uint32_t request_id, uint64_t expected) {
    if (clip_staged_request_matches(request_id,expected))
        hop_clipboard_txn_cancel_pending_paste(&s_clip_txn,request_id);
}

static void discard_pending_replacing(uint32_t next_request_id) {
    uint32_t old_id=s_queued_paste_request;
    DWORD old_version=s_pending_sequence;
    if(old_id && old_id!=next_request_id)
        clip_cancel_staged_if_match(old_id,old_version);
    discard_pending();
    s_queued_paste_request=0;
}

static int apply_global_expected(UINT format,HGLOBAL h,DWORD required_sequence,int enforce_sequence,uint32_t request_id) {
    if (!h) return 0;
    DWORD expected=enforce_sequence?required_sequence:GetClipboardSequenceNumber();
    if(request_id) AcquireSRWLockExclusive(&s_offer_lock);
    if(GetClipboardSequenceNumber()!=expected) {
        if(request_id) { clip_cancel_staged_if_match(request_id,expected); ReleaseSRWLockExclusive(&s_offer_lock); }
        GlobalFree(h); return 0;
    }
    if(request_id) {
        int valid=clip_staged_request_matches(request_id,expected);
        if(!valid){ReleaseSRWLockExclusive(&s_offer_lock);GlobalFree(h);return 0;}
        ReleaseSRWLockExclusive(&s_offer_lock);
    }
    if(open_retry(g_app.hwnd)) {
        if(request_id) AcquireSRWLockExclusive(&s_offer_lock);
        int sequence_matches=GetClipboardSequenceNumber()==expected;
        int valid=sequence_matches;
        if(request_id) valid=valid && clip_staged_request_matches(request_id,expected);
        if(!sequence_matches && request_id) clip_cancel_staged_if_match(request_id,expected);
        if(valid) {
            if(!request_id) AcquireSRWLockExclusive(&s_offer_lock);
            discard_pending_replacing(request_id);
            if(!request_id) ReleaseSRWLockExclusive(&s_offer_lock);
            if(EmptyClipboard()) {
                DWORD before_empty=expected;
                if(SetClipboardData(format,h)) {
                    s_remote_sequence=GetClipboardSequenceNumber(); s_last_sequence=s_remote_sequence;
                    int finished=1;
                    if(request_id) finished=hop_clip_coord_paste_apply_result(&s_clip_txn,request_id,expected,expected,1)==HOP_CLIP_PASTE_APPLY_COMPLETE;
                    if(request_id) ReleaseSRWLockExclusive(&s_offer_lock);
                    CloseClipboard();
                    if(request_id && finished) { s_queued_paste_request=0; ui_notify_paste_ready(); }
                    return 1;
                }
                expected=GetClipboardSequenceNumber(); s_remote_sequence=expected; s_last_sequence=expected;
                if (request_id && hop_clip_coord_paste_apply_result(&s_clip_txn,
                        request_id, before_empty, expected, 0) != HOP_CLIP_PASTE_APPLY_REARM) {
                    ReleaseSRWLockExclusive(&s_offer_lock);
                    CloseClipboard();
                    GlobalFree(h);
                    return 0;
                }
            }
            if(request_id) ReleaseSRWLockExclusive(&s_offer_lock);
            CloseClipboard();
        } else {
            if(request_id) ReleaseSRWLockExclusive(&s_offer_lock);
            CloseClipboard(); GlobalFree(h); return 0;
        }
    }
    if(GetClipboardSequenceNumber()!=expected) {
        if(request_id) {
            AcquireSRWLockExclusive(&s_offer_lock);
            clip_cancel_staged_if_match(request_id,expected);
            ReleaseSRWLockExclusive(&s_offer_lock);
        }
        GlobalFree(h); return 0;
    }
    /* Queue only after confirming the same transaction still owns this request. */
    if(request_id) {
        AcquireSRWLockExclusive(&s_offer_lock);
        int valid=clip_staged_request_matches(request_id,expected);
        if(!valid){ReleaseSRWLockExclusive(&s_offer_lock);GlobalFree(h);return 0;}
    }
    if(!request_id) AcquireSRWLockExclusive(&s_offer_lock);
    discard_pending_replacing(request_id);
    s_pending_global=h; s_pending_format=format; s_pending_sequence=expected;
    s_queued_paste_request=request_id;
    ReleaseSRWLockExclusive(&s_offer_lock);
    schedule_clip_retry(); hop_log(HOP_LOG_WARN,"clip: clipboard busy; queued remote format %u for retry",format);
    return 0;
}
static int apply_global(UINT format,HGLOBAL h){return apply_global_expected(format,h,0,0,0);}

void clip_retry_pending(void) {
    if(!s_pending_global) { KillTimer(g_app.hwnd,IDT_CLIP_RETRY); return; }
    uint32_t request_id=s_queued_paste_request; int paste=request_id!=0;
    if(paste) AcquireSRWLockExclusive(&s_offer_lock);
    if(paste && !clip_staged_request_matches(request_id,s_pending_sequence)) {
        ReleaseSRWLockExclusive(&s_offer_lock); discard_pending(); s_queued_paste_request=0;
        KillTimer(g_app.hwnd,IDT_CLIP_RETRY); return;
    }
    if(paste) ReleaseSRWLockExclusive(&s_offer_lock);
    if(!open_retry(g_app.hwnd)){schedule_clip_retry();return;}
    if(paste) AcquireSRWLockExclusive(&s_offer_lock);
    int valid=GetClipboardSequenceNumber()==s_pending_sequence;
    if(paste) valid=valid && clip_staged_request_matches(request_id,s_pending_sequence);
    if(!valid) {
        if(paste) clip_cancel_staged_if_match(request_id,s_pending_sequence);
        if(paste) ReleaseSRWLockExclusive(&s_offer_lock);
        discard_pending(); s_queued_paste_request=0; CloseClipboard(); KillTimer(g_app.hwnd,IDT_CLIP_RETRY); return;
    }
    if(EmptyClipboard()) {
        DWORD before_empty=s_pending_sequence;
        if(SetClipboardData(s_pending_format,s_pending_global)) {
            s_pending_global=NULL; s_remote_sequence=GetClipboardSequenceNumber(); s_last_sequence=s_remote_sequence;
            int finished=1;
            if(paste) finished=hop_clip_coord_paste_apply_result(&s_clip_txn,request_id,s_pending_sequence,s_pending_sequence,1)==HOP_CLIP_PASTE_APPLY_COMPLETE;
            if(paste) ReleaseSRWLockExclusive(&s_offer_lock);
            CloseClipboard(); hop_log(HOP_LOG_INFO,"clip: pending remote clipboard applied");
            if(paste && finished) { s_queued_paste_request=0; ui_notify_paste_ready(); }
        } else {
            DWORD next=GetClipboardSequenceNumber(); s_pending_sequence=next; s_remote_sequence=next; s_last_sequence=next;
            int rearm = !paste || hop_clip_coord_paste_apply_result(&s_clip_txn,
                request_id, before_empty, next, 0) == HOP_CLIP_PASTE_APPLY_REARM;
            if(paste) ReleaseSRWLockExclusive(&s_offer_lock);
            CloseClipboard();
            if (rearm) {
                schedule_clip_retry();
            } else {
                discard_pending();
                s_queued_paste_request = 0;
                KillTimer(g_app.hwnd, IDT_CLIP_RETRY);
            }
            return;
        }
    } else {
        if(paste) ReleaseSRWLockExclusive(&s_offer_lock);
        CloseClipboard(); schedule_clip_retry(); return;
    }
    KillTimer(g_app.hwnd,IDT_CLIP_RETRY);
}

/* 图片上限：4K 截图裸 BGRA 约 33 MB，留到 256 MB 足够，再大就是异常了 */
#define CLIP_IMG_MAX_BYTES (256u * 1024u * 1024u)

/* 剪贴板是全局独占资源，别的程序可能正拿着 —— 短暂重试 */
static int open_retry(HWND hwnd) {
    /* UI-thread path: never sleep while low-level hooks need this thread. */
    return OpenClipboard(hwnd) != 0;
}

void clip_init(HWND hwnd) {
    hop_clipboard_txn_init(&s_clip_txn);hop_clipboard_txn_set_stopping(&s_clip_txn,0);
    if(s_paste_workers_done)CloseHandle(s_paste_workers_done);
    s_paste_workers_done=CreateEventW(NULL,TRUE,TRUE,NULL);
    s_last_sequence = GetClipboardSequenceNumber();
    if (!AddClipboardFormatListener(hwnd))
        hop_log(HOP_LOG_ERROR, "AddClipboardFormatListener failed: %lu", GetLastError());
    else
        hop_log(HOP_LOG_INFO, "clipboard listener installed");
}

int clip_shutdown(HWND hwnd) {
    AcquireSRWLockExclusive(&s_offer_lock);
    hop_clipboard_txn_set_stopping(&s_clip_txn, 1);
    HANDLE workers_done = s_paste_workers_done;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if (workers_done && WaitForSingleObject(workers_done, INFINITE) != WAIT_OBJECT_0) {
        hop_log(HOP_LOG_ERROR, "clip: waiting for paste worker failed");
        return 0;
    }
    RemoveClipboardFormatListener(hwnd);
    if (s_image_thread) { WaitForSingleObject(s_image_thread, INFINITE); CloseHandle(s_image_thread); s_image_thread = NULL; }
    if (!image_work_retry_pending()) {
        hop_log(HOP_LOG_ERROR, "clip: image Task/Reservation cleanup remains pending; retain Task owner for retry");
        return 0;
    }
    MSG pending;
    while (PeekMessageW(&pending, hwnd, WM_APP_CLIP, WM_APP_CLIP, PM_REMOVE))
        if (pending.lParam) GlobalFree((HGLOBAL)pending.lParam);
    while (PeekMessageW(&pending, hwnd, WM_APP_CLIP_PASTE, WM_APP_CLIP_PASTE, PM_REMOVE))
        if (pending.lParam) GlobalFree((HGLOBAL)pending.lParam);
    AcquireSRWLockExclusive(&s_offer_lock);
    s_paste_workers_done = NULL;
    ReleaseSRWLockExclusive(&s_offer_lock);
    if (workers_done) CloseHandle(workers_done);
    KillTimer(hwnd, IDT_CLIP_RETRY);
    if (s_pending_global) { GlobalFree(s_pending_global); s_pending_global = NULL; }
    return 1;
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
    return hop_xfer_task_init_classified(task, 0, 0, transfer_id,
               HOP_XFER_DIR_NONE, HOP_TASK_KIND_IMAGE_WORK) &&
           hop_xfer_task_reserve_job(task) &&
           hop_xfer_task_set_state(task, HOP_XFER_TASK_ACTIVE);
}
static int image_work_task_finish(clip_image_task_owner *owner, int success);

/* Admission is shared by the worker and its production-path fixture. Return
 * 1 when the input snapshot is reserved, 0 for retryable pressure, -1 when
 * the image can never fit the configured ceilings or cleanup failed. */
static int image_snapshot_prepare(size_t bytes, clip_image_task_owner **owner_out) {
    if (!owner_out) return -1;
    *owner_out=NULL;
    hop_xfer_budget_snapshot budget;
    hop_xfer_budget_get_snapshot(&budget);
    if (!hop_xfer_budget_image_conversion_fits((uint64_t)bytes,0,&budget.limits)) return -1;

    clip_image_task_owner *owner=(clip_image_task_owner*)calloc(1,sizeof(*owner));
    if (!owner) return 0;
    if (!image_work_task_begin(&owner->task))
        return image_work_task_finish(owner,0)?0:-1;
    *owner_out=owner;
    if (!hop_xfer_task_reserve_image_temp(&owner->task,(uint64_t)bytes)) {
        hop_log(HOP_LOG_WARN,"clip: DIB snapshot memory budget exhausted (task=%llu)",
                (unsigned long long)owner->task.id);
        return 0;
    }
    return 1;
}
static int image_snapshot_should_copy(int prepared) { return prepared==1; }
static int image_worker_should_retry(int result, int task_finished,
                                    int same_sequence, int stopping) {
    return !stopping && ((result==0 && task_finished) || !same_sequence);
}

static int image_work_task_finish_attempt(clip_image_task_owner *owner, int success) {
    if (!owner) return 1;
    hop_xfer_task *task=&owner->task;
    if (!task->initialized && !task->id) return 1;
    if (!task->initialized) return 0; /* keep an uninitialized-but-live handle */
    uint64_t temp_bytes=hop_xfer_task_image_temp_bytes(task);
    if (temp_bytes && !hop_xfer_task_release_image_temp(task, temp_bytes)) {
        hop_log(HOP_LOG_ERROR, "clip: image temporary budget release failed (task=%llu)",
                (unsigned long long)task->id);
        return 0;
    }
    if (!hop_xfer_task_is_terminal(task) &&
        !hop_xfer_task_finish(task, success ? HOP_XFER_TASK_COMPLETED : HOP_XFER_TASK_FAILED))
        return 0;
    hop_xfer_task_dispose(task);
    return !task->initialized && !task->id;
}

static int image_work_task_finish(clip_image_task_owner *owner, int success) {
    if (!owner) return 1;
    if (image_work_task_finish_attempt(owner, success)) { free(owner); return 1; }
    owner->next=s_pending_image_tasks;
    s_pending_image_tasks=owner;
    return 0;
}

static int image_work_retry_pending(void) {
    while (s_pending_image_tasks) {
        clip_image_task_owner *owner=s_pending_image_tasks;
        if (!image_work_task_finish_attempt(owner, 0)) return 0;
        s_pending_image_tasks=owner->next;
        free(owner);
    }
    return 1;
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
        hop_xfer_budget_snapshot budget;
        hop_xfer_budget_get_snapshot(&budget);
        uint64_t input=task?hop_xfer_task_image_temp_bytes(task):0;
        int retryable=task && hop_xfer_budget_image_conversion_fits(input,(uint64_t)total,&budget.limits);
        hop_log(HOP_LOG_WARN, "clip: DIB conversion memory budget exhausted");
        return retryable?0:-1;
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
    int handed_off = 0;
    if (GetClipboardSequenceNumber() == sequence) {
        uint8_t *owned_pixels = dst;
        dst = NULL; /* API consumes both the allocation and its temp reservation */
        handed_off = 1;
        ok = xfer_submit_image_owned_from_task(owned_pixels, w, hgt, (int)stride,
                                               task, (uint64_t)total);
    }
    if (dst) free(dst);
    if (!handed_off && !hop_xfer_task_release_image_temp(task, (uint64_t)total)) {
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
    clip_image_task_owner *image_task_owner = NULL;
    int opened = 0, result = 0, snapshot_ready = 1;
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
                    int prepared=image_snapshot_prepare((size_t)bytes,&image_task_owner);
                    if(prepared!=1){result=prepared;snapshot_ready=0;}
                }
                if (result != -1 && image_snapshot_should_copy(snapshot_ready)) {
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
        } else result = send_dib(copy, sequence, image_task_owner ? &image_task_owner->task : NULL);
        GlobalFree(copy);
    }
    int task_finished=!image_task_owner || image_work_task_finish(image_task_owner, result > 0);
    InterlockedExchange(&s_image_busy, 0);
    AcquireSRWLockExclusive(&s_offer_lock);
    if (image_worker_should_retry(result,task_finished,
            GetClipboardSequenceNumber()==sequence,s_clip_stopping))
        PostMessageW(g_app.hwnd, WM_APP_CLIP, 0, 0);
    ReleaseSRWLockExclusive(&s_offer_lock);
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
        clip_retire_offers();
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
    clip_consume_offers();
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
    clip_consume_offers();

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
    clip_consume_offers();
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
#include "clipboard_paste.inc"
