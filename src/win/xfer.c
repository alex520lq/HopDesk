/* =====================================================================
 *  HopDesk - Windows 文件/图片数据通道实现
 * ===================================================================== */
/* winsock2 必须在 windows.h 之前 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include "app.h"
#include "xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define XFER_MAX_ENTRIES  8192     /* 一次提交的条目上限，防跑飞 */
#define XFER_RX_ROOT      L"HopDesk"

/* ---------------- 条目 / 作业 ---------------- */

typedef struct {
    wchar_t *full;      /* 本机绝对路径（目录条目也用它建目录） */
    char    *name;      /* UTF-8 文件名 */
    char    *rel;       /* UTF-8 相对路径，用 '/' 分隔 */
    uint64_t size;
    uint64_t mtime;     /* Unix 秒 */
    uint32_t flags;
} xfer_entry;

typedef struct xfer_job {
    struct xfer_job *next;
    uint32_t    batch;
    int         is_image;
    int         n;              /* JOB_FILES 的条目数 */
    xfer_entry *e;
    uint8_t    *pixels;         /* JOB_IMAGE */
    int         w, h, stride;
} xfer_job;

typedef struct {
    xfer_entry *v;
    int n, cap;
} elist;

/* ---------------- 全局状态 ---------------- */

static struct {
    HWND             hwnd;
    hop_config       cfg;
    SOCKET           listen_s;
    SOCKET           client;
    CRITICAL_SECTION lock;      /* 保护 client + 作业队列 */
    HANDLE           stop;
    HANDLE           q_evt;
    HANDLE           th_listen;
    HANDLE           th_send;
    xfer_job        *q_head, *q_tail;
    uint32_t         batch_seq;
    uint32_t         chunk;     /* 实际分块字节数 */
} s_x;

/* ---------------- UTF-8 转换 ---------------- */

static char *w2u(const wchar_t *w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return NULL;
    char *o = (char *)malloc((size_t)need);
    if (!o) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, o, need, NULL, NULL);
    return o;
}

/* 返回 malloc 的宽字符串；非法序列返回 NULL */
static wchar_t *u2w(const char *u, int len) {
    int need = MultiByteToWideChar(CP_UTF8, 0, u, len, NULL, 0);
    if (need <= 0) return NULL;
    wchar_t *o = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!o) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, u, len, o, need);
    o[need] = L'\0';
    return o;
}

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = p;
    for (const wchar_t *q = p; *q; ++q)
        if (*q == L'\\' || *q == L'/') b = q + 1;
    return b;
}

/* 相对路径安全校验。
 * 对端是可信的，但"越界写入"不该由"信任"兜底 —— 一条 ../.. 就能写到
 * 临时目录外面去。规则：不许绝对路径、不许盘符、不许 .. 作为完整分量
 * （"a..b" 是合法文件名，不能一并拒掉）。 */
static int rel_safe(const char *rel) {
    if (!rel || !*rel) return 0;
    if (rel[0] == '/' || rel[0] == '\\') return 0;
    if (strchr(rel, ':')) return 0;

    const char *p = rel;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        size_t n = (size_t)(p - seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (n == 0) return 0;                 /* 连续的 / 或 \\，异常 */
        if (*p) ++p;
    }
    return 1;
}

