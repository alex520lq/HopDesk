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
 *    2) 本进程 s_suppress 计数（本机）：程序性写入触发的那一次更新不广播
 *  s_suppress 只在主线程读写（窗口消息 + on_recv 都在主线程），无竞态。
 * ===================================================================== */
#include "app.h"
#include "xfer.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* DROPFILES 在 MSVC 的 shellapi.h 里，MinGW 的 shellapi.h 没有（在 shlobj.h）。
 * 自己定义，省掉一个头文件版本差异 —— 布局是 ABI 固定的 20 字节。 */
typedef struct {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} HOP_DROPFILES;

/* 程序性写入后的抑制窗口。
 *
 * EmptyClipboard + SetClipboardData 会触发**两次** WM_CLIPBOARDUPDATE。
 * 原来用 s_suppress 计数只挡一次，第二次通知就被当成用户复制广播出去，
 * 两端互相回环 —— 日志里 clip: text in/out 刷屏，文件剪贴板被文本冲掉，
 * 用户按粘贴什么也拿不到。用时间窗把整串通知一起吃掉。 */
static int   s_suppress;
static DWORD s_suppress_until;
#define CLIP_SUPPRESS_MS 400

/* Latest remote clipboard payload survives temporary clipboard contention.
 * Ownership of HGLOBAL transfers here; Windows owns it only after SetClipboardData. */
static HGLOBAL s_pending_global;
static UINT s_pending_format;

static void schedule_clip_retry(void) {
    if (g_app.hwnd) SetTimer(g_app.hwnd, IDT_CLIP_RETRY, 120, NULL);
}

static int apply_global(UINT format, HGLOBAL h) {
    if (!h) return 0;
    if (open_retry(g_app.hwnd)) {
        if (EmptyClipboard() && SetClipboardData(format, h)) {
            s_suppress = 1;
            s_suppress_until = GetTickCount() + CLIP_SUPPRESS_MS;
            CloseClipboard();
            return 1;
        }
        CloseClipboard();
    }
    if (s_pending_global) GlobalFree(s_pending_global);
    s_pending_global = h;
    s_pending_format = format;
    schedule_clip_retry();
    hop_log(HOP_LOG_WARN, "clip: clipboard busy; queued remote format %u for retry", format);
    return 0;
}

