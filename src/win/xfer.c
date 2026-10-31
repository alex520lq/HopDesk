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

    /* ---- 重试状态 ----
     * 只有一个游标，没有 per-entry 状态数组：next_i 之前的条目都已确认送达，
     * next_i 之后都还没发。失败就从 next_i 原地重来，不需要记账。 */
    int         next_i;         /* 下一条待发条目 */
    int         ok;             /* 已收到 ACK_OK 的条目数 */
    int         attempt;        /* 连接级（作业级）已重试次数 */
    uint64_t    retry_at;       /* 毫秒时间戳，早于它不发送 */
    uint64_t    give_up_at;     /* 断线重连预算的截止时刻，0 = 未开始计时 */
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
    int              peer_ready;    /* 数据通道版本与 token 握手通过 */
    CRITICAL_SECTION lock;      /* 保护 client + 作业队列 + 取消标记 */
    HANDLE           stop;
    HANDLE           q_evt;
    HANDLE           th_listen;
    HANDLE           th_send;
    xfer_job        *q_head, *q_tail;
    uint32_t         batch_seq;
    uint32_t         chunk;     /* 实际分块字节数 */

    /* ---- 取消 ----
     * cur_batch 由发送线程在开始一批时设置；cancel_batch 由 UI 线程设置。
     * 发送线程每块之后比一次，所以取消的响应延迟上限是一个分块（256 KB）。 */
    uint32_t         cur_batch;     /* 0 = 当前没有在发的批次 */
    uint32_t         cancel_batch;  /* 0 = 没有取消请求 */
    volatile LONG    cancel_rx;     /* 接收侧取消请求，由 listen 线程消费 */
    uint32_t         abort_batch;   /* 对端 ABORT 过的批次，0 = 无 */
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

/* Relative path validation is shared in src/core/xfer_validate.c. */

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

/* 发送互斥：send_thread 和接收线程（出错时回 ABORT）会同时往同一个
 * socket 写。一次 send_all 内部可能分多次 send，两线程交错就会把
 * 帧字节搅在一起 —— 锁的粒度必须是"整帧"，不是"单次 send"。 */
static CRITICAL_SECTION s_txlock;

static int send_all(SOCKET c, const char *p, int len) {
    while (len > 0) {
        int n = send(c, p, len, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEINTR) continue;
        if (n <= 0) {
            /* 发送失败可能已写出半帧；关闭双向流以触发接收线程重连。 */
            shutdown(c, SD_BOTH);
            return 0;
        }
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
    EnterCriticalSection(&s_txlock);
    int ok = send_all(c, (const char *)hdr, HOP_HEADER_SIZE);
    if (ok && len) ok = send_all(c, (const char *)p, (int)len);
    LeaveCriticalSection(&s_txlock);
    return ok;
}

/* 分块：一次 send 发完头 + 分块头 + 数据。
 * 数据直接读进发送缓冲的 CHUNK_HDR 处，省掉一次内存拷贝。
 * 每块带 CRC32：TCP 的 16 位校验和配合网卡 offload 有可能漏检，
 * GB 级传输漏检一次就是静默损坏。 */
#define CHUNK_HDR 25u   /* 5 帧头 + 4 id + 8 off + 4 len + 4 crc */

static int send_chunk(SOCKET c, uint8_t *tx, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc) {
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + len);
    hop_put_be32(tx + 5, id);
    hop_put_be64(tx + 9, off);
    hop_put_be32(tx + 17, len);
    hop_put_be32(tx + 21, crc);
    if (data != tx + CHUNK_HDR) memcpy(tx + CHUNK_HDR, data, len);
    EnterCriticalSection(&s_txlock);
    int ok = send_all(c, (const char *)tx, (int)(CHUNK_HDR + len));
    LeaveCriticalSection(&s_txlock);
    return ok;
}

/* ---------------- 单文件 ACK ----------------
 *
 * 同一时刻只有一个文件在途（发送是串行的），所以一个槽就够。
 * 发送线程填完 END 就等这里；接收线程处理完 END 把结论放进来。
 *
 * 为什么要等：TCP 只保证"字节到了"，不保证"落盘成功、校验通过"。
 * 没有这个回执，发送端对"这个文件成没成"一无所知，
 * 就只能整批重传 —— 5 GB 的批次失败一次，代价是重传 5 GB。 */
static struct {
    CRITICAL_SECTION lock;
    HANDLE           ev;        /* 自动重置：有信号就说明槽里可能有新东西 */
    uint32_t         batch, id;
    int              status;
    int              valid;
} s_ack;

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)

static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }

static void ack_clear(void) {
    EnterCriticalSection(&s_ack.lock);
    s_ack.valid = 0;
    LeaveCriticalSection(&s_ack.lock);
}

/* 接收线程调用。槽满（上一份没人取）就覆盖 —— 上一份已经超时，
 * 发送端本来就要重发，旧结论没有价值。 */
static void ack_deliver(uint32_t batch, uint32_t id, int status) {
    EnterCriticalSection(&s_ack.lock);
    s_ack.batch  = batch;
    s_ack.id     = id;
    s_ack.status = status;
    s_ack.valid  = 1;
    LeaveCriticalSection(&s_ack.lock);
    SetEvent(s_ack.ev);
}

/* 用户取消了这个批次？每块之后查一次，锁开销可以忽略。 */
static int cancel_requested(uint32_t batch) {
    EnterCriticalSection(&s_x.lock);
    int c = (s_x.cancel_batch == batch);
    LeaveCriticalSection(&s_x.lock);
    return c;
}

/* 对端中止过这个批次？（ABORT 是批量级的，比如图片超限、接收端建不了目录） */
static int peer_aborted(uint32_t batch) {
    EnterCriticalSection(&s_x.lock);
    int a = (s_x.abort_batch == batch);
    LeaveCriticalSection(&s_x.lock);
    return a;
}

/* 发送线程调用。返回 status，超时返回 XFER_ACK_TIMEOUT。
 *
 * 用"先查槽再等事件"的循环，而不是"先 ResetEvent 再等"：
 * ACK 可能在两者之间到达，后者会漏掉信号然后白等满 10 秒。 */
