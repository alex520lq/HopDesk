/* =====================================================================
 *  HopDesk - Windows 剪贴板同步（阶段 3：文本 + 单文件）
 *
 *  AddClipboardFormatListener + WM_CLIPBOARDUPDATE：
 *    本机复制 -> CF_HDROP（文件优先）或 CF_UNICODETEXT -> 发送
 *    收到对端 -> 写回剪贴板，并抑制由此产生的本机广播
 *
 *  为什么文件优先：资源管理器里 Ctrl+C 一个文件，剪贴板里放的是
 *  CF_HDROP，不是 CF_UNICODETEXT。只读文本的话，复制文件会"什么都不发生"。
 *
 *  文件传输的限制（有意为之，不是待办）：
 *    - 只传第一个文件，多选时日志里说明
 *    - 单文件 ≤ HOP_FILE_MAX（8 MB），一帧传完，不做分块
 *  这两条够用：这个工具的目标是"跳一下就过去"，不是替代 AirDrop。
 *
 *  回环抑制两道：
 *    1) 协议层 origin 字段（跨设备）：收到自己发的不再写回
 *    2) 本进程 s_suppress 计数（本机）：程序性写入触发的那一次更新不广播
 *  s_suppress 只在主线程读写（窗口消息 + on_recv 都在主线程），无竞态。
 * ===================================================================== */
#include "app.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int s_suppress;

/* DROPFILES 在 MSVC 的 shellapi.h 里，MinGW 的 shellapi.h 没有（在 shlobj.h）。
 * 自己定义，省掉一个头文件版本差异 —— 布局是 ABI 固定的 20 字节。 */
typedef struct {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} HOP_DROPFILES;

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

/* ---- 发送 ---- */

static void send_text(const char *utf8, uint32_t len) {
    uint8_t *p = (uint8_t *)malloc(4 + len);
    if (!p) return;
    hop_put_be32(p, HOP_DEV_SERVER);
    memcpy(p + 4, utf8, len);
    if (net_server_send(g_app.net, HOP_MSG_CLIP_TEXT, p, 4 + len))
        hop_log(HOP_LOG_INFO, "clip: text out %u bytes", len);
    else
        hop_log(HOP_LOG_WARN, "clip: text out failed (no client?)");
    free(p);
}

/* payload = origin(4) + nlen(2) + name + size(4) + data */
static void send_file(const wchar_t *path) {
    FILE *f;
    long size;
    const wchar_t *base, *p;
    char name[256];
    uint32_t nlen;
    uint8_t *p8;
    size_t total;
    int need;
    DWORD attr;

    /* Windows 路径两种分隔符都合法，只认反斜杠会把整个路径当文件名 */
    base = path;
    for (p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;

    attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        hop_log(HOP_LOG_WARN, "clip: folder copy not supported, skipped");
        return;
    }

    need = WideCharToMultiByte(CP_UTF8, 0, base, -1, NULL, 0, NULL, NULL);
    if (need <= 1 || need > (int)sizeof(name)) {
        hop_log(HOP_LOG_WARN, "clip: file name too long, skipped");
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, (int)sizeof(name), NULL, NULL);
    nlen = (uint32_t)strlen(name);
    if (nlen == 0 || nlen > HOP_NAME_LEN_MAX) return;

    f = _wfopen(path, L"rb");
    if (!f) { hop_log(HOP_LOG_WARN, "clip: cannot open file"); return; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > HOP_FILE_MAX) {
        hop_log(HOP_LOG_WARN, "clip: file %ld bytes out of range, skipped", size);
        fclose(f);
        return;
    }

    total = 4 + 2 + nlen + 4 + (size_t)size;
    p8 = (uint8_t *)malloc(total);
    if (!p8) { fclose(f); return; }

    hop_put_be32(p8, HOP_DEV_SERVER);
    hop_put_be16(p8 + 4, (uint16_t)nlen);
    memcpy(p8 + 6, name, nlen);
    hop_put_be32(p8 + 6 + nlen, (uint32_t)size);

    if (fread(p8 + 10 + nlen, 1, (size_t)size, f) != (size_t)size) {
        hop_log(HOP_LOG_WARN, "clip: file read short, aborted");
        fclose(f);
        free(p8);
        return;
    }
    fclose(f);

    if (net_server_send(g_app.net, HOP_MSG_CLIP_FILE, p8, (uint32_t)total))
        hop_log(HOP_LOG_INFO, "clip: file out %s (%ld bytes)", name, size);
    else
        hop_log(HOP_LOG_WARN, "clip: file out failed (no client?)");
    free(p8);
}

