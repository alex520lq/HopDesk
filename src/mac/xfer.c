/* =====================================================================
 *  HopDesk - macOS 文件/图片数据通道（客户端侧）
 *
 *  Mac 主动连 Windows 的 data_port。一条连接全双工：
 *    Windows -> Mac  剪贴板里的文件/图片（接收线程读）
 *    Mac -> Windows  剪贴板里的文件/图片（发送线程写）
 *  两个方向共用 socket，但只有各自线程写自己方向，不冲突。
 *
 *  纯 C，不碰 AppKit —— 挂剪贴板由 clipboard.m 里的 clipboard_apply_* 做，
 *  那边负责切到主队列。
 * ===================================================================== */
#include "mac_app.h"
#include "xfer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XFER_MAX_ENTRIES 8192

/* ---------------- 条目 / 作业 ---------------- */

typedef struct {
    char    *full;
    char    *name;
    char    *rel;
    uint64_t size;
    uint64_t mtime;
    uint32_t flags;
} xf_entry;

typedef struct xf_job {
    struct xf_job *next;
    uint32_t  batch;
    int       is_image;
    int       n;
    xf_entry *e;
    uint8_t  *pixels;
    int       w, h, stride;

    /* ---- 重试状态 ----
     * 只有一个游标，没有 per-entry 状态数组：next_i 之前的条目都已确认送达，
     * next_i 之后都还没发。失败就从 next_i 原地重来，不需要记账。 */
    int       next_i;         /* 下一条待发条目 */
    int       ok;             /* 已收到 ACK_OK 的条目数 */
    int       attempt;        /* 连接级（作业级）已重试次数 */
    uint64_t  retry_at;       /* 毫秒时间戳，早于它不发送 */
    uint64_t  give_up_at;     /* 断线重连预算的截止时刻，0 = 未开始计时 */
} xf_job;

typedef struct {
    xf_entry *v;
    int n, cap;
} elist;

/* ---------------- 状态 ---------------- */

static struct {
    int              sock;
    pthread_mutex_t  slock;          /* 保护 sock */
    pthread_mutex_t  qlock;          /* 保护作业队列 */
    pthread_cond_t   qcond;
    int              stop;
    xf_job          *q_head, *q_tail;
    uint32_t         batch_seq;
    uint32_t         chunk;
    pthread_t        th_conn, th_send;

    /* ---- 取消 ----
     * cur_batch 由发送线程在开始一批时设置；cancel_batch 由主线程设置。
     * 发送线程每块之后比一次，所以取消的响应延迟上限是一个分块（256 KB）。 */
    uint32_t         cur_batch;     /* 0 = 当前没有在发的批次 */
    uint32_t         cancel_batch;  /* 0 = 没有取消请求 */
    volatile int     cancel_rx;     /* 接收侧取消请求，由 conn 线程消费 */
    uint32_t         abort_batch;   /* 对端 ABORT 过的批次，0 = 无 */
} s_x;

/* ---------------- 小工具 ---------------- */

/* 发送互斥：send_thread 和接收线程（出错时回 ABORT）会同时往同一个
 * socket 写。一次 send_all 内部可能分多次 write，两线程交错就会把
 * 帧字节搅在一起 —— 锁的粒度必须是"整帧"，不是"单次 write"。 */
static pthread_mutex_t s_txlock = PTHREAD_MUTEX_INITIALIZER;

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_small(int fd, uint8_t type, const uint8_t *p, uint32_t len) {
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);
    pthread_mutex_lock(&s_txlock);
    int ok = send_all(fd, hdr, HOP_HEADER_SIZE);
    if (ok && len) ok = send_all(fd, p, len);
    pthread_mutex_unlock(&s_txlock);
    return ok;
}

/* 每块之后让一次 CPU。传输是后台工作，键鼠才是前台 ——
 * 真正起作用的保障是 send_thread 里的 QOS_CLASS_UTILITY，
 * 这里让一次是给同优先级的其他工作减压。 */
static void yield_cpu(void) { sched_yield(); }