static int ack_wait(uint32_t batch, uint32_t id) {
    uint64_t deadline = now_ms() + HOP_XFER_ACK_TIMEOUT_MS;
    for (;;) {
        EnterCriticalSection(&s_ack.lock);
        int hit = s_ack.valid && s_ack.batch == batch && s_ack.id == id;
        int st  = s_ack.status;
        if (hit) s_ack.valid = 0;
        LeaveCriticalSection(&s_ack.lock);
        if (hit) return st;

        /* 对端中止了就别干等：ABORT 的处理器会 SetEvent 把这个等待踢醒 */
        if (peer_aborted(batch)) return XFER_PEER_ABORT;
        /* 用户取消同理。发送大文件时线程绝大部分时间都停在这里而不是
         * 分块循环里 —— 不检查这个，点了取消要等满 10 秒才有反应。 */
        if (cancel_requested(batch)) return XFER_CANCELLED;
        /* 退出时别把 join 拖满 10 秒 */
        if (WaitForSingleObject(s_x.stop, 0) == WAIT_OBJECT_0) return XFER_SEND_DEAD;

        uint64_t n = now_ms();
        if (n >= deadline) return XFER_ACK_TIMEOUT;
        WaitForSingleObject(s_ack.ev, (DWORD)(deadline - n));
    }
}

/* 发送一个条目，并等接收端的单文件回执。
 *
 * 返回 >=0：接收端 ACK 的 status（HOP_FILE_ACK_*）
 * 返回 XFER_SEND_DEAD  ：写 socket 失败，连接已断 → 整批退回队列
 * 返回 XFER_LOCAL_IO   ：本地读失败 → 重发同一个文件
 * 返回 XFER_CANCELLED  ：用户取消 → 不重试
 * 返回 XFER_ACK_TIMEOUT：END 发出去了但等不到回执 → 重发同一个文件
 */
static int send_file_entry(SOCKET c, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xfer_entry *e) {
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);

    /* BEGIN: batch(4)+id(4)+size(8)+mtime(8)+flags(1)+nlen(2)+name+plen(2)+rel */
    uint32_t blen = 4 + 4 + 8 + 8 + 1 + 2 + nlen + 2 + plen;
    uint8_t *bp = (uint8_t *)malloc(blen);
    if (!bp) return XFER_LOCAL_IO;
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
    if (!ok) return XFER_SEND_DEAD;

    /* 整文件校验 = 对各块 CRC 再做一次累加 CRC。
     * 每块的 CRC 已经覆盖了它自己的数据，这里只需再覆盖"块的顺序与偏移"，
     * 不必把整份文件重算一遍 —— GB 级文件重算就是白白多一次全量读。
     * 这样既抓得到数据损坏（块 CRC），也抓得到错位/乱序（文件 CRC）。 */
    uint32_t fcrc = 0;
    uint8_t  end_status = HOP_FILE_OK;
    int      local_fail = 0;

    if (!(e->flags & HOP_FILE_FLAG_DIR) && e->size > 0) {
        HANDLE h = CreateFileW(e->full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (err %lu)",
                    e->rel, GetLastError());
            end_status = HOP_FILE_IO_ERROR;
            local_fail = 1;
        } else {
            uint64_t off = 0;
            for (;;) {
                DWORD got = 0;
                if (!ReadFile(h, tx + CHUNK_HDR, (DWORD)s_x.chunk, &got, NULL) || got == 0)
                    break;
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, got);
                if (!send_chunk(c, tx, id, off, tx + CHUNK_HDR, got, ccrc)) {
                    CloseHandle(h);
                    return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, ccrc);
                off += got;
                yield_cpu();
                /* 取消的响应粒度 = 一个分块。再细就要每 4 KB 查一次锁，
                 * 那点响应速度换不来任何可感知的差别。 */
                if (cancel_requested(batch)) {
                    CloseHandle(h);
                    return XFER_CANCELLED;
                }
            }
            CloseHandle(h);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    if (!send_small(c, HOP_MSG_FILE_END, endp, 9)) return XFER_SEND_DEAD;

    /* 本地就没读成功，没必要再等回执：接收端那份注定是残缺的，
     * 等到的结论也只会是"失败"。直接进重试。 */
    if (local_fail) return XFER_LOCAL_IO;

    return ack_wait(batch, id);
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
        uint32_t crc = hop_crc32(0, j->pixels + off, n);
        if (!send_chunk(c, tx, 1, off, j->pixels + off, n, crc)) return 0;
        off += n;
        yield_cpu();
    }

    uint8_t endp[9];
    hop_put_be32(endp, 1);
    endp[4] = HOP_FILE_OK;
    hop_put_be32(endp + 5, 0);
    return send_small(c, HOP_MSG_FILE_END, endp, 9);
}

/* 收尾：告诉接收端本批实际送达了多少。
 * ok < total 时接收端会报"部分成功" —— 这是用户唯一能拿到准确结论的地方，
 * 所以 count 必须是真实值，不能像以前那样写死 0。 */
static int send_done(SOCKET c, const xfer_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp,      j->batch);
    hop_put_be32(dp + 4,  (uint32_t)j->ok);
    hop_put_be32(dp + 8,  (uint32_t)j->n);
    hop_put_be64(dp + 12, 0);
    return send_small(c, HOP_MSG_XFER_DONE, dp, 20);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */

/* 实际发送（send 线程持有 tx 缓冲）。 */
static int run_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    if (j->is_image) {
        if (!send_image_job(c, tx, j)) return JOB_NET_DOWN;
        j->ok = 1;
        j->next_i = j->n;
        return JOB_DONE;
    }

    while (j->next_i < j->n) {
        if (peer_aborted(j->batch)) {
            hop_log(HOP_LOG_WARN, "xfer: batch %u aborted by peer at entry %d",
                    j->batch, j->next_i);
            return JOB_PEER_ABORT;
        }

        const xfer_entry *e = &j->e[j->next_i];
        uint32_t id = (uint32_t)(j->next_i + 1);
        int st = HOP_FILE_ACK_OK;
        int tries = 0;

        for (;;) {
            ack_clear();
            st = send_file_entry(c, tx, j->batch, id, e);
            if (st == HOP_FILE_ACK_OK) break;

            /* 连接层面的问题不在这里耗：交给队列层等重连，
             * 否则 3 次重试全打在一条死链路上，白白丢掉整批。 */
            if (st == XFER_SEND_DEAD) return JOB_NET_DOWN;
            if (st == XFER_PEER_ABORT) return JOB_PEER_ABORT;
            if (st == XFER_CANCELLED) {
                hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled by user at %s",
                        j->batch, e->rel);
                return JOB_CANCELLED;
            }
            /* 路径被拒是确定性失败，重发一百次也一样 */
            if (st == HOP_FILE_ACK_BADPATH) break;

            if (++tries >= HOP_XFER_FILE_RETRY_MAX) break;
            unsigned d = hop_xfer_file_retry_delay_ms(tries);
            hop_log(HOP_LOG_WARN, "xfer: retry %d/%d for %s in %u ms (status %d)",
                    tries, HOP_XFER_FILE_RETRY_MAX - 1, e->rel, d, st);
            Sleep(d);
        }

        if (st == HOP_FILE_ACK_OK) {
            j->ok++;
        } else {
            hop_log(HOP_LOG_ERROR, "xfer: giving up on %s after %d attempt(s) (status %d)",
                    e->rel, tries + 1, st);
        }
        j->next_i++;
    }
    return JOB_DONE;
}

/* 从作业队列里摘掉 cur。prev 是它的前驱（cur 是头时为 NULL）。
 *
 * 尾指针不能简单地"摘到尾巴就置 NULL"：cur 是尾巴但前面还有节点时，
 * q_tail 必须指回前驱。置 NULL 之后下一次入队会走
 * `if (q_tail) ... else q_head = j` 的 else 分支，把 q_head 直接覆盖掉 ——
 * 前面排队的作业既不会被发送，也不会被释放，凭空消失。
 * 调用方必须持有 s_x.lock。 */