/* FILETIME -> Unix 秒 */
static uint64_t ft_to_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (u.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

/* ---------------- 条目列表 ---------------- */

static int elist_push(elist *l, xfer_entry *e) {
    if (l->n >= XFER_MAX_ENTRIES) return 0;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        xfer_entry *nv = (xfer_entry *)realloc(l->v, (size_t)nc * sizeof(*nv));
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = *e;
    return 1;
}

static void elist_free(elist *l) {
    for (int i = 0; i < l->n; ++i) {
        free(l->v[i].full);
        free(l->v[i].name);
        free(l->v[i].rel);
    }
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* 递归收集。rel 是 UTF-8 相对路径（不含首尾斜杠）。 */
static void walk_into(elist *l, const wchar_t *full, const char *rel) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) return;

    int is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const wchar_t *bn = base_name(full);

    xfer_entry e;
    memset(&e, 0, sizeof(e));
    e.full  = _wcsdup(full);
    e.name  = w2u(bn);
    e.rel   = rel ? _strdup(rel) : NULL;
    e.mtime = ft_to_unix(&fad.ftLastWriteTime);

    if (!e.full || !e.name || !e.rel) {
        free(e.full); free(e.name); free(e.rel);
        return;
    }

    if (!is_dir) {
        e.size  = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        e.flags = 0;
        if (!elist_push(l, &e)) {
            free(e.full); free(e.name); free(e.rel);
        }
        return;
    }

    /* 目录：先登记自身（保住空目录），再递归子项 */
    e.size  = 0;
    e.flags = HOP_FILE_FLAG_DIR;
    if (!elist_push(l, &e)) {
        free(e.full); free(e.name); free(e.rel);
        return;
    }

    size_t dl = wcslen(full);
    wchar_t *pat = (wchar_t *)malloc((dl + 3) * sizeof(wchar_t));
    if (!pat) return;
    wcscpy(pat, full);
    if (dl && pat[dl - 1] != L'\\' && pat[dl - 1] != L'/') wcscat(pat, L"\\");
    wcscat(pat, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;

        size_t fl = wcslen(full);
        int need_sep = (fl && full[fl - 1] != L'\\' && full[fl - 1] != L'/');
        wchar_t *child = (wchar_t *)malloc((fl + wcslen(fd.cFileName) + 3) * sizeof(wchar_t));
        if (!child) continue;
        wcscpy(child, full);
        if (need_sep) wcscat(child, L"\\");
        wcscat(child, fd.cFileName);

        /* 子项相对路径 = 父相对路径 + '/' + 文件名 */
        char *cname = w2u(fd.cFileName);
        char *crel = NULL;
        if (cname) {
            size_t rl = strlen(rel), cl = strlen(cname);
            crel = (char *)malloc(rl + cl + 2);
            if (crel) {
                memcpy(crel, rel, rl);
                crel[rl] = '/';
                memcpy(crel + rl + 1, cname, cl + 1);
            }
            free(cname);
        }

        if (crel) walk_into(l, child, crel);
        free(crel);
        free(child);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* ---------------- 发送 ---------------- */

static int send_all(SOCKET c, const char *p, int len) {
    while (len > 0) {
        int n = send(c, p, len, 0);
        if (n <= 0) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

/* 每块之后让一次 CPU。传输是后台工作，键鼠才是前台 ——
 * 真正起作用的保障是 send_thread 里的 BELOW_NORMAL 优先级（输入线程
 * 是 NORMAL，会直接抢占）；这里让一次是给同优先级的其他工作减压。 */
static void yield_cpu(void) { Sleep(0); }

/* 小帧：头 + 载荷两次 send。载荷都在 1 KB 级别，不值得拼缓冲。 */
static int send_small(SOCKET c, uint8_t type, const uint8_t *p, uint32_t len) {
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);
    if (!send_all(c, (const char *)hdr, HOP_HEADER_SIZE)) return 0;
    if (len && !send_all(c, (const char *)p, (int)len)) return 0;
    return 1;
}

/* 分块：一次 send 发完头 + 分块头 + 数据。
 * 数据直接读进发送缓冲的第 21 字节处，省掉一次内存拷贝。 */
#define CHUNK_HDR 21u   /* 5 帧头 + 4 id + 8 off + 4 len */

static int send_chunk(SOCKET c, uint8_t *tx, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len) {
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 16 + len);
    hop_put_be32(tx + 5, id);
    hop_put_be64(tx + 9, off);
    hop_put_be32(tx + 17, len);
    if (data != tx + CHUNK_HDR) memcpy(tx + CHUNK_HDR, data, len);
    return send_all(c, (const char *)tx, (int)(CHUNK_HDR + len));
}

static int send_file_entry(SOCKET c, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xfer_entry *e) {
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);

    /* BEGIN: batch(4)+id(4)+size(8)+mtime(8)+flags(1)+nlen(2)+name+plen(2)+rel */
    uint32_t blen = 4 + 4 + 8 + 8 + 1 + 2 + nlen + 2 + plen;
    uint8_t *bp = (uint8_t *)malloc(blen);
    if (!bp) return 0;
    uint32_t o = 0;
    hop_put_be32(bp + o, batch); o += 4;
    hop_put_be32(bp + o, id);    o += 4;
    hop_put_be64(bp + o, e->size); o += 8;
    hop_put_be64(bp + o, e->mtime); o += 8;
    bp[o++] = (uint8_t)e->flags;
    hop_put_be16(bp + o, (uint16_t)nlen); o += 2;
    memcpy(bp + o, e->name, nlen); o += nlen;
    hop_put_be16(bp + o, (uint16_t)plen); o += 2;
    memcpy(bp + o, e->rel, plen); o += plen;

    int ok = send_small(c, HOP_MSG_FILE_BEGIN, bp, blen);
    free(bp);
    if (!ok) return 0;

    if (!(e->flags & HOP_FILE_FLAG_DIR) && e->size > 0) {
        HANDLE h = CreateFileW(e->full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, skipping %s (err %lu)",
                    e->rel, GetLastError());
            uint8_t endp[5];
            hop_put_be32(endp, id);
            endp[4] = 1;                      /* status = 失败 */
            send_small(c, HOP_MSG_FILE_END, endp, 5);
            return 1;                          /* 单个文件失败不终止整批 */
        }

        uint64_t off = 0;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, tx + CHUNK_HDR, (DWORD)s_x.chunk, &got, NULL) || got == 0) break;
            if (!send_chunk(c, tx, id, off, tx + CHUNK_HDR, got)) {
                CloseHandle(h);
                return 0;
            }
            off += got;
            yield_cpu();
        }
        CloseHandle(h);
    }

    uint8_t endp[5];
    hop_put_be32(endp, id);
    endp[4] = 0;
    return send_small(c, HOP_MSG_FILE_END, endp, 5);
}