/* WM_CLIPBOARDUPDATE -> 广播本机内容 */
void clip_on_update(void) {
    HANDLE h;

    if (s_suppress) { s_suppress = 0; return; }   /* 是我们自己写的 */

    if (!open_retry(g_app.hwnd)) {
        hop_log(HOP_LOG_WARN, "clip: clipboard busy, update skipped");
        return;
    }

    /* 文件优先：复制文件时也可能同时带一段文本（文件名），但用户要的是文件 */
    h = GetClipboardData(CF_HDROP);
    if (h) {
        HDROP drop = (HDROP)h;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        if (count > 0) {
            wchar_t path[MAX_PATH];
            if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
                if (count > 1)
                    hop_log(HOP_LOG_WARN, "clip: %u files copied, only the first is sent", count);
                send_file(path);
                CloseClipboard();
                return;
            }
        }
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
                        send_text(u, (uint32_t)(n - 1));   /* 不含结尾 NUL */
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
            hop_log(HOP_LOG_INFO, "clip: text in %u bytes", len - 4);
        }
        CloseClipboard();
    }
    if (h) GlobalFree(h);
    free(u);
}

/* 文件名只允许普通字符：对端是可信的，但越界写入不该由"信任"来兜底 */
static int name_safe(const char *n) {
    if (!n || !*n) return 0;
    if (strstr(n, "..")) return 0;
    for (; *n; ++n)
        if (*n == '/' || *n == '\\' || *n == ':') return 0;
    return 1;
}

/* 把收到的文件落到 %TEMP%\HopDesk\ 并挂到剪贴板，用户可直接 Ctrl+V 到资源管理器 */
static void write_file_and_set_clip(const char *name, const uint8_t *data, uint32_t size) {
    wchar_t base[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    wchar_t wname[MAX_PATH];
    HOP_DROPFILES *df;
    HGLOBAL h;
    FILE *f;
    SIZE_T bytes;

    if (GetTempPathW(MAX_PATH, base) == 0) return;
    if (wcslen(base) + 16 >= MAX_PATH) return;
    wcscpy(dir, base);
    wcscat(dir, L"HopDesk");
    CreateDirectoryW(dir, NULL);

    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH) <= 0) return;
    if (wcslen(dir) + wcslen(wname) + 2 >= MAX_PATH) return;
    wcscpy(path, dir);
    wcscat(path, L"\\");
    wcscat(path, wname);

    f = _wfopen(path, L"wb");
    if (!f) { hop_log(HOP_LOG_WARN, "clip: cannot write temp file"); return; }
    if (fwrite(data, 1, size, f) != size) {
        hop_log(HOP_LOG_WARN, "clip: temp file write short");
        fclose(f);
        return;
    }
    fclose(f);

    bytes = sizeof(HOP_DROPFILES) + (wcslen(path) + 2) * sizeof(wchar_t);
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return;
    df = (HOP_DROPFILES *)GlobalLock(h);
    if (!df) { GlobalFree(h); return; }
    df->pFiles = sizeof(HOP_DROPFILES);
    df->fWide = TRUE;
    memcpy((BYTE *)df + sizeof(HOP_DROPFILES), path, wcslen(path) * sizeof(wchar_t));
    GlobalUnlock(h);

    if (open_retry(g_app.hwnd)) {
        if (EmptyClipboard() && SetClipboardData(CF_HDROP, h)) {
            s_suppress = 1;
            hop_log(HOP_LOG_INFO, "clip: file in %s (%u bytes)", name, size);
            CloseClipboard();
            return;
        }
        CloseClipboard();
    }
    GlobalFree(h);
    hop_log(HOP_LOG_WARN, "clip: failed to put received file on clipboard");
}

/* payload = origin(4) + nlen(2) + name + size(4) + data */
void clip_apply_remote_file(const uint8_t *p, uint32_t len) {
    uint16_t nlen;
    uint32_t size;
    char name[256];

    if (len < 10) return;
    if (hop_get_be32(p) == HOP_DEV_SERVER) return;

    nlen = hop_get_be16(p + 4);
    if (nlen == 0 || nlen > HOP_NAME_LEN_MAX) return;
    if (10u + nlen > len) return;

    size = hop_get_be32(p + 6 + nlen);
    if (size == 0 || size > HOP_FILE_MAX) return;
    if (10u + (uint32_t)nlen + size > len) return;

    memcpy(name, p + 6, nlen);
    name[nlen] = '\0';
    if (!name_safe(name)) {
        hop_log(HOP_LOG_WARN, "clip: unsafe file name rejected");
        return;
    }

    write_file_and_set_clip(name, p + 10 + nlen, size);
}