static void queue_unlink(xfer_job *prev, xfer_job *cur) {
    if (prev) prev->next = cur->next;
    else      s_x.q_head = cur->next;
    if (s_x.q_tail == cur) s_x.q_tail = prev;
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

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(SOCKET c, uint32_t batch, uint8_t reason) {
    uint8_t ab[5];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    send_small(c, HOP_MSG_XFER_ABORT, ab, 5);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
static void send_ack(SOCKET c, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(c, HOP_MSG_FILE_ACK, b, 9);
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
        /* 200 ms 一次：既够快响应用户的取消，又不至于空转烧 CPU。
         * 队列里有等待重连的作业时，这个轮询就是它唯一的唤醒源。 */
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        for (;;) {
            EnterCriticalSection(&s_x.lock);
            SOCKET c = s_x.client;
            xfer_job *j = NULL, *expired = NULL;
            uint64_t now = now_ms();

            /* 一次只摘一个，摘到就跳出、在锁外处理 —— 队列操作留在锁里，
             * 释放和日志留在锁外，锁的持有时间永远是最短的。 */
            xfer_job *prev = NULL;
            xfer_job *cur  = s_x.q_head;
            while (cur) {
                /* 重连预算用完：连接在不在都放弃。
                 * 这个检查必须独立于"连接是否可用" —— 对端再也不回来时，
                 * 下面的分支根本不会执行，作业就会永久挂在队列里。 */
                if (cur->give_up_at && now >= cur->give_up_at) {
                    expired = cur;
                    queue_unlink(prev, cur);
                    expired->next = NULL;
                    break;
                }
                if (c != INVALID_SOCKET && cur->retry_at <= now) {
                    j = cur;
                    queue_unlink(prev, cur);
                    break;
                }
                prev = cur;
                cur  = cur->next;
            }
            if (j) s_x.cur_batch = j->batch;
            LeaveCriticalSection(&s_x.lock);

            if (expired) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                        expired->batch, expired->ok, expired->n);
                job_free(expired);
                continue;
            }

            if (!j) break;

            int r = run_job(c, tx, j);

            EnterCriticalSection(&s_x.lock);
            s_x.cur_batch = 0;
            LeaveCriticalSection(&s_x.lock);

            if (r == JOB_NET_DOWN) {
                if (j->give_up_at == 0)
                    j->give_up_at = now_ms() + HOP_XFER_RECONNECT_BUDGET_MS;
                j->attempt++;
                if (j->attempt < HOP_XFER_JOB_RETRY_MAX && now_ms() < j->give_up_at) {
                    unsigned d = hop_xfer_job_retry_delay_ms(j->attempt);
                    j->retry_at = now_ms() + d;
                    hop_log(HOP_LOG_WARN,
                            "xfer: batch %u interrupted at entry %d, retry %d/%d in %u ms",
                            j->batch, j->next_i, j->attempt,
                            HOP_XFER_JOB_RETRY_MAX - 1, d);
                    /* 退回队尾，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                    j->next = NULL;
                    EnterCriticalSection(&s_x.lock);
                    if (s_x.q_tail) s_x.q_tail->next = j;
                    else            s_x.q_head = j;
                    s_x.q_tail = j;
                    LeaveCriticalSection(&s_x.lock);
                    continue;
                }
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u abandoned after %d attempt(s), %d/%d delivered",
                        j->batch, j->attempt, j->ok, j->n);
            } else if (r == JOB_CANCELLED) {
                send_abort_msg(c, j->batch, HOP_ABORT_CANCEL);
            } else if (r == JOB_DONE) {
                if (j->ok < j->n)
                    hop_log(HOP_LOG_WARN, "xfer: batch %u finished %d/%d",
                            j->batch, j->ok, j->n);
                send_done(c, j);
            }
            job_free(j);
        }
    }
    free(tx);
    return 0;
}

/* ---------------- 写盘队列 ----------------
 *
 * 网络线程只做"收包 + 校验 + 入队"，写盘交给独立线程。
 *
 * 队列有界（RX_Q_SLOTS）。满了网络线程就阻塞在 wq_push，
 * recv 随之停止 → TCP 接收窗口收缩 → 发送端自动减速。
 * 这就是显式背压：内存上界 = RX_Q_SLOTS × chunk，不随传输时长增长。
 * 文件句柄由写线程独占，避免"网络线程关文件、写线程还在写"的竞态。
 */
#define RX_Q_SLOTS 8

enum { RX_OPEN = 1, RX_WRITE, RX_CLOSE };

typedef struct {
    int      kind;
    wchar_t  path[MAX_PATH];
    uint64_t off;
    uint32_t len;
    uint8_t *data;
} rx_task;

static struct {
    rx_task            ring[RX_Q_SLOTS];
    int                head, tail, count, busy;
    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE cv_work, cv_space;
    HANDLE             th;
    HANDLE             f;        /* 写线程独占 */
    int                err;      /* 写盘失败标记，主路径读 */
    int                stop;
} s_wq;

static void wq_task_run(rx_task *t) {
    switch (t->kind) {
        case RX_OPEN:
            if (s_wq.f && s_wq.f != INVALID_HANDLE_VALUE) CloseHandle(s_wq.f);
            s_wq.f = CreateFileW(t->path, GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_wq.f == INVALID_HANDLE_VALUE) {
                hop_log(HOP_LOG_WARN, "xfer: cannot create %ls (err %lu)",
                        t->path, GetLastError());
                s_wq.err = 1;
            }
            break;

        case RX_WRITE:
            if (s_wq.f && s_wq.f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)t->off;
                SetFilePointerEx(s_wq.f, li, NULL, FILE_BEGIN);
                DWORD wrote = 0;
                if (!WriteFile(s_wq.f, t->data, t->len, &wrote, NULL) ||
                    wrote != t->len) {
                    hop_log(HOP_LOG_WARN, "xfer: write failed at %llu (err %lu)",
                            (unsigned long long)t->off, GetLastError());
                    s_wq.err = 1;
                }
            }
            break;

        case RX_CLOSE:
            if (s_wq.f && s_wq.f != INVALID_HANDLE_VALUE) {
                CloseHandle(s_wq.f);
                s_wq.f = INVALID_HANDLE_VALUE;
            }
            break;
    }
}

static DWORD WINAPI writer_thread(LPVOID arg) {
    (void)arg;
    /* 写盘也是后台工作，同样降级，别跟输入路径抢 CPU */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;) {
        EnterCriticalSection(&s_wq.lock);
        while (s_wq.count == 0 && !s_wq.stop)
            SleepConditionVariableCS(&s_wq.cv_work, &s_wq.lock, INFINITE);
        if (s_wq.count == 0 && s_wq.stop) {
            LeaveCriticalSection(&s_wq.lock);
            break;
        }
        rx_task t = s_wq.ring[s_wq.head];
        s_wq.head = (s_wq.head + 1) % RX_Q_SLOTS;
        s_wq.count--;
        s_wq.busy = 1;
        LeaveCriticalSection(&s_wq.lock);

        wq_task_run(&t);

        EnterCriticalSection(&s_wq.lock);
        s_wq.busy = 0;
        WakeConditionVariable(&s_wq.cv_space);
        LeaveCriticalSection(&s_wq.lock);
        free(t.data);
    }
    if (s_wq.f && s_wq.f != INVALID_HANDLE_VALUE) {
        CloseHandle(s_wq.f);
        s_wq.f = INVALID_HANDLE_VALUE;
    }
    return 0;
}

/* 入队。队列满时阻塞 —— 这就是背压的着力点。 */
static int wq_push(const rx_task *t) {
    EnterCriticalSection(&s_wq.lock);
    while (s_wq.count >= RX_Q_SLOTS && !s_wq.stop)
        SleepConditionVariableCS(&s_wq.cv_space, &s_wq.lock, INFINITE);
    if (s_wq.stop) {
        LeaveCriticalSection(&s_wq.lock);
        return 0;
    }
    s_wq.ring[s_wq.tail] = *t;
    s_wq.tail = (s_wq.tail + 1) % RX_Q_SLOTS;
    s_wq.count++;
    WakeConditionVariable(&s_wq.cv_work);
    LeaveCriticalSection(&s_wq.lock);
    return 1;
}

/* 等队列排空且写线程空闲。关文件、比对文件 CRC 之前必须调用。 */
static void wq_drain(void) {
    EnterCriticalSection(&s_wq.lock);
    while ((s_wq.count > 0 || s_wq.busy) && !s_wq.stop)
        SleepConditionVariableCS(&s_wq.cv_space, &s_wq.lock, INFINITE);
    LeaveCriticalSection(&s_wq.lock);
}

static int wq_start(void) {
    memset(&s_wq, 0, sizeof(s_wq));
    s_wq.f = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&s_wq.lock);
    InitializeConditionVariable(&s_wq.cv_work);
    InitializeConditionVariable(&s_wq.cv_space);
    s_wq.th = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
    if (!s_wq.th) {
        DeleteCriticalSection(&s_wq.lock);
        return 0;
    }
    return 1;
}

/* 停写线程：先让它把队列里的活干完（数据已经收下来了，丢了就是损坏），
 * 再置 stop 唤醒，最后 join。没 join 成功就保留临界区。 */
static void wq_stop(void) {
    if (!s_wq.th) return;
    wq_drain();
    EnterCriticalSection(&s_wq.lock);
    s_wq.stop = 1;
    WakeAllConditionVariable(&s_wq.cv_work);
    WakeAllConditionVariable(&s_wq.cv_space);
    LeaveCriticalSection(&s_wq.lock);

    if (WaitForSingleObject(s_wq.th, 3000) == WAIT_OBJECT_0) {
        CloseHandle(s_wq.th);
        s_wq.th = NULL;
        DeleteCriticalSection(&s_wq.lock);
    } else {
        hop_log(HOP_LOG_WARN, "xfer: writer thread did not exit, keep lock");
        CloseHandle(s_wq.th);
        s_wq.th = NULL;
    }
}

/* ---------------- 接收 ---------------- */

typedef struct {
    uint32_t  batch;
    int       active;
    uint32_t  dead_batch;    /* 已作废的批次（本地取消 / 对端中止）。
                              * 它之后的帧一律忽略，且不再为它建目录 ——
                              * 否则取消之后对端再发一帧就把目录又建回来了。 */
    wchar_t   dir[MAX_PATH];
    uint32_t  cur_id;
    wchar_t  *cur_path;
    int       cur_is_image;
    int       cur_is_dir;    /* 目录在 BEGIN 时已登记，FILE_END 不能再登记一次 */
    int       cur_fail;      /* 本条目已判死：-1 = 正常，否则是 FILE_END 要回的 ACK 状态。
                              * 坏路径和坏数据都在这里落定，FILE_END 只负责回执。 */
    uint64_t  cur_size;      /* 声明的大小：超出就拒收，防止被灌成无限大 */
    /* 当前文件的校验累加：每块 CRC 折进来，FILE_END 时与发送端比对 */
    uint32_t  fcrc;
    /* 图片累积 */
    uint8_t  *img;
    size_t    img_cap, img_len;
    int       img_w, img_h, img_stride;
    /* 本批顶层条目（用于最后设置剪贴板） */
    wchar_t **tops;
    int       ntop;
} xfer_rx;

static xfer_rx s_rx;

/* 关掉当前文件（排空写队列后）。断线时也要调，但不能清批次状态 ——
 * 发送端会带着同一个 batch 号回来重发剩下的条目。 */
static void rx_close_file(void) {
    wq_drain();
    rx_task close_t;
    memset(&close_t, 0, sizeof(close_t));
    close_t.kind = RX_CLOSE;
    wq_push(&close_t);
    wq_drain();
}

static void rx_clear_state(void) {
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
    s_rx.fcrc = 0;
    s_rx.cur_fail = -1;
    s_rx.dir[0] = L'\0';
}

static void rx_reset(void) {
    /* 先把队列排空并关掉文件，再清状态 —— 否则写线程可能还在用旧路径 */
    rx_close_file();
    rx_clear_state();
}

/* 递归删掉本批临时目录。
 * 只在"用户取消 / 对端中止"时调用：失败批次要保留已送达的文件，
 * 成功批次的文件就是产物本身，都不能删。 */
static void rx_discard_dir(void) {
    if (!s_rx.dir[0]) return;
    /* 路径是我们自己拼的，但删除是不可逆操作 —— 仍然验一次根目录，
     * 免得哪天拼错了把用户目录端掉。 */
    if (!wcsstr(s_rx.dir, XFER_RX_ROOT)) {
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %ls", s_rx.dir);
        return;
    }
    wchar_t buf[MAX_PATH + 2];
    size_t n = wcslen(s_rx.dir);
    if (n + 2 > MAX_PATH + 2) return;
    wcscpy(buf, s_rx.dir);
    buf[n + 1] = L'\0';                 /* SHFileOperationW 要双 null 结尾 */

    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = buf;
    op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0)
        hop_log(HOP_LOG_WARN, "xfer: failed to remove %ls", s_rx.dir);
    s_rx.dir[0] = L'\0';
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
    /* 重发会让同一个顶层条目再来一次（比如 ACK 超时但文件其实写成功了），
     * 不去重的话剪贴板里会出现两份同样的路径。ntop 只是顶层条目数，
     * 线性扫一遍的代价可以忽略。 */
    for (int i = 0; i < s_rx.ntop; ++i)
        if (wcscmp(s_rx.tops[i], p) == 0) return 1;

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
    /* 清掉上一批遗留的"接收侧取消"请求。
     *
     * 用户可能在"没有接收在途"的时候点取消（比如只在发文件），
     * 那时 cancel_rx 置了 1 却没人消费 —— 它会一直躺到对端下一次
     * 开始发文件，在第一个数据帧之后被 InterlockedExchange 吃掉，
     * 把刚建立的新批次误判成"用户要取消"。批次一换，旧请求就该作废。 */
    InterlockedExchange(&s_x.cancel_rx, 0);

    /* 作废过的批次又来了（对端还没收到取消）：回一个 ABORT 让它立刻放手，
     * 千万别为它重建目录 —— 用户已经说过不要了。 */
    if (batch == s_rx.dead_batch) {
        send_abort_msg(s_x.client, batch, HOP_ABORT_CANCEL);
        return;
    }
    if (s_rx.active && s_rx.batch == batch) return;   /* 重发同一个批次：接着用 */
    rx_reset();
    s_rx.dead_batch = 0;                              /* 换批次了，墓碑清掉 */

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        send_abort_msg(s_x.client, batch, HOP_ABORT_DISK);
        return;
    }

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
        send_abort_msg(s_x.client, batch, HOP_ABORT_DISK);
        return;
    }

    wcscpy(s_rx.dir, tmp);
    wcscat(s_rx.dir, sub);

    s_rx.active = 1;
    s_rx.batch = batch;

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
    /* 落盘全部完成后才能把路径挂到剪贴板 —— 否则用户粘贴到的是半截文件 */
    rx_task t;
    memset(&t, 0, sizeof(t));
    t.kind = RX_CLOSE;
    wq_push(&t);
    wq_drain();

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

    if (type == HOP_MSG_XFER_HELLO) {
        size_t want = strlen(s_x.cfg.token);
        int valid = hop_xfer_hello_valid(p, len, s_x.cfg.token);
        if (!valid) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: peer handshake rejected (protocol/token mismatch)");
            EnterCriticalSection(&s_x.lock);
            SOCKET active = s_x.client;
            s_x.peer_ready = 0;
            LeaveCriticalSection(&s_x.lock);
            if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
            return;
        }
        EnterCriticalSection(&s_x.lock);
        s_x.peer_ready = 1;
        LeaveCriticalSection(&s_x.lock);
        hop_log(HOP_LOG_INFO,
                want ? "xfer: authenticated peer data channel (v%u)" :
                       "xfer: peer data channel ready (v%u)",
                (unsigned)HOP_XFER_PROTO_VER);
        return;
    }

    EnterCriticalSection(&s_x.lock);
    int ready = s_x.peer_ready;
    SOCKET active = s_x.client;
    LeaveCriticalSection(&s_x.lock);
    if (!ready) {
        hop_log(HOP_LOG_WARN, "xfer: frame received before handshake; closing peer");
        if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
        return;
    }

    switch (type) {

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

            /* 先把"当前条目"登记好，哪怕它是坏的：FILE_END 要靠 cur_id 回 ACK，
             * 漏回一次，发送端就要白等 10 秒超时再重试。 */
            free(s_rx.cur_path);
            s_rx.cur_path     = NULL;
            s_rx.cur_id       = id;
            s_rx.cur_is_image = 0;
            s_rx.cur_is_dir   = (flags & HOP_FILE_FLAG_DIR) ? 1 : 0;
            s_rx.cur_fail     = -1;
            s_rx.cur_size     = 0;   /* 0 = 任何分块都越界被拒，坏条目不落盘 */
            s_rx.fcrc         = 0;
            s_wq.err          = 0;

            char *relz = (char *)malloc((size_t)plen + 1);
            if (!relz) { s_rx.cur_fail = HOP_FILE_ACK_DISK; break; }
            memcpy(relz, rel, plen);
            relz[plen] = '\0';

            if (!hop_xfer_relpath_bytes_safe((const uint8_t *)relz, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", relz);
                free(relz);
                s_rx.cur_fail = HOP_FILE_ACK_BADPATH;
                break;
            }

            wchar_t *path = rx_path_for(relz);
            free(relz);
            if (!path) { s_rx.cur_fail = HOP_FILE_ACK_BADPATH; break; }

            s_rx.cur_path = path;
            s_rx.cur_size = size;

            if (s_rx.cur_is_dir) {
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
                /* 打开交给写线程 —— 句柄归它独占 */
                rx_task t;
                memset(&t, 0, sizeof(t));
                t.kind = RX_OPEN;
                wcsncpy(t.path, path, MAX_PATH - 1);
                wq_push(&t);

                /* 不在这里进剪贴板清单 —— 此刻文件还没写成功。
                 * 写盘失败 / 校验失败的文件绝不能出现在剪贴板里，
                 * 否则用户粘贴到的是一份截断的、看起来完全正常的文件。
                 * 由 FILE_END 的成功分支决定是否登记。 */
            }
            break;
        }

        case HOP_MSG_IMAGE_BEGIN: {
            if (len < 20) return;
            uint32_t batch = hop_get_be32(p);
            rx_begin_batch(batch);
            if (!s_rx.active) return;

            int w = (int)hop_get_be32(p + 8);
            int h = (int)hop_get_be32(p + 12);
            int stride = (int)hop_get_be32(p + 16);

            /* 对端声称的尺寸必须自己验：不然 w=h=100000 就能把内存吃光。
             * 这正是"避免内存不断增长"的边界。 */
            if (!hop_xfer_image_valid(w, h, stride)) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                uint8_t ab[5];
                hop_put_be32(ab, s_rx.batch);
                ab[4] = HOP_ABORT_TOO_LARGE;
                send_small(s_x.client, HOP_MSG_XFER_ABORT, ab, 5);
                return;
            }

            s_rx.cur_is_image = 1;
            s_rx.cur_id = hop_get_be32(p + 4);
            s_rx.img_w = w;
            s_rx.img_h = h;
            s_rx.img_stride = stride;
            s_rx.img_len = 0;
            break;
        }

        case HOP_MSG_FILE_CHUNK: {
            if (len < 20) return;
            uint64_t off = hop_get_be64(p + 4);
            uint32_t n   = hop_get_be32(p + 12);
            uint32_t crc = hop_get_be32(p + 16);
            if (!hop_xfer_chunk_valid(p, len,
                    s_rx.cur_fail >= 0 ? UINT64_MAX : s_rx.cur_size,
                    s_rx.cur_is_image)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed or out-of-range chunk; closing data channel");
                EnterCriticalSection(&s_x.lock);
                SOCKET active = s_x.client;
                LeaveCriticalSection(&s_x.lock);
                if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
                return;
            }
            const uint8_t *data = p + 20;

            /* 逐块校验。TCP 的 16 位校验和 + 网卡 offload 有漏检可能，
             * 漏检一次就是静默写入损坏数据。这里宁可报错也不要写坏文件。
             *
             * 坏一块只作废这一个文件，不再 ABORT 整批：
             * 发送端收到 ACK_CRC 会单独重发它，其余文件照常落地。 */
            if (hop_crc32(0, data, n) != crc) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: CHUNK CRC MISMATCH off=%llu len=%u (expect %08x)",
                        (unsigned long long)off, n, crc);
                s_rx.cur_fail = HOP_FILE_ACK_CRC;
                return;
            }

            if (s_rx.cur_is_image) {
                size_t need = (size_t)off + n;
                if (need > HOP_IMG_MAX_BYTES) {
                    hop_log(HOP_LOG_ERROR, "xfer: image exceeds cap, dropped");
                    return;
                }
                if (need > s_rx.img_cap) {
                    size_t nc = s_rx.img_cap ? s_rx.img_cap : (size_t)1024 * 1024;
                    while (nc < need) nc *= 2;
                    if (nc > HOP_IMG_MAX_BYTES) nc = (size_t)HOP_IMG_MAX_BYTES;
                    uint8_t *nv = (uint8_t *)realloc(s_rx.img, nc);
                    if (!nv) return;
                    s_rx.img = nv;
                    s_rx.img_cap = nc;
                }
                memcpy(s_rx.img + off, data, n);
                if (need > s_rx.img_len) s_rx.img_len = need;
                break;
            }

            /* 不许超出声明的大小：否则对端能一直灌，把磁盘写爆 */
            if (off + n > s_rx.cur_size) {
                hop_log(HOP_LOG_ERROR, "xfer: chunk beyond declared size (%llu+%u > %llu)",
                        (unsigned long long)off, n, (unsigned long long)s_rx.cur_size);
                return;
            }

            /* 校验通过才入队。拷贝一份交给写线程 —— 解码器的缓冲
             * 在回调返回后就被释放了，不能把指针递出去。 */
            rx_task t;
            memset(&t, 0, sizeof(t));
            t.kind = RX_WRITE;
            t.off = off;
            t.len = n;
            t.data = (uint8_t *)malloc(n);
            if (!t.data) return;
            memcpy(t.data, data, n);
            if (!wq_push(&t)) free(t.data);

            /* 文件 CRC 折入本块 CRC：既覆盖数据（块 CRC 已验证），
             * 也覆盖顺序与偏移，且不需要把整份文件重算一遍 */
            s_rx.fcrc = hop_xfer_file_crc_step(s_rx.fcrc, crc);
            break;
        }

        case HOP_MSG_FILE_END: {
            if (len < 9) return;
            uint32_t id     = hop_get_be32(p);
            uint32_t want   = hop_get_be32(p + 5);
            uint8_t  status = p[4];

            /* 图片整批走，没有逐文件回执：发送端不会等 */
            if (s_rx.cur_is_image) break;

            int ack = HOP_FILE_ACK_OK;

            if (s_rx.cur_fail >= 0) {
                ack = s_rx.cur_fail;
            } else {
                rx_close_file();     /* 关文件、比 CRC 之前必须排空写队列 */

                if (s_wq.err) {
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %ls incomplete",
                            s_rx.cur_path ? s_rx.cur_path : L"?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %ls",
                            (unsigned)status, s_rx.cur_path ? s_rx.cur_path : L"?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (s_rx.cur_size > 0 && want != s_rx.fcrc) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %ls (expect %08x got %08x)",
                            s_rx.cur_path ? s_rx.cur_path : L"?", want, s_rx.fcrc);
                    ack = HOP_FILE_ACK_CRC;
                } else if (s_rx.cur_path && !s_rx.cur_is_dir) {
                    /* 落盘成功、校验通过，才登记为可粘贴条目。
                     * 目录条目不在这里登记 —— 它在 BEGIN 时就已建好并登记。 */
                    const wchar_t *tail = s_rx.cur_path + wcslen(s_rx.dir) + 1;
                    if (!wcschr(tail, L'\\')) rx_add_top(s_rx.cur_path);
                }
            }

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            send_ack(s_x.client, s_rx.batch, id, ack);
            break;
        }

        case HOP_MSG_FILE_ACK:
            /* 本端是发送方时，单文件结论从这里进来 —— 送到槽里，
             * 让 send_file_entry 里等着的那个 ack_wait 返回。 */
            if (len >= 9)
                ack_deliver(hop_get_be32(p), hop_get_be32(p + 4), p[8]);
            break;

        case HOP_MSG_XFER_ABORT: {
            if (len < 5) return;
            uint32_t batch  = hop_get_be32(p);
            uint8_t  reason = p[4];

            /* 发送线程可能正卡在等 ACK 里 —— 踢醒它，取消不该等满 10 秒 */
            EnterCriticalSection(&s_x.lock);
            s_x.abort_batch = batch;
            LeaveCriticalSection(&s_x.lock);
            SetEvent(s_ack.ev);

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。
             * 墓碑必须立 —— 否则对端再发一帧 FILE_BEGIN 就把目录又建回来了。 */
            if (s_rx.active && s_rx.batch == batch) {
                rx_close_file();
                rx_discard_dir();
                s_rx.dead_batch = batch;
                rx_clear_state();
            }
            break;
        }

        case HOP_MSG_XFER_DONE:
            if (len >= 20) {
                uint32_t batch = hop_get_be32(p);
                uint32_t ok    = hop_get_be32(p + 4);
                uint32_t total = hop_get_be32(p + 8);
                if (ok < total)
                    hop_log(HOP_LOG_ERROR,
                            "xfer: batch %u PARTIAL - %u/%u delivered, %u failed",
                            batch, ok, total, total - ok);
            }
            rx_finish_batch();
            break;

        default:
            break;
    }
}