static int send_image_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    uint8_t bp[20];
    hop_put_be32(bp, j->batch);
    hop_put_be32(bp + 4, 1);
    hop_put_be32(bp + 8, (uint32_t)j->w);
    hop_put_be32(bp + 12, (uint32_t)j->h);
    hop_put_be32(bp + 16, (uint32_t)j->stride);
    if (!send_small(c, HOP_MSG_IMAGE_BEGIN, bp, 20)) return 0;

    uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
    uint64_t off = 0;
    while (off < total) {
        uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
        if (!send_chunk(c, tx, 1, off, j->pixels + off, n)) return 0;
        off += n;
        yield_cpu();
    }

    uint8_t endp[5];
    hop_put_be32(endp, 1);
    endp[4] = 0;
    return send_small(c, HOP_MSG_FILE_END, endp, 5);
}

/* 实际发送（send 线程持有 tx 缓冲） */
static int run_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    if (j->is_image) {
        if (!send_image_job(c, tx, j)) return 0;
    } else {
        for (int i = 0; i < j->n; ++i) {
            if (!send_file_entry(c, tx, j->batch, (uint32_t)(i + 1), &j->e[i]))
                return 0;
        }
    }
    uint8_t dp[16];
    hop_put_be32(dp, j->batch);
    hop_put_be32(dp + 4, (uint32_t)j->n);
    hop_put_be64(dp + 8, 0);
    return send_small(c, HOP_MSG_XFER_DONE, dp, 16);
}