static int mkdir_p(const char *path) {
    char tmp[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static const char *temp_root(void) {
    const char *t = getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
}

/* 相对路径安全校验：不许绝对路径、不许 .. 作为完整分量。
 * "a..b" 是合法文件名，不能一并拒掉。 */
static int rel_safe(const char *rel) {
    if (!rel || !*rel) return 0;
    if (rel[0] == '/') return 0;

    const char *p = rel;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (n == 0) return 0;                 /* 连续的 /，异常 */
        if (*p) ++p;
    }
    return 1;
}

/* ---------------- 条目列表 ---------------- */

static int elist_push(elist *l, xf_entry *e) {
    if (l->n >= XFER_MAX_ENTRIES) return 0;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        xf_entry *nv = (xf_entry *)realloc(l->v, (size_t)nc * sizeof(*nv));
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

/* 递归收集。lstat 而不是 stat：符号链接跳过，避免环和越界。 */
static void walk_into(elist *l, const char *full, const char *rel) {
    struct stat st;
    if (lstat(full, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) return;             /* 不跟随符号链接 */

    const char *bn = strrchr(full, '/');
    bn = bn ? bn + 1 : full;

    xf_entry e;
    memset(&e, 0, sizeof(e));
    e.full  = strdup(full);
    e.name  = strdup(bn);
    e.rel   = rel ? strdup(rel) : NULL;
    e.mtime = (uint64_t)st.st_mtime;

    if (!e.full || !e.name || !e.rel) {
        free(e.full); free(e.name); free(e.rel);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {              /* 设备/FIFO/socket 一律跳过 */
            free(e.full); free(e.name); free(e.rel);
            return;
        }
        e.size  = (uint64_t)st.st_size;
        e.flags = 0;
        if (!elist_push(l, &e)) { free(e.full); free(e.name); free(e.rel); }
        return;
    }

    e.size  = 0;
    e.flags = HOP_FILE_FLAG_DIR;
    if (!elist_push(l, &e)) { free(e.full); free(e.name); free(e.rel); return; }

    DIR *d = opendir(full);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        size_t fl = strlen(full), nl = strlen(de->d_name), rl = strlen(rel);
        char *child = (char *)malloc(fl + nl + 2);
        char *crel  = (char *)malloc(rl + nl + 2);
        if (!child || !crel) { free(child); free(crel); continue; }

        snprintf(child, fl + nl + 2, "%s/%s", full, de->d_name);
        snprintf(crel,  rl + nl + 2, "%s/%s", rel, de->d_name);

        walk_into(l, child, crel);
        free(child);
        free(crel);
    }
    closedir(d);
}

/* ---------------- 发送 ---------------- */

/* 分块：一次 write 发完头 + 分块头 + 数据。
 * 数据直接读进发送缓冲的 CHUNK_HDR 处，省掉一次内存拷贝。
 * 每块带 CRC32：TCP 的 16 位校验和配合网卡 offload 有可能漏检，
 * GB 级传输漏检一次就是静默损坏。 */
#define CHUNK_HDR 25u   /* 5 帧头 + 4 id + 8 off + 4 len + 4 crc */

static int send_chunk(int fd, uint8_t *tx, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc) {
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + len);
    hop_put_be32(tx + 5, id);
    hop_put_be64(tx + 9, off);
    hop_put_be32(tx + 17, len);
    hop_put_be32(tx + 21, crc);
    if (data != tx + CHUNK_HDR) memcpy(tx + CHUNK_HDR, data, len);
    pthread_mutex_lock(&s_txlock);
    int ok = send_all(fd, tx, CHUNK_HDR + len);
    pthread_mutex_unlock(&s_txlock);
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
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint32_t        batch, id;
    int             status;
    int             valid;
} s_ack;

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void ack_clear(void) {
    pthread_mutex_lock(&s_ack.lock);
    s_ack.valid = 0;
    pthread_mutex_unlock(&s_ack.lock);
}

/* 接收线程调用。槽满（上一份没人取）就覆盖 —— 上一份已经超时，
 * 发送端本来就要重发，旧结论没有价值。 */
static void ack_deliver(uint32_t batch, uint32_t id, int status) {
    pthread_mutex_lock(&s_ack.lock);
    s_ack.batch  = batch;
    s_ack.id     = id;
    s_ack.status = status;
    s_ack.valid  = 1;
    pthread_cond_broadcast(&s_ack.cond);
    pthread_mutex_unlock(&s_ack.lock);
}

/* 用户取消了这个批次？每块之后查一次，锁开销可以忽略。 */
static int cancel_requested(uint32_t batch) {
    pthread_mutex_lock(&s_x.qlock);
    int c = (s_x.cancel_batch == batch);
    pthread_mutex_unlock(&s_x.qlock);
    return c;
}

/* 对端中止过这个批次？（ABORT 是批量级的，比如图片超限、接收端建不了目录） */
static int peer_aborted(uint32_t batch) {
    pthread_mutex_lock(&s_x.qlock);
    int a = (s_x.abort_batch == batch);
    pthread_mutex_unlock(&s_x.qlock);
    return a;
}

/* 发送线程调用。返回 status，超时返回 XFER_ACK_TIMEOUT。
 *
 * 用"先查槽再等条件变量"的循环：ACK 可能在任何时刻到达，
 * 只等一次会漏掉"信号早于等待"的情况。
 * 等待切片 100 ms，是为了让取消 / 对端中止能在 100 ms 内被看到 ——
 * 不然就要去折腾 condattr_setclock 让条件变量走单调时钟。 */
static int ack_wait(uint32_t batch, uint32_t id) {
    uint64_t deadline = now_ms() + HOP_XFER_ACK_TIMEOUT_MS;
    for (;;) {
        pthread_mutex_lock(&s_ack.lock);
        int hit = s_ack.valid && s_ack.batch == batch && s_ack.id == id;
        int st  = s_ack.status;
        if (hit) s_ack.valid = 0;
        if (!hit) {
            uint64_t n = now_ms();
            if (n < deadline) {
                uint64_t slice = deadline - n;
                if (slice > 100) slice = 100;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += (long)slice * 1000000L;
                while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&s_ack.cond, &s_ack.lock, &ts);
            }
        }
        pthread_mutex_unlock(&s_ack.lock);
        if (hit) return st;

        /* 对端中止 / 用户取消都不该干等满 10 秒 */
        if (peer_aborted(batch)) return XFER_PEER_ABORT;
        if (cancel_requested(batch)) return XFER_CANCELLED;
        if (s_x.stop) return XFER_SEND_DEAD;      /* 退出时别把 join 拖满 10 秒 */
        if (now_ms() >= deadline) return XFER_ACK_TIMEOUT;
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
static int send_file_entry(int fd, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xf_entry *e) {
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);
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

    int ok = send_small(fd, HOP_MSG_FILE_BEGIN, bp, blen);
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
        int f = open(e->full, O_RDONLY);
        if (f < 0) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (%s)",
                    e->rel, strerror(errno));
            end_status = HOP_FILE_IO_ERROR;
            local_fail = 1;
        } else {
            uint64_t off = 0;
            for (;;) {
                ssize_t got = read(f, tx + CHUNK_HDR, s_x.chunk);
                if (got <= 0) break;
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, (size_t)got);
                if (!send_chunk(fd, tx, id, off, tx + CHUNK_HDR, (uint32_t)got, ccrc)) {
                    close(f);
                    return XFER_SEND_DEAD;
                }
                uint8_t cb[4];
                hop_put_be32(cb, ccrc);
                fcrc = hop_crc32(fcrc, cb, 4);
                off += (uint64_t)got;
                yield_cpu();
                /* 取消的响应粒度 = 一个分块 */
                if (cancel_requested(batch)) {
                    close(f);
                    return XFER_CANCELLED;
                }
            }
            close(f);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    if (!send_small(fd, HOP_MSG_FILE_END, endp, 9)) return XFER_SEND_DEAD;

    /* 本地就没读成功，没必要再等回执：接收端那份注定是残缺的 */
    if (local_fail) return XFER_LOCAL_IO;

    return ack_wait(batch, id);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */

/* 收尾：告诉接收端本批实际送达了多少。
 * ok < total 时接收端会报"部分成功" —— 这是用户唯一能拿到准确结论的地方，
 * 所以 count 必须是真实值，不能像以前那样写死 0。 */
static int send_done(int fd, const xf_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp,      j->batch);
    hop_put_be32(dp + 4,  (uint32_t)j->ok);
    hop_put_be32(dp + 8,  (uint32_t)j->n);
    hop_put_be64(dp + 12, 0);
    return send_small(fd, HOP_MSG_XFER_DONE, dp, 20);
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(int fd, uint32_t batch, uint8_t reason) {
    uint8_t ab[5];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    send_small(fd, HOP_MSG_XFER_ABORT, ab, 5);
}

/* 取当前连接再发 ABORT。sock 由 slock 保护，不能裸读。
 * 锁序固定 slock -> txlock，不会和发送线程反序。 */
static void send_abort(uint32_t batch, uint8_t reason) {
    uint8_t ab[5];
    hop_put_be32(ab, batch);
    ab[4] = reason;

    pthread_mutex_lock(&s_x.slock);
    int fd = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (fd >= 0) send_small(fd, HOP_MSG_XFER_ABORT, ab, 5);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
static void send_ack(int fd, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(fd, HOP_MSG_FILE_ACK, b, 9);
}

static int run_job(int fd, uint8_t *tx, xf_job *j) {
    if (j->is_image) {
        uint8_t bp[20];
        hop_put_be32(bp, j->batch);
        hop_put_be32(bp + 4, 1);
        hop_put_be32(bp + 8, (uint32_t)j->w);
        hop_put_be32(bp + 12, (uint32_t)j->h);
        hop_put_be32(bp + 16, (uint32_t)j->stride);
        if (!send_small(fd, HOP_MSG_IMAGE_BEGIN, bp, 20)) return JOB_NET_DOWN;

        uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
        uint64_t off = 0;
        while (off < total) {
            uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
            uint32_t crc = hop_crc32(0, j->pixels + off, n);
            if (!send_chunk(fd, tx, 1, off, j->pixels + off, n, crc)) return JOB_NET_DOWN;
            off += n;
            yield_cpu();
            if (cancel_requested(j->batch)) return JOB_CANCELLED;
        }

        uint8_t endp[9];
        hop_put_be32(endp, 1);
        endp[4] = HOP_FILE_OK;
        hop_put_be32(endp + 5, 0);      /* 图片不做整文件 CRC，块 CRC 已足够 */
        if (!send_small(fd, HOP_MSG_FILE_END, endp, 9)) return JOB_NET_DOWN;

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

        const xf_entry *e = &j->e[j->next_i];
        uint32_t id = (uint32_t)(j->next_i + 1);
        int st = HOP_FILE_ACK_OK;
        int tries = 0;

        for (;;) {
            ack_clear();
            st = send_file_entry(fd, tx, j->batch, id, e);
            if (st == HOP_FILE_ACK_OK) break;

            /* 连接层面的问题不在这里耗：交给队列层等重连 */
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
            usleep((useconds_t)d * 1000);
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
 * 调用方必须持有 s_x.qlock。 */
static void queue_unlink(xf_job *prev, xf_job *cur) {
    if (prev) prev->next = cur->next;
    else      s_x.q_head = cur->next;
    if (s_x.q_tail == cur) s_x.q_tail = prev;
}

static void job_free(xf_job *j) {
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

static void *send_thread(void *arg) {
    (void)arg;

    /* 主动降到 UTILITY。控制通道承载的是键鼠，主队列是 USER_INTERACTIVE
     * 级别 —— 降级之后输入路径永远能直接抢占传输线程，
     * 不会出现"传文件时鼠标发涩"。 */
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    uint8_t *tx = (uint8_t *)malloc(CHUNK_HDR + s_x.chunk);
    if (!tx) { hop_log(HOP_LOG_ERROR, "xfer: tx alloc failed"); return NULL; }

    for (;;) {
        xf_job *j = NULL, *expired = NULL;
        int fd;

        pthread_mutex_lock(&s_x.qlock);
        /* 等到"有事可做"：有作业到期（或过期），或者被 stop 唤醒。
         * 200 ms 一轮 —— 重连退避和取消都靠这个轮询推进。 */
        for (;;) {
            if (s_x.stop) break;
            uint64_t n = now_ms();
            xf_job *p;
            for (p = s_x.q_head; p; p = p->next)
                if ((p->give_up_at && n >= p->give_up_at) || p->retry_at <= n) break;
            if (p) break;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s_x.qcond, &s_x.qlock, &ts);
        }

        if (s_x.stop) {
            /* 退出时不再发新东西：队列直接释放，避免在这里空转 */
            xf_job *p = s_x.q_head;
            s_x.q_head = s_x.q_tail = NULL;
            pthread_mutex_unlock(&s_x.qlock);
            while (p) { xf_job *nx = p->next; job_free(p); p = nx; }
            break;
        }

        uint64_t now = now_ms();
        xf_job *prev = NULL;
        xf_job *cur  = s_x.q_head;
        while (cur) {
            /* 重连预算用完：连接在不在都放弃 */
            if (cur->give_up_at && now >= cur->give_up_at) {
                expired = cur;
                queue_unlink(prev, cur);
                expired->next = NULL;
                break;
            }
            if (cur->retry_at <= now) {
                j = cur;
                queue_unlink(prev, cur);
                break;
            }
            prev = cur;
            cur  = cur->next;
        }
        if (j) s_x.cur_batch = j->batch;
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                    expired->batch, expired->ok, expired->n);
            job_free(expired);
            continue;
        }
        if (!j) continue;

        pthread_mutex_lock(&s_x.slock);
        fd = s_x.sock;
        pthread_mutex_unlock(&s_x.slock);

        int r = (fd >= 0) ? run_job(fd, tx, j) : JOB_NET_DOWN;

        pthread_mutex_lock(&s_x.qlock);
        s_x.cur_batch = 0;
        pthread_mutex_unlock(&s_x.qlock);

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
                pthread_mutex_lock(&s_x.qlock);
                if (s_x.q_tail) s_x.q_tail->next = j;
                else            s_x.q_head = j;
                s_x.q_tail = j;
                pthread_cond_signal(&s_x.qcond);
                pthread_mutex_unlock(&s_x.qlock);
                continue;
            }
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned after %d attempt(s), %d/%d delivered",
                    j->batch, j->attempt, j->ok, j->n);
        } else if (r == JOB_CANCELLED) {
            if (fd >= 0) send_abort_msg(fd, j->batch, HOP_ABORT_CANCEL);
        } else if (r == JOB_DONE) {
            if (j->ok < j->n)
                hop_log(HOP_LOG_WARN, "xfer: batch %u finished %d/%d",
                        j->batch, j->ok, j->n);
            if (fd >= 0) send_done(fd, j);
        }
        job_free(j);
    }

    free(tx);
    return NULL;
}