/* 用户在本端取消接收：通知对端、删掉半成品、立墓碑。
 * 由 listen 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.active) return;
    uint32_t batch = s_rx.batch;
    send_abort_msg(s_x.client, batch, HOP_ABORT_CANCEL);
    rx_close_file();
    rx_discard_dir();
    s_rx.dead_batch = batch;
    rx_clear_state();
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
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

        /* 数据通道是空闲时间最长的：不开 keepalive 的话，链路中间设备
         * 丢掉空闲连接表项后这条 socket 就变黑洞，send 返回成功但字节
         * 永远到不了对端，两端还都以为连着。 */
        net_set_keepalive(c);

        /* 发送超时：ABORT 是从接收线程发的，如果发送线程正卡在 send 里
         * （对端窗口满），接收线程拿不到发送锁就会跟着卡死 —— 那是死锁。
         * 给个上限，卡住就当作传输失败，别把整个通道拖死。
         * 256 KB 块在 100 Mbps 链路上只要 20 ms，5 秒是极宽的余量。 */
        DWORD sndto = 5000;
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&sndto, sizeof(sndto));

        /* 接收超时不是错误处理，是给取消请求开一条唤醒通道：
         * 对端暂停发送时 recv 会一直阻塞，而 s_rx 只归本线程所有，
         * 取消就只能干等 —— 用户点了"取消"，界面 30 秒没反应。
         * 200 ms 醒一次，代价是空闲时每秒 5 次空转，可以忽略。 */
        DWORD rxto = 200;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&rxto, sizeof(rxto));

        EnterCriticalSection(&s_x.lock);
        s_x.client = c;
        s_x.peer_ready = 0;
        s_x.abort_batch = 0;          /* 新会话，上一轮的 ABORT 不再算数 */
        LeaveCriticalSection(&s_x.lock);

        hop_decoder_reset(dec);
        rx_reset();

        /* 对称握手：Mac 端（客户端）连上后会发 HELLO，服务端这边原来只
         * 接收不发，于是 Mac 端永远打不出 "peer data channel ready"，
         * 版本不一致时也发现不了 —— 一条静默的协议漂移。两边都发。 */
        {
            uint8_t hello[2 + HOP_TOKEN_MAX];
            size_t token_len = strlen(s_x.cfg.token);
            hello[0] = HOP_XFER_PROTO_VER;
            hello[1] = (uint8_t)token_len;
            if (token_len) memcpy(hello + 2, s_x.cfg.token, token_len);
            if (!send_small(c, HOP_MSG_XFER_HELLO, hello, (uint32_t)(2 + token_len)))
                shutdown(c, SD_BOTH);
        }

        hop_log(HOP_LOG_INFO, "xfer: data channel connected; waiting for peer authentication");

        char buf[65536];
        for (;;) {
            int r = recv(c, buf, sizeof(buf), 0);

            if (r < 0) {
                int e = WSAGetLastError();
                if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) {
                    /* 不是断线，只是这 200 ms 没数据：借这次醒来处理取消请求。
                     * s_rx 只归本线程所有，别的线程不能直接碰。 */
                    if (InterlockedExchange(&s_x.cancel_rx, 0)) rx_cancel_local();
                    continue;
                }
                break;
            }
            if (r == 0) break;                 /* 对端正常关闭 */

            hop_decoder_feed(dec, (const uint8_t *)buf, (uint32_t)r);
            if (InterlockedExchange(&s_x.cancel_rx, 0)) rx_cancel_local();
        }

        EnterCriticalSection(&s_x.lock);
        s_x.client = INVALID_SOCKET;
        s_x.peer_ready = 0;
        LeaveCriticalSection(&s_x.lock);
        closesocket(c);
        /* 只关文件，不清批次：发送端会带着同一个 batch 号回来，
         * 从断点条目继续发。这里清掉就变成"从头重传整批"了。 */
        rx_close_file();
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
    InitializeCriticalSection(&s_txlock);
    InitializeCriticalSection(&s_ack.lock);
    s_ack.ev = CreateEventW(NULL, FALSE, FALSE, NULL);   /* 自动重置 */
    if (!s_ack.ev) goto fail;
    s_x.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_x.q_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    s_x.listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_x.listen_s == INVALID_SOCKET) goto fail;

    /* 同 net_server：用 SO_EXCLUSIVEADDRUSE 独占端口，不让别的进程
     * （包括另一个实例、或者调试用的对端模拟器）把连接悄悄抢走。 */
    BOOL yes = TRUE;
    if (setsockopt(s_x.listen_s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (char *)&yes, sizeof(yes)) != 0) {
        hop_log(HOP_LOG_WARN, "xfer: SO_EXCLUSIVEADDRUSE failed (%d)",
                WSAGetLastError());
    }

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

    /* 写盘线程要先起来，接收路径一进来就会往队列里塞 */
    if (!wq_start()) goto fail;

    s_x.th_listen = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
    s_x.th_send = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    if (!s_x.th_listen || !s_x.th_send) goto fail;

    hop_log(HOP_LOG_INFO, "xfer: listening on port %u, chunk %u KB",
            (unsigned)cfg->data_port, ck);
    return 1;