static void job_free(xfer_job *j) {
    if (!j) return;
    for (int i = 0; i < j->n; ++i) {
        free(j->e[i].full);
        free(j->e[i].name);
        free(j->e[i].rel);
    }
    free(j->e);
    free(j->pixels);
    free(j);
}

static DWORD WINAPI send_thread(LPVOID arg) {
    (void)arg;

    /* 主动降到 BELOW_NORMAL。控制通道承载的是键鼠，钩子回调跑在主线程
     * （NORMAL 优先级）—— 降级之后输入路径永远能直接抢占传输线程，
     * 不会出现"传文件时鼠标发涩"。这是"传输不影响控制"的主要保障。 */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    uint8_t *tx = (uint8_t *)malloc(CHUNK_HDR + s_x.chunk);
    if (!tx) { hop_log(HOP_LOG_ERROR, "xfer: tx buffer alloc failed"); return 1; }

    HANDLE ev[2] = { s_x.stop, s_x.q_evt };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 500);
        if (w == WAIT_OBJECT_0) break;

        for (;;) {
            EnterCriticalSection(&s_x.lock);
            xfer_job *j = s_x.q_head;
            SOCKET c = s_x.client;
            if (j && c != INVALID_SOCKET) {
                s_x.q_head = j->next;
                if (!s_x.q_head) s_x.q_tail = NULL;
            } else {
                j = NULL;
            }
            LeaveCriticalSection(&s_x.lock);

            if (!j) break;

            if (!run_job(c, tx, j))
                hop_log(HOP_LOG_WARN, "xfer: send aborted (batch %u)", j->batch);
            job_free(j);
        }
    }
    free(tx);
    return 0;
}

/* ---------------- 接收 ---------------- */

typedef struct {
    uint32_t  batch;
    int       active;
    wchar_t   dir[MAX_PATH];
    HANDLE    f;
    uint32_t  cur_id;
    wchar_t  *cur_path;
    int       cur_is_image;
    /* 图片累积 */
    uint8_t  *img;
    size_t    img_cap, img_len;
    int       img_w, img_h, img_stride;
    /* 本批顶层条目（用于最后设置剪贴板） */
    wchar_t **tops;
    int       ntop;
} xfer_rx;

static xfer_rx s_rx;

static void rx_reset(void) {
    if (s_rx.f && s_rx.f != INVALID_HANDLE_VALUE) CloseHandle(s_rx.f);
    s_rx.f = INVALID_HANDLE_VALUE;
    free(s_rx.cur_path);
    s_rx.cur_path = NULL;
    free(s_rx.img);
    s_rx.img = NULL;
    s_rx.img_cap = s_rx.img_len = 0;
    for (int i = 0; i < s_rx.ntop; ++i) free(s_rx.tops[i]);
    free(s_rx.tops);
    s_rx.tops = NULL;
    s_rx.ntop = 0;
    s_rx.active = 0;
    s_rx.batch = 0;
}