/* ---------------- 写盘队列 ----------------
 *
 * 网络线程只做"收包 + 校验 + 入队"，写盘交给独立线程。
 *
 * 队列有界（RX_Q_SLOTS）。满了网络线程就阻塞在 wq_push，
 * recv 随之停止 → TCP 接收窗口收缩 → 发送端自动减速。
 * 这就是显式背压：内存上界 = RX_Q_SLOTS × chunk，不随传输时长增长。
 * 文件描述符由写线程独占，避免"网络线程关文件、写线程还在写"的竞态。
 */
#define RX_Q_SLOTS 8

enum { RX_OPEN = 1, RX_WRITE, RX_CLOSE };

typedef struct {
    int      kind;
    char     path[4096];
    uint64_t off;
    uint32_t len;
    uint8_t *data;
} rx_task;

static struct {
    rx_task         ring[RX_Q_SLOTS];
    int             head, tail, count, busy;
    pthread_mutex_t lock;
    pthread_cond_t  cv_work, cv_space;
    pthread_t       th;
    int             f;          /* 写线程独占 */
    int             err;        /* 写盘失败标记，主路径读 */
    int             stop;
} s_wq;

static void wq_task_run(rx_task *t) {
    switch (t->kind) {
        case RX_OPEN:
            if (s_wq.f >= 0) { close(s_wq.f); s_wq.f = -1; }
            s_wq.f = open(t->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (s_wq.f < 0) {
                hop_log(HOP_LOG_WARN, "xfer: cannot create %s (%s)",
                        t->path, strerror(errno));
                s_wq.err = 1;
            }
            break;

        case RX_WRITE:
            if (s_wq.f >= 0) {
                if (lseek(s_wq.f, (off_t)t->off, SEEK_SET) < 0) { s_wq.err = 1; break; }
                size_t done = 0;
                while (done < t->len) {
                    ssize_t w = write(s_wq.f, t->data + done, t->len - done);
                    if (w <= 0) {
                        hop_log(HOP_LOG_WARN, "xfer: write failed at %llu (%s)",
                                (unsigned long long)t->off, strerror(errno));
                        s_wq.err = 1;
                        break;
                    }
                    done += (size_t)w;
                }
            }
            break;

        case RX_CLOSE:
            if (s_wq.f >= 0) { close(s_wq.f); s_wq.f = -1; }
            break;
    }
}

static void *writer_thread(void *arg) {
    (void)arg;
    /* 写盘也是后台工作，同样降级，别跟输入路径抢 CPU */
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    for (;;) {
        pthread_mutex_lock(&s_wq.lock);
        while (s_wq.count == 0 && !s_wq.stop)
            pthread_cond_wait(&s_wq.cv_work, &s_wq.lock);
        if (s_wq.count == 0 && s_wq.stop) {
            pthread_mutex_unlock(&s_wq.lock);
            break;
        }
        rx_task t = s_wq.ring[s_wq.head];
        s_wq.head = (s_wq.head + 1) % RX_Q_SLOTS;
        s_wq.count--;
        s_wq.busy = 1;
        pthread_mutex_unlock(&s_wq.lock);

        wq_task_run(&t);

        pthread_mutex_lock(&s_wq.lock);
        s_wq.busy = 0;
        pthread_cond_broadcast(&s_wq.cv_space);
        pthread_mutex_unlock(&s_wq.lock);
        free(t.data);
    }
    if (s_wq.f >= 0) { close(s_wq.f); s_wq.f = -1; }
    return NULL;
}

/* 入队。队列满时阻塞 —— 这就是背压的着力点。 */
static int wq_push(const rx_task *t) {
    pthread_mutex_lock(&s_wq.lock);
    while (s_wq.count >= RX_Q_SLOTS && !s_wq.stop)
        pthread_cond_wait(&s_wq.cv_space, &s_wq.lock);
    if (s_wq.stop) {
        pthread_mutex_unlock(&s_wq.lock);
        return 0;
    }
    s_wq.ring[s_wq.tail] = *t;
    s_wq.tail = (s_wq.tail + 1) % RX_Q_SLOTS;
    s_wq.count++;
    pthread_cond_signal(&s_wq.cv_work);
    pthread_mutex_unlock(&s_wq.lock);
    return 1;
}

/* 等队列排空且写线程空闲。关文件、比对文件 CRC 之前必须调用。 */
static void wq_drain(void) {
    pthread_mutex_lock(&s_wq.lock);
    while ((s_wq.count > 0 || s_wq.busy) && !s_wq.stop)
        pthread_cond_wait(&s_wq.cv_space, &s_wq.lock);
    pthread_mutex_unlock(&s_wq.lock);
}

static int wq_start(void) {
    memset(&s_wq, 0, sizeof(s_wq));
    s_wq.f = -1;
    pthread_mutex_init(&s_wq.lock, NULL);
    pthread_cond_init(&s_wq.cv_work, NULL);
    pthread_cond_init(&s_wq.cv_space, NULL);
    if (pthread_create(&s_wq.th, NULL, writer_thread, NULL) != 0) return 0;
    return 1;
}

/* 停写线程：先让它把队列里的活干完（数据已经收下来了，丢了就是损坏），
 * 再置 stop 唤醒，最后 join。 */
static void wq_stop(void) {
    if (!s_wq.th) return;
    wq_drain();
    pthread_mutex_lock(&s_wq.lock);
    s_wq.stop = 1;
    pthread_cond_broadcast(&s_wq.cv_work);
    pthread_cond_broadcast(&s_wq.cv_space);
    pthread_mutex_unlock(&s_wq.lock);

    pthread_join(s_wq.th, NULL);
    s_wq.th = 0;
    pthread_mutex_destroy(&s_wq.lock);
    pthread_cond_destroy(&s_wq.cv_work);
    pthread_cond_destroy(&s_wq.cv_space);
}

/* ---------------- 接收 ---------------- */

static struct {
    uint32_t batch;
    int      active;
    uint32_t dead_batch;    /* 已作废的批次（本地取消 / 对端中止）。
                             * 它之后的帧一律忽略，且不再为它建目录 ——
                             * 否则取消之后对端再发一帧就把目录又建回来了。 */
    char     dir[4096];
    char    *cur_path;
    uint32_t cur_id;
    int      cur_is_image;
    int      cur_is_dir;    /* 目录在 BEGIN 时已登记，FILE_END 不能再登记一次 */
    int      cur_fail;      /* 本条目已判死：-1 = 正常，否则是 FILE_END 要回的 ACK 状态。
                             * 坏路径和坏数据都在这里落定，FILE_END 只负责回执。 */
    uint64_t cur_size;      /* 声明的大小：超出就拒收，防止被灌成无限大 */
    /* 当前文件的校验累加：每块 CRC 折进来，FILE_END 时与发送端比对 */
    uint32_t fcrc;
    uint8_t *img;
    size_t   img_cap, img_len;
    int      img_w, img_h, img_stride;
    char   **tops;
    int      ntop;
} s_rx;

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
    s_rx.dir[0] = '\0';
}