fail:
    if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);
    wq_stop();
    if (s_x.stop) CloseHandle(s_x.stop);
    if (s_x.q_evt) CloseHandle(s_x.q_evt);
    if (s_ack.ev) { CloseHandle(s_ack.ev); s_ack.ev = NULL; }
    DeleteCriticalSection(&s_x.lock);
    DeleteCriticalSection(&s_txlock);
    DeleteCriticalSection(&s_ack.lock);
    return 0;
}

void xfer_shutdown(void) {
    SetEvent(s_x.stop);
    SetEvent(s_ack.ev);          /* 踢醒可能正卡在等 ACK 的发送线程 */
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

    /* 线程没退出就不能拆临界区 / 队列 —— 那是释放后使用。
     * 线程还在说明有阻塞调用没返回，此时宁可什么都不释放（进程马上退出），
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

    if (!joined) {
        hop_log(HOP_LOG_WARN, "xfer: threads still alive at shutdown, skip cleanup");
        return;
    }

    rx_reset();      /* 先把写盘队列排空并关掉文件，再停写线程 */
    wq_stop();
    CloseHandle(s_x.stop);
    CloseHandle(s_x.q_evt);
    if (s_ack.ev) { CloseHandle(s_ack.ev); s_ack.ev = NULL; }
    DeleteCriticalSection(&s_x.lock);
    DeleteCriticalSection(&s_txlock);
    DeleteCriticalSection(&s_ack.lock);
}