/* 逐级创建目录。从盘符之后开始，避免对 "C:" 本身调 CreateDirectory。 */
static void ensure_dirs(wchar_t *path) {
    wchar_t *p = path;
    if (((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z')) && p[1] == L':')
        p += 2;
    for (; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t save = *p;
            *p = L'\0';
            CreateDirectoryW(path, NULL);
            *p = save;
        }
    }
}

static int rx_add_top(const wchar_t *p) {
    wchar_t **nv = (wchar_t **)realloc(s_rx.tops, (size_t)(s_rx.ntop + 1) * sizeof(wchar_t *));
    if (!nv) return 0;
    s_rx.tops = nv;
    s_rx.tops[s_rx.ntop] = _wcsdup(p);
    if (!s_rx.tops[s_rx.ntop]) return 0;
    s_rx.ntop++;
    return 1;
}

/* 把 UTF-8 相对路径拼到本批目录下，返回 malloc 的宽路径 */
static wchar_t *rx_path_for(const char *rel) {
    wchar_t *w = u2w(rel, (int)strlen(rel));
    if (!w) return NULL;
    for (wchar_t *p = w; *p; ++p) if (*p == L'/') *p = L'\\';

    size_t dl = wcslen(s_rx.dir), wl = wcslen(w);
    wchar_t *o = (wchar_t *)malloc((dl + wl + 2) * sizeof(wchar_t));
    if (!o) { free(w); return NULL; }
    wcscpy(o, s_rx.dir);
    if (dl && o[dl - 1] != L'\\') wcscat(o, L"\\");
    wcscat(o, w);
    free(w);
    return o;
}

static void rx_begin_batch(uint32_t batch) {
    if (s_rx.active && s_rx.batch == batch) return;
    rx_reset();

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return;

    /* 手工拼接并显式检查长度 —— wsprintfW 没有边界检查，临时目录路径
     * 一旦接近 MAX_PATH 就会直接越界写。
     * 注意：swprintf 的 %s 要的是【窄】字符串（%ls 才是宽的），
     * 用 %s 传 L"HopDesk" 只会读到第一个字符 'H'。这里不碰格式化，直接拼。 */
    wchar_t sub[32];
    wchar_t num[16];
    if (swprintf(num, 16, L"%u", batch) <= 0) return;
    wcscpy(sub, XFER_RX_ROOT);
    wcscat(sub, L"\\");
    wcscat(sub, num);

    size_t sub_len = wcslen(sub);
    if ((size_t)n + sub_len + 1 > MAX_PATH) {
        hop_log(HOP_LOG_WARN, "xfer: temp path too long, batch %u dropped", batch);
        return;
    }

    wcscpy(s_rx.dir, tmp);
    wcscat(s_rx.dir, sub);

    s_rx.active = 1;
    s_rx.batch = batch;
    s_rx.f = INVALID_HANDLE_VALUE;

    ensure_dirs(s_rx.dir);
    CreateDirectoryW(s_rx.dir, NULL);

    /* GetTempPathW 可能返回 8.3 短路径（ADMINI~1），剪贴板里给用户看的
     * 路径要换成完整形式 */
    wchar_t full[MAX_PATH];
    DWORD fl = GetLongPathNameW(s_rx.dir, full, MAX_PATH);
    if (fl > 0 && fl < MAX_PATH) wcscpy(s_rx.dir, full);
}

/* 收完后把顶层条目放进剪贴板（CF_HDROP），Mac 那边就能直接粘贴 */
static void rx_finish_batch(void) {
    if (s_rx.f && s_rx.f != INVALID_HANDLE_VALUE) { CloseHandle(s_rx.f); s_rx.f = INVALID_HANDLE_VALUE; }

    if (s_rx.cur_is_image && s_rx.img && s_rx.img_len) {
        clip_apply_remote_image(s_rx.img, s_rx.img_w, s_rx.img_h, s_rx.img_stride);
    } else if (s_rx.ntop > 0) {
        clip_apply_remote_paths((const wchar_t *const *)s_rx.tops, s_rx.ntop);
    }
    hop_log(HOP_LOG_INFO, "xfer: batch %u done (%d items)", s_rx.batch, s_rx.ntop);
    rx_reset();
}

static void on_data_msg(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;

    switch (type) {
        case HOP_MSG_XFER_HELLO:
            hop_log(HOP_LOG_INFO, "xfer: peer data channel ready");
            break;

        case HOP_MSG_FILE_BEGIN: {
            if (len < 27) return;
            uint32_t batch = hop_get_be32(p);
            uint32_t id    = hop_get_be32(p + 4);
            uint64_t size  = hop_get_be64(p + 8);
            uint32_t flags = p[24];
            uint32_t nlen  = hop_get_be16(p + 25);
            if (27 + nlen + 2 > len) return;
            uint32_t plen  = hop_get_be16(p + 27 + nlen);
            if (29 + nlen + plen > len) return;
            const char *rel = (const char *)(p + 29 + nlen);

            rx_begin_batch(batch);
            if (!s_rx.active) return;

            char *relz = (char *)malloc((size_t)plen + 1);
            if (!relz) return;
            memcpy(relz, rel, plen);
            relz[plen] = '\0';

            if (!rel_safe(relz)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", relz);
                free(relz);
                return;
            }

            wchar_t *path = rx_path_for(relz);
            free(relz);
            if (!path) return;

            if (s_rx.f && s_rx.f != INVALID_HANDLE_VALUE) {
                CloseHandle(s_rx.f);
                s_rx.f = INVALID_HANDLE_VALUE;
            }
            free(s_rx.cur_path);
            s_rx.cur_path = path;
            s_rx.cur_id = id;
            s_rx.cur_is_image = 0;

            if (flags & HOP_FILE_FLAG_DIR) {
                ensure_dirs(path);
                CreateDirectoryW(path, NULL);
                /* 顶层目录才进剪贴板清单 */
                if (!wcschr(path + wcslen(s_rx.dir) + 1, L'\\'))
                    rx_add_top(path);
            } else {
                /* 建父目录 */
                wchar_t *sep = wcsrchr(path, L'\\');
                if (sep) {
                    wchar_t save = *sep;
                    *sep = L'\0';
                    ensure_dirs(path);
                    CreateDirectoryW(path, NULL);
                    *sep = save;
                }
                s_rx.f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
                if (s_rx.f == INVALID_HANDLE_VALUE) {
                    hop_log(HOP_LOG_WARN, "xfer: cannot create %ls (err %lu)",
                            path, GetLastError());
                }
                const wchar_t *tail = path + wcslen(s_rx.dir) + 1;
                if (!wcschr(tail, L'\\')) rx_add_top(path);
            }
            (void)size;
            break;
        }

        case HOP_MSG_IMAGE_BEGIN: {
            if (len < 20) return;
            uint32_t batch = hop_get_be32(p);
            rx_begin_batch(batch);
            if (!s_rx.active) return;
            s_rx.cur_is_image = 1;
            s_rx.cur_id = hop_get_be32(p + 4);
            s_rx.img_w = (int)hop_get_be32(p + 8);
            s_rx.img_h = (int)hop_get_be32(p + 12);
            s_rx.img_stride = (int)hop_get_be32(p + 16);
            s_rx.img_len = 0;
            break;
        }

        case HOP_MSG_FILE_CHUNK: {
            if (len < 16) return;
            uint32_t id  = hop_get_be32(p);
            uint64_t off = hop_get_be64(p + 4);
            uint32_t n   = hop_get_be32(p + 12);
            if (16u + n > len) return;
            const uint8_t *data = p + 16;

            if (s_rx.cur_is_image) {
                size_t need = (size_t)off + n;
                if (need > s_rx.img_cap) {
                    size_t nc = s_rx.img_cap ? s_rx.img_cap : (size_t)1024 * 1024;
                    while (nc < need) nc *= 2;
                    uint8_t *nv = (uint8_t *)realloc(s_rx.img, nc);
                    if (!nv) return;
                    s_rx.img = nv;
                    s_rx.img_cap = nc;
                }
                memcpy(s_rx.img + off, data, n);
                if (need > s_rx.img_len) s_rx.img_len = need;
                break;
            }

            if (!s_rx.f || s_rx.f == INVALID_HANDLE_VALUE) return;
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)off;
            SetFilePointerEx(s_rx.f, li, NULL, FILE_BEGIN);
            DWORD wrote = 0;
            WriteFile(s_rx.f, data, n, &wrote, NULL);
            (void)id;
            break;
        }

        case HOP_MSG_FILE_END:
            if (s_rx.f && s_rx.f != INVALID_HANDLE_VALUE) {
                CloseHandle(s_rx.f);
                s_rx.f = INVALID_HANDLE_VALUE;
            }
            break;

        case HOP_MSG_XFER_DONE:
            rx_finish_batch();
            break;

        default:
            break;
    }
}

static DWORD WINAPI listen_thread(LPVOID arg) {
    (void)arg;
    hop_decoder *dec = hop_decoder_new(NULL, on_data_msg);
    if (!dec) return 1;

    while (WaitForSingleObject(s_x.stop, 0) != WAIT_OBJECT_0) {
        struct sockaddr_in addr;
        int alen = sizeof(addr);
        SOCKET c = accept(s_x.listen_s, (struct sockaddr *)&addr, &alen);
        if (c == INVALID_SOCKET) {
            if (WaitForSingleObject(s_x.stop, 0) == WAIT_OBJECT_0) break;
            Sleep(100);
            continue;
        }

        int bufsz = 4 * 1024 * 1024;
        setsockopt(c, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof(bufsz));
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof(bufsz));

        EnterCriticalSection(&s_x.lock);
        s_x.client = c;
        LeaveCriticalSection(&s_x.lock);

        hop_decoder_reset(dec);
        rx_reset();
        hop_log(HOP_LOG_INFO, "xfer: data channel connected");

        char buf[65536];
        for (;;) {
            int r = recv(c, buf, sizeof(buf), 0);
            if (r <= 0) break;
            hop_decoder_feed(dec, (const uint8_t *)buf, (uint32_t)r);
        }

        EnterCriticalSection(&s_x.lock);
        s_x.client = INVALID_SOCKET;
        LeaveCriticalSection(&s_x.lock);
        closesocket(c);
        rx_reset();
        hop_log(HOP_LOG_WARN, "xfer: data channel closed");
    }

    hop_decoder_free(dec);
    return 0;
}