void clip_retry_pending(void) {
    if (s_pending_global) {
        if (open_retry(g_app.hwnd)) {
            if (EmptyClipboard() && SetClipboardData(s_pending_format, s_pending_global)) {
                s_pending_global = NULL; /* ownership transferred to Windows */
                s_suppress = 1;
                s_suppress_until = GetTickCount() + CLIP_SUPPRESS_MS;
                hop_log(HOP_LOG_INFO, "clip: pending remote clipboard applied");
                CloseClipboard();
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
    int i;
    for (i = 0; i < 6; ++i) {
        if (OpenClipboard(hwnd)) return 1;
        Sleep(15);
    }
    return 0;
}

void clip_init(HWND hwnd) {
    s_suppress = 0;
    s_suppress_until = 0;
    if (!AddClipboardFormatListener(hwnd))
        hop_log(HOP_LOG_ERROR, "AddClipboardFormatListener failed: %lu", GetLastError());
    else
        hop_log(HOP_LOG_INFO, "clipboard listener installed");
}

void clip_shutdown(HWND hwnd) {
    RemoveClipboardFormatListener(hwnd);
    KillTimer(hwnd, IDT_CLIP_RETRY);
    if (s_pending_global) { GlobalFree(s_pending_global); s_pending_global = NULL; }
}

/* ---- 发送 ---- */

static int send_text(const char *utf8, uint32_t len) {
    uint8_t *p = (uint8_t *)malloc(4 + len);
    if (!p) return 0;
    hop_put_be32(p, HOP_DEV_SERVER);
    memcpy(p + 4, utf8, len);
    int ok = net_server_send(g_app.net, HOP_MSG_CLIP_TEXT, p, 4 + len);
    if (ok) hop_log(HOP_LOG_INFO, "clip: text out %u bytes", len);
    else hop_log(HOP_LOG_WARN, "clip: text out failed; will retry");
    free(p);
    return ok;
}

/* CF_HDROP：全部条目一次性交给数据通道，目录由 xfer 递归展开 */
static int send_drop(HDROP drop, UINT count) {
    wchar_t **paths = (wchar_t **)calloc(count, sizeof(wchar_t *));
    if (!paths) return 0;

    int n = 0;
    for (UINT i = 0; i < count && n < 256; ++i) {
        UINT need = DragQueryFileW(drop, i, NULL, 0);
        if (need == 0 || need >= 32768) continue;
        wchar_t *buf = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
        if (!buf) continue;
        if (DragQueryFileW(drop, i, buf, need + 1) == 0) { free(buf); continue; }
        paths[n++] = buf;
    }

    int ok = 0;
    if (n > 0) {
        char first_utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, paths[0], -1,
                            first_utf8, sizeof(first_utf8), NULL, NULL);
        hop_log(HOP_LOG_INFO, "clip: %d path(s) out (first: %s)", n, first_utf8);
        ok = xfer_submit_paths((const wchar_t *const *)paths, n);
        if (!ok) hop_log(HOP_LOG_WARN, "clip: paths not queued; will retry");
    }

    for (int i = 0; i < n; ++i) free(paths[i]);
    free(paths);
    return ok;
}

/* CF_DIB -> 自上而下 BGRA。只处理 BI_RGB 的 24/32 位，其余（压缩格式）跳过。 */
static int send_dib(HANDLE h) {
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)GlobalLock(h);
    if (!bi) return 0;

    int w = (int)bi->biWidth;
    int hgt = (int)bi->biHeight;
    int bpp = (int)bi->biBitCount;
    int topdown = 0;

    if (hgt < 0) { topdown = 1; hgt = -hgt; }
    if (w <= 0 || hgt <= 0 || bi->biCompression != BI_RGB || (bpp != 24 && bpp != 32)) {
        hop_log(HOP_LOG_INFO, "clip: dib %dx%d %dbpp comp %lu unsupported, skipped",
                w, hgt, bpp, (unsigned long)bi->biCompression);
        GlobalUnlock(h);
        return 0;
    }

    size_t stride = (size_t)w * 4;
    size_t total = stride * (size_t)hgt;
    if (total > CLIP_IMG_MAX_BYTES) {
        hop_log(HOP_LOG_WARN, "clip: image %.1f MB too large, skipped",
                (double)total / 1048576.0);
        GlobalUnlock(h);
        return 0;
    }

    const uint8_t *src = (const uint8_t *)bi + bi->biSize + bi->biClrUsed * 4;
    uint8_t *dst = (uint8_t *)malloc(total);
    if (!dst) { GlobalUnlock(h); return 0; }

    size_t src_stride = (((size_t)w * bpp + 31) / 32) * 4;
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
    hop_log(HOP_LOG_INFO, "clip: image out %dx%d", w, hgt);
    int ok = xfer_submit_image(dst, w, hgt, (int)stride);
    free(dst);
    if (!ok) hop_log(HOP_LOG_WARN, "clip: image not queued; will retry");
    return ok;
}

/* WM_CLIPBOARDUPDATE -> 广播本机内容 */
void clip_on_update(void) {
    HANDLE h;

    /* 抑制窗口内的所有通知都吃掉。
     * EmptyClipboard + SetClipboardData 会各触发一次，只挡一次挡不住。 */
    if (s_suppress || (s_suppress_until &&
                       (LONG)(GetTickCount() - s_suppress_until) < 0)) {
        s_suppress = 0;
        return;
    }
    s_suppress_until = 0;

    if (!open_retry(g_app.hwnd)) {
        hop_log(HOP_LOG_WARN, "clip: clipboard busy, scheduling retry");
        schedule_clip_retry();
        return;
    }

    /* 文件优先：复制文件时剪贴板里也可能带文件名文本，但用户要的是文件。
     * 从浏览器"复制图片"常常同时给 CF_HDROP（临时文件）和 CF_DIB，
     * 发文件更忠实原样。 */
    h = GetClipboardData(CF_HDROP);
    if (h) {
        HDROP drop = (HDROP)h;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        if (count > 0) {
            int ok = send_drop(drop, count);
            CloseClipboard();
            if (!ok) schedule_clip_retry();
            return;
        }
    }

    h = GetClipboardData(CF_DIB);
    if (h) {
        int ok = send_dib(h);
        CloseClipboard();
        if (!ok) schedule_clip_retry();
        return;
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
    hop_log(HOP_LOG_INFO, "clip: unsupported clipboard format, ignored");
}

/* ---- 接收 ---- */

/* 收到对端文本：payload = origin(4) + UTF-8 */
void clip_apply_remote(const uint8_t *p, uint32_t len) {
    char *u;
    int wn;
    HGLOBAL h;

    if (len < 5) return;
    if (!hop_clip_should_apply_remote(hop_get_be32(p), HOP_DEV_SERVER)) return; /* 自己发的，回环 */

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
        hop_log(HOP_LOG_INFO, "clip: text in %u bytes", len - 4);
    } else {
        h = NULL; /* apply_global retained ownership for retry */
    }
    if (h) GlobalFree(h);
    free(u);
}

/* 一批文件/目录落盘完毕：把顶层条目挂到剪贴板，用户直接 Ctrl+V */
void clip_apply_remote_paths(const wchar_t *const *paths, int n) {
    HOP_DROPFILES *df;
    HGLOBAL h;
    SIZE_T bytes;
    SIZE_T off;

    if (!paths || n <= 0) return;

    bytes = sizeof(HOP_DROPFILES);
    for (int i = 0; i < n; ++i) bytes += (wcslen(paths[i]) + 1) * sizeof(wchar_t);
    bytes += sizeof(wchar_t);        /* 双 NUL 结尾 */

    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return;
    df = (HOP_DROPFILES *)GlobalLock(h);
    if (!df) { GlobalFree(h); return; }

    df->pFiles = sizeof(HOP_DROPFILES);
    df->fWide = TRUE;

    off = sizeof(HOP_DROPFILES);
    for (int i = 0; i < n; ++i) {
        SIZE_T cb = wcslen(paths[i]) * sizeof(wchar_t);
        memcpy((BYTE *)df + off, paths[i], cb);
        off += cb + sizeof(wchar_t);      /* 逐条以 NUL 分隔 */
    }
    GlobalUnlock(h);

    if (apply_global(CF_HDROP, h)) {
        hop_log(HOP_LOG_INFO, "clip: %d path(s) in (first: %ls)", n, paths[0]);
    } else {
        hop_log(HOP_LOG_WARN, "clip: received paths queued for clipboard retry");
    }
}

/* 收到图片：裸 BGRA 自上而下 -> CF_DIB（32 位，自下而上，BI_RGB） */
void clip_apply_remote_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return;

    size_t need = sizeof(BITMAPINFOHEADER) + (size_t)stride * (size_t)h;
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
    bi->biSizeImage = (DWORD)((size_t)stride * (size_t)h);

    uint8_t *dst = base + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; ++y) {
        memcpy(dst + (size_t)y * stride,
               bgra + (size_t)(h - 1 - y) * stride, (size_t)stride);
    }
    GlobalUnlock(gh);

    if (apply_global(CF_DIB, gh)) {
        hop_log(HOP_LOG_INFO, "clip: image in %dx%d", w, h);
    } else {
        hop_log(HOP_LOG_WARN, "clip: received image queued for clipboard retry");
    }
}