int xfer_connected(void) {
    int c;
    EnterCriticalSection(&s_x.lock);
    c = (s_x.client != INVALID_SOCKET && s_x.peer_ready);
    LeaveCriticalSection(&s_x.lock);
    return c;
}

/* 有没有正在进行的传输？菜单项据此决定是否可点。 */
int xfer_busy(void) {
    int b;
    EnterCriticalSection(&s_x.lock);
    b = (s_x.cur_batch != 0 || s_x.q_head != NULL);
    LeaveCriticalSection(&s_x.lock);
    return b;
}

/* 用户取消。两件事一起做，因为"取消"不分方向：
 *   - 发：标记在发的批次，发送线程每块之后就会看到
 *   - 收：置个标志，listen 线程在两帧之间处理（s_rx 只归它所有）
 * 已经排队但还没开始发的批次也一并取消 —— 用户点了取消，
 * 不该还有一队文件在后面等着发出去。 */
void xfer_cancel(void) {
    EnterCriticalSection(&s_x.lock);
    if (s_x.cur_batch) {
        s_x.cancel_batch = s_x.cur_batch;
    } else if (s_x.q_head) {
        s_x.cancel_batch = s_x.q_head->batch;
    }
    s_x.cancel_rx = 1;
    LeaveCriticalSection(&s_x.lock);
    SetEvent(s_x.q_evt);      /* 唤醒发送线程，别让它等满 200 ms */
    SetEvent(s_ack.ev);       /* 唤醒可能正卡在等 ACK 的发送线程 */
    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

static int submit(xfer_job *j) {
    int ok = 0;
    EnterCriticalSection(&s_x.lock);
    if (s_x.client != INVALID_SOCKET && s_x.peer_ready) {
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