/* ---------------- 对外接口 ---------------- */

int xfer_init(HWND hwnd, const hop_config *cfg) {
    memset(&s_x, 0, sizeof(s_x));
    s_x.hwnd = hwnd;
    s_x.cfg = *cfg;
    s_x.listen_s = INVALID_SOCKET;
    s_x.client = INVALID_SOCKET;

    int ck = cfg->xfer_chunk_kb;
    if (ck < 64) ck = 64;
    if (ck > 4096) ck = 4096;
    s_x.chunk = (uint32_t)ck * 1024u;

    InitializeCriticalSection(&s_x.lock);
    s_x.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_x.q_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    s_x.listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_x.listen_s == INVALID_SOCKET) goto fail;

    BOOL yes = TRUE;
    setsockopt(s_x.listen_s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->data_port);
    if (bind(s_x.listen_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        hop_log(HOP_LOG_ERROR, "xfer: bind %u failed (%d)",
                (unsigned)cfg->data_port, WSAGetLastError());
        goto fail;
    }
    if (listen(s_x.listen_s, 2) != 0) goto fail;

    s_rx.f = INVALID_HANDLE_VALUE;
    s_x.th_listen = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
    s_x.th_send = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    if (!s_x.th_listen || !s_x.th_send) goto fail;

    hop_log(HOP_LOG_INFO, "xfer: listening on port %u, chunk %u KB",
            (unsigned)cfg->data_port, ck);
    return 1;

fail:
    if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);
    if (s_x.stop) CloseHandle(s_x.stop);
    if (s_x.q_evt) CloseHandle(s_x.q_evt);
    DeleteCriticalSection(&s_x.lock);
    return 0;
}