static void rx_reset(void) {
    /* 先把队列排空并关掉文件，再清状态 —— 否则写线程可能还在用旧路径 */
    rx_close_file();
    rx_clear_state();
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)ftw;
    if (flag == FTW_DP) rmdir(path);
    else                unlink(path);
    return 0;
}

/* 递归删掉本批临时目录。
 * 只在"用户取消 / 对端中止"时调用：失败批次要保留已送达的文件，
 * 成功批次的文件就是产物本身，都不能删。 */
static void rx_discard_dir(void) {
    if (!s_rx.dir[0]) return;
    /* 路径是我们自己拼的，但删除是不可逆操作 —— 仍然验一次根目录 */
    if (!strstr(s_rx.dir, "/HopDesk/")) {
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete %s", s_rx.dir);
        return;
    }
    if (nftw(s_rx.dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
        hop_log(HOP_LOG_WARN, "xfer: failed to remove %s (%s)", s_rx.dir, strerror(errno));
    s_rx.dir[0] = '\0';
}

static int rx_add_top(const char *p) {
    /* 重发会让同一个顶层条目再来一次（比如 ACK 超时但文件其实写成功了），
     * 不去重的话剪贴板里会出现两份同样的路径。ntop 只是顶层条目数，
     * 线性扫一遍的代价可以忽略。 */
    for (int i = 0; i < s_rx.ntop; ++i)
        if (strcmp(s_rx.tops[i], p) == 0) return 1;

    char **nv = (char **)realloc(s_rx.tops, (size_t)(s_rx.ntop + 1) * sizeof(char *));
    if (!nv) return 0;
    s_rx.tops = nv;
    s_rx.tops[s_rx.ntop] = strdup(p);
    if (!s_rx.tops[s_rx.ntop]) return 0;
    s_rx.ntop++;
    return 1;
}

static void rx_begin_batch(uint32_t batch) {
    /* 清掉上一批遗留的"接收侧取消"请求。
     *
     * 用户可能在"没有接收在途"的时候点取消（比如只在发文件），
     * 那时 cancel_rx 置了 1 却没人消费 —— 它会一直躺到对端下一次
     * 开始发文件，在第一个数据帧之后被 test_and_set 吃掉，
     * 把刚建立的新批次误判成"用户要取消"。批次一换，旧请求就该作废。 */
    __sync_lock_test_and_set(&s_x.cancel_rx, 0);

    /* 作废过的批次又来了（对端还没收到取消）：回一个 ABORT 让它立刻放手，
     * 千万别为它重建目录 —— 用户已经说过不要了。 */
    if (batch == s_rx.dead_batch) {
        send_abort(batch, HOP_ABORT_CANCEL);
        return;
    }
    if (s_rx.active && s_rx.batch == batch) return;   /* 重发同一个批次：接着用 */
    rx_reset();
    s_rx.dead_batch = 0;                              /* 换批次了，墓碑清掉 */

    snprintf(s_rx.dir, sizeof(s_rx.dir), "%s/HopDesk/%u", temp_root(), batch);
    if (mkdir_p(s_rx.dir) != 0) {
        send_abort(batch, HOP_ABORT_DISK);
        s_rx.dir[0] = '\0';
        return;
    }

    s_rx.active = 1;
    s_rx.batch = batch;
}

static void rx_finish_batch(void) {
    rx_task t;
    memset(&t, 0, sizeof(t));
    t.kind = RX_CLOSE;
    wq_push(&t);
    wq_drain();

    if (s_rx.cur_is_image && s_rx.img && s_rx.img_len)
        clipboard_apply_image(s_rx.img, s_rx.img_w, s_rx.img_h, s_rx.img_stride);
    else if (s_rx.ntop > 0)
        clipboard_apply_paths((const char *const *)s_rx.tops, s_rx.ntop);

    hop_log(HOP_LOG_INFO, "xfer: batch %u done (%d items)", s_rx.batch, s_rx.ntop);
    rx_reset();
}

static void on_data_msg(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;

    switch (type) {
        case HOP_MSG_XFER_HELLO:
            if (len >= 1 && p[0] != HOP_XFER_PROTO_VER)
                hop_log(HOP_LOG_ERROR,
                        "xfer: peer speaks data protocol v%u, we speak v%u — "
                        "两端版本不一致，请把两边都重建",
                        p[0], (unsigned)HOP_XFER_PROTO_VER);
            else
                hop_log(HOP_LOG_INFO, "xfer: peer data channel ready (v%u)",
                        (unsigned)HOP_XFER_PROTO_VER);
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
            if (29u + nlen + plen > len) return;

            rx_begin_batch(batch);
            if (!s_rx.active) return;

            char rel[HOP_PATH_LEN_MAX + 1];
            uint32_t cp = plen < HOP_PATH_LEN_MAX ? plen : HOP_PATH_LEN_MAX;
            memcpy(rel, p + 29 + nlen, cp);
            rel[cp] = '\0';

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

            if (!rel_safe(rel)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", rel);
                s_rx.cur_fail = HOP_FILE_ACK_BADPATH;   /* 单个文件的问题，别拖垮整批 */
                break;
            }

            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", s_rx.dir, rel);

            s_rx.cur_path = strdup(path);
            s_rx.cur_size = size;

            if (s_rx.cur_is_dir) {
                mkdir_p(path);
                /* 顶层目录才进剪贴板清单 */
                if (!strchr(rel, '/')) rx_add_top(path);
            } else {
                /* 建父目录 */
                char *sep = strrchr(path, '/');
                if (sep) { *sep = '\0'; mkdir_p(path); *sep = '/'; }

                /* 打开交给写线程 —— 描述符归它独占 */
                rx_task t;
                memset(&t, 0, sizeof(t));
                t.kind = RX_OPEN;
                snprintf(t.path, sizeof(t.path), "%s", path);
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
            if (w <= 0 || h <= 0 || w > HOP_IMG_MAX_SIDE || h > HOP_IMG_MAX_SIDE ||
                stride < w * 4 || (uint64_t)stride * (uint64_t)h > HOP_IMG_MAX_BYTES) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                send_abort(s_rx.batch, HOP_ABORT_TOO_LARGE);
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
            if (20u + n > len) return;
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
            uint8_t cb[4];
            hop_put_be32(cb, crc);
            s_rx.fcrc = hop_crc32(s_rx.fcrc, cb, 4);
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
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %s incomplete",
                            s_rx.cur_path ? s_rx.cur_path : "?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %s",
                            (unsigned)status, s_rx.cur_path ? s_rx.cur_path : "?");
                    ack = HOP_FILE_ACK_DISK;
                } else if (s_rx.cur_size > 0 && want != s_rx.fcrc) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %s (expect %08x got %08x)",
                            s_rx.cur_path ? s_rx.cur_path : "?", want, s_rx.fcrc);
                    ack = HOP_FILE_ACK_CRC;
                } else if (s_rx.cur_path && !s_rx.cur_is_dir) {
                    /* 落盘成功、校验通过，才登记为可粘贴条目。
                     * 目录条目不在这里登记 —— 它在 BEGIN 时就已建好并登记。 */
                    const char *tail = s_rx.cur_path + strlen(s_rx.dir) + 1;
                    if (!strchr(tail, '/')) rx_add_top(s_rx.cur_path);
                }
            }

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            {
                pthread_mutex_lock(&s_x.slock);
                int fd = s_x.sock;
                pthread_mutex_unlock(&s_x.slock);
                if (fd >= 0) send_ack(fd, s_rx.batch, id, ack);
            }
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
            pthread_mutex_lock(&s_x.qlock);
            s_x.abort_batch = batch;
            pthread_mutex_unlock(&s_x.qlock);
            pthread_mutex_lock(&s_ack.lock);
            pthread_cond_broadcast(&s_ack.cond);
            pthread_mutex_unlock(&s_ack.lock);

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。 */
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

