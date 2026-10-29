/* =====================================================================
 *  HopDesk - Windows 剪贴板同步（阶段 3，仅文本）
 *
 *  AddClipboardFormatListener + WM_CLIPBOARDUPDATE：
 *    本机复制 -> 读 CF_UNICODETEXT -> UTF-8 -> CLIP_TEXT(origin=SERVER)
 *    收到对端  -> 写 CF_UNICODETEXT，并抑制由此产生的本机广播
 *
 *  回环抑制两道：
 *    1) 协议层 origin 字段（跨设备）：收到自己发的不再写回
 *    2) 本进程 s_suppress 计数（本机）：程序性写入触发的那一次更新不广播
 *  s_suppress 只在主线程读写（窗口消息 + on_recv 都在主线程），无竞态。
 * ===================================================================== */
#include "app.h"

#include <stdlib.h>
#include <string.h>

static int s_suppress;

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
    if (!AddClipboardFormatListener(hwnd))
        hop_log(HOP_LOG_ERROR, "AddClipboardFormatListener failed: %lu", GetLastError());
    else
        hop_log(HOP_LOG_INFO, "clipboard listener installed");
}

void clip_shutdown(HWND hwnd) {
    RemoveClipboardFormatListener(hwnd);
}

static void send_text(const char *utf8, uint32_t len) {
    uint8_t *p = (uint8_t *)malloc(4 + len);
    if (!p) return;
    hop_put_be32(p, HOP_DEV_SERVER);
    memcpy(p + 4, utf8, len);
    net_server_send(g_app.net, HOP_MSG_CLIP_TEXT, p, 4 + len);
    free(p);
}

/* WM_CLIPBOARDUPDATE -> 广播本机文本 */
void clip_on_update(void) {
    HANDLE h;
    const wchar_t *w;
    int n;

    if (s_suppress) { s_suppress = 0; return; }   /* 是我们自己写的 */

    if (!open_retry(g_app.hwnd)) {
        hop_log(HOP_LOG_WARN, "clipboard busy, update skipped");
        return;
    }
    h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return; }          /* 非文本内容：阶段 3 不管 */

    w = (const wchar_t *)GlobalLock(h);
    if (!w) { CloseClipboard(); return; }

    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 1 && n <= HOP_TEXT_MAX) {
        char *u = (char *)malloc((size_t)n);
        if (u) {
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL) > 0) {
                hop_log(HOP_LOG_INFO, "send utf8 bytes: %02x %02x %02x",
                        (unsigned char)u[0], (unsigned char)u[1],
                        (unsigned char)u[2]);
                send_text(u, (uint32_t)(n - 1));   /* 不含结尾 NUL */
            }
            free(u);
        }
    } else if (n > HOP_TEXT_MAX) {
        hop_log(HOP_LOG_WARN, "clipboard text %d bytes > limit, skipped", n);
    }

    GlobalUnlock(h);
    CloseClipboard();
}

/* 收到对端文本：payload = origin(4) + UTF-8 */
void clip_apply_remote(const uint8_t *p, uint32_t len) {
    char *u;
    int wn;
    HGLOBAL h;

    if (len < 5) return;
    if (hop_get_be32(p) == HOP_DEV_SERVER) return;   /* 自己发的，回环 */

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

    if (open_retry(g_app.hwnd)) {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h)) {
            s_suppress = 1;      /* 所有权归系统，不再 GlobalFree */
            h = NULL;
        }
        CloseClipboard();
    }
    if (h) GlobalFree(h);
    free(u);
}