void xfer_shutdown(void) {
    SetEvent(s_x.stop);
    if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);

    EnterCriticalSection(&s_x.lock);
    if (s_x.client != INVALID_SOCKET) {
        /* shutdown 让对端 recv 立刻返回，send 也会立刻失败 ——
         * 这样两个线程都能迅速退出，不会卡在阻塞调用里 */
        shutdown(s_x.client, SD_BOTH);
        closesocket(s_x.client);
        s_x.client = INVALID_SOCKET;
    }
    xfer_job *j = s_x.q_head;
    while (j) {
        xfer_job *nx = j->next;
        job_free(j);
        j = nx;
    }
    s_x.q_head = s_x.q_tail = NULL;
    LeaveCriticalSection(&s_x.lock);

    /* 线程没退出就不能删临界区 —— 那是释放后使用。
     * 线程还在说明有阻塞调用没返回，此时宁可留着临界区（进程马上退出），
     * 也不要把锁拆掉让它在未定义行为里跑。 */
    int joined = 1;
    if (s_x.th_listen) {
        if (WaitForSingleObject(s_x.th_listen, 3000) != WAIT_OBJECT_0) joined = 0;
        CloseHandle(s_x.th_listen);
    }
    if (s_x.th_send) {
        if (WaitForSingleObject(s_x.th_send, 3000) != WAIT_OBJECT_0) joined = 0;
        CloseHandle(s_x.th_send);
    }

    rx_reset();
    CloseHandle(s_x.stop);
    CloseHandle(s_x.q_evt);
    if (joined) DeleteCriticalSection(&s_x.lock);
}