/* ---------------- 连接 ---------------- */

static int do_connect(void) {
    const char *host = g_mac.cfg.peer_host[0] ? g_mac.cfg.peer_host : "192.168.31.8";
    uint16_t port = g_mac.cfg.data_port ? g_mac.cfg.data_port : HOP_DATA_PORT;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int s = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        s = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (s < 0) continue;
        if (connect(s, r->ai_addr, r->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}

/* 用户在本端取消接收：通知对端、删掉半成品、立墓碑。
 * 由 conn 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.active) return;
    uint32_t batch = s_rx.batch;
    send_abort(batch, HOP_ABORT_CANCEL);
    rx_close_file();
    rx_discard_dir();
    s_rx.dead_batch = batch;
    rx_clear_state();
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}

static void *conn_thread(void *arg) {
    (void)arg;
    hop_decoder *dec = hop_decoder_new(NULL, on_data_msg);
    if (!dec) return NULL;

    int backoff = 1;
    while (!s_x.stop) {
        int s = do_connect();
        if (s < 0) {
            /* 退避要能被 stop 打断 —— 否则退出时要等满 10 秒 */
            for (int i = 0; i < backoff * 10 && !s_x.stop; ++i)
                usleep(100 * 1000);
            backoff = backoff < 10 ? backoff * 2 : 10;
            continue;
        }
        backoff = 1;

        int bufsz = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

        /* 发送超时：ABORT 是从接收线程发的，如果发送线程正卡在 write 里
         * （对端窗口满），接收线程拿不到发送锁就会跟着卡死 —— 那是死锁。
         * 给个上限，卡住就当作传输失败，别把整个通道拖死。
         * 256 KB 块在 100 Mbps 链路上只要 20 ms，5 秒是极宽的余量。 */
        struct timeval sndto;
        sndto.tv_sec = 5;
        sndto.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

        /* 接收超时不是错误处理，是给取消请求开一条唤醒通道：
         * 对端暂停发送时 read 会一直阻塞，而 s_rx 只归本线程所有，
         * 取消就只能干等 —— 用户点了"取消"，界面 30 秒没反应。
         * 200 ms 醒一次，代价是空闲时每秒 5 次空转，可以忽略。 */
        struct timeval rxto;
        rxto.tv_sec = 0;
        rxto.tv_usec = 200 * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = s;
        pthread_mutex_unlock(&s_x.slock);

        pthread_mutex_lock(&s_x.qlock);
        s_x.abort_batch = 0;          /* 新会话，上一轮的 ABORT 不再算数 */
        pthread_mutex_unlock(&s_x.qlock);

        hop_decoder_reset(dec);
        rx_reset();

        uint8_t hello = HOP_XFER_PROTO_VER;
        send_small(s, HOP_MSG_XFER_HELLO, &hello, 1);
        hop_log(HOP_LOG_INFO, "xfer: data channel connected");

        char buf[65536];
        for (;;) {
            ssize_t r = read(s, buf, sizeof(buf));

            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 不是断线，只是这 200 ms 没数据：借这次醒来处理取消请求 */
                    if (__sync_lock_test_and_set(&s_x.cancel_rx, 0)) rx_cancel_local();
                    continue;
                }
                break;
            }
            if (r == 0) break;                 /* 对端正常关闭 */

            hop_decoder_feed(dec, (const uint8_t *)buf, (uint32_t)r);
            if (__sync_lock_test_and_set(&s_x.cancel_rx, 0)) rx_cancel_local();
        }

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = -1;
        pthread_mutex_unlock(&s_x.slock);
        close(s);
        /* 只关文件，不清批次：发送端会带着同一个 batch 号回来，
         * 从断点条目继续发。这里清掉就变成"从头重传整批"了。 */
        rx_close_file();
        if (!s_x.stop) hop_log(HOP_LOG_WARN, "xfer: data channel closed");
    }

    hop_decoder_free(dec);
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