int xfer_connected(void) {
    int c;
    EnterCriticalSection(&s_x.lock);
    c = (s_x.client != INVALID_SOCKET);
    LeaveCriticalSection(&s_x.lock);
    return c;
}

static int submit(xfer_job *j) {
    int ok = 0;
    EnterCriticalSection(&s_x.lock);
    if (s_x.client != INVALID_SOCKET) {
        if (s_x.q_tail) s_x.q_tail->next = j;
        else            s_x.q_head = j;
        s_x.q_tail = j;
        ok = 1;
    }
    LeaveCriticalSection(&s_x.lock);
    if (ok) SetEvent(s_x.q_evt);
    return ok;
}

int xfer_submit_paths(const wchar_t *const *paths, int n) {
    if (!paths || n <= 0) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop %d path(s)", n);
        return 0;
    }

    elist l;
    memset(&l, 0, sizeof(l));

    for (int i = 0; i < n; ++i) {
        if (!paths[i] || !paths[i][0]) continue;
        const wchar_t *bn = base_name(paths[i]);
        char *root = w2u(bn);
        if (!root) continue;
        walk_into(&l, paths[i], root);
        free(root);
    }

    if (l.n == 0) { elist_free(&l); return 0; }

    xfer_job *j = (xfer_job *)calloc(1, sizeof(*j));
    if (!j) { elist_free(&l); return 0; }

    j->batch = InterlockedIncrement((LONG *)&s_x.batch_seq);
    j->n = l.n;
    j->e = l.v;                 /* 直接接管 */

    uint64_t total = 0;
    for (int i = 0; i < l.n; ++i) total += l.v[i].size;

    hop_log(HOP_LOG_INFO, "xfer: queued batch %u, %d entries, %.1f MB",
            j->batch, j->n, (double)total / 1048576.0);

    if (!submit(j)) { job_free(j); return 0; }
    return 1;
}

int xfer_submit_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop image");
        return 0;
    }

    size_t total = (size_t)stride * (size_t)h;
    uint8_t *copy = (uint8_t *)malloc(total);
    if (!copy) return 0;
    memcpy(copy, bgra, total);

    xfer_job *j = (xfer_job *)calloc(1, sizeof(*j));
    if (!j) { free(copy); return 0; }

    j->batch = InterlockedIncrement((LONG *)&s_x.batch_seq);
    j->is_image = 1;
    j->pixels = copy;
    j->w = w; j->h = h; j->stride = stride;

    hop_log(HOP_LOG_INFO, "xfer: queued image %dx%d (%.1f MB)",
            w, h, (double)total / 1048576.0);

    if (!submit(j)) { job_free(j); return 0; }
    return 1;
}