void xfer_init(const hop_config *cfg) {
    memset(&s_x, 0, sizeof(s_x));
    s_x.sock = -1;

    int ck = cfg->xfer_chunk_kb;
    if (ck < 64) ck = 64;
    if (ck > 4096) ck = 4096;
    s_x.chunk = (uint32_t)ck * 1024u;

    pthread_mutex_init(&s_x.slock, NULL);
    pthread_mutex_init(&s_x.qlock, NULL);
    pthread_cond_init(&s_x.qcond, NULL);
    pthread_mutex_init(&s_ack.lock, NULL);
    pthread_cond_init(&s_ack.cond, NULL);

    /* 写盘线程要先起来，接收路径一进来就会往队列里塞 */
    if (!wq_start()) {
        hop_log(HOP_LOG_ERROR, "xfer: writer thread start failed");
        return;
    }

    pthread_create(&s_x.th_send, NULL, send_thread, NULL);
    pthread_create(&s_x.th_conn, NULL, conn_thread, NULL);

    hop_log(HOP_LOG_INFO, "xfer: chunk %u KB", ck);
}

void xfer_shutdown(void) {
    s_x.stop = 1;

    pthread_mutex_lock(&s_x.slock);
    if (s_x.sock >= 0) { shutdown(s_x.sock, SHUT_RDWR); close(s_x.sock); s_x.sock = -1; }
    pthread_mutex_unlock(&s_x.slock);

    pthread_mutex_lock(&s_x.qlock);
    pthread_cond_broadcast(&s_x.qcond);
    pthread_mutex_unlock(&s_x.qlock);

    pthread_mutex_lock(&s_ack.lock);
    pthread_cond_broadcast(&s_ack.cond);   /* 踢醒可能正卡在等 ACK 的发送线程 */
    pthread_mutex_unlock(&s_ack.lock);

    /* 必须 join：两个线程都会碰写盘队列，销毁队列时它们还活着就是释放后使用。
     * read/write 都是阻塞的，靠上面 shutdown(sock) 和 SO_SNDTIMEO 让它们返回。 */
    pthread_join(s_x.th_conn, NULL);
    pthread_join(s_x.th_send, NULL);

    rx_reset();      /* 先把写盘队列排空并关掉文件，再停写线程 */
    wq_stop();
    pthread_mutex_destroy(&s_ack.lock);
    pthread_cond_destroy(&s_ack.cond);
}

int xfer_connected(void) {
    int c;
    pthread_mutex_lock(&s_x.slock);
    c = (s_x.sock >= 0);
    pthread_mutex_unlock(&s_x.slock);
    return c;
}

/* 有没有正在进行的传输？菜单项据此决定是否可点。 */
int xfer_busy(void) {
    int b;
    pthread_mutex_lock(&s_x.qlock);
    b = (s_x.cur_batch != 0 || s_x.q_head != NULL);
    pthread_mutex_unlock(&s_x.qlock);
    return b;
}

/* 用户取消。两件事一起做，因为"取消"不分方向：
 *   - 发：标记在发的批次，发送线程每块之后就会看到
 *   - 收：置个标志，conn 线程在两帧之间处理（s_rx 只归它所有）
 * 已经排队但还没开始发的批次也一并取消。 */
void xfer_cancel(void) {
    pthread_mutex_lock(&s_x.qlock);
    if (s_x.cur_batch) {
        s_x.cancel_batch = s_x.cur_batch;
    } else if (s_x.q_head) {
        s_x.cancel_batch = s_x.q_head->batch;
    }
    s_x.cancel_rx = 1;
    pthread_cond_signal(&s_x.qcond);      /* 唤醒发送线程，别让它等满 200 ms */
    pthread_mutex_unlock(&s_x.qlock);

    pthread_mutex_lock(&s_ack.lock);
    pthread_cond_broadcast(&s_ack.cond);  /* 唤醒可能正卡在等 ACK 的发送线程 */
    pthread_mutex_unlock(&s_ack.lock);

    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

static int submit(xf_job *j) {
    pthread_mutex_lock(&s_x.qlock);
    if (s_x.q_tail) s_x.q_tail->next = j;
    else            s_x.q_head = j;
    s_x.q_tail = j;
    pthread_cond_signal(&s_x.qcond);
    pthread_mutex_unlock(&s_x.qlock);
    return 1;
}

int xfer_submit_paths(const char *const *paths, int n) {
    if (!paths || n <= 0) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop %d path(s)", n);
        return 0;
    }

    elist l;
    memset(&l, 0, sizeof(l));

    for (int i = 0; i < n; ++i) {
        if (!paths[i] || !paths[i][0]) continue;
        const char *bn = strrchr(paths[i], '/');
        bn = bn ? bn + 1 : paths[i];
        walk_into(&l, paths[i], bn);
    }

    if (l.n == 0) { elist_free(&l); return 0; }

    xf_job *j = (xf_job *)calloc(1, sizeof(*j));
    if (!j) { elist_free(&l); return 0; }

    j->batch = __sync_add_and_fetch(&s_x.batch_seq, 1);
    j->n = l.n;
    j->e = l.v;

    uint64_t total = 0;
    for (int i = 0; i < l.n; ++i) total += l.v[i].size;

    hop_log(HOP_LOG_INFO, "xfer: queued batch %u, %d entries, %.1f MB",
            j->batch, j->n, (double)total / 1048576.0);

    return submit(j);
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

    xf_job *j = (xf_job *)calloc(1, sizeof(*j));
    if (!j) { free(copy); return 0; }

    j->batch = __sync_add_and_fetch(&s_x.batch_seq, 1);
    j->is_image = 1;
    j->pixels = copy;
    j->w = w; j->h = h; j->stride = stride;

    hop_log(HOP_LOG_INFO, "xfer: queued image %dx%d (%.1f MB)",
            w, h, (double)total / 1048576.0);

    return submit(j);
}
