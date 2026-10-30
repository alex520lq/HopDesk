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
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
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
} s_x;

/* ---------------- 小工具 ---------------- */

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
    if (!send_all(fd, hdr, HOP_HEADER_SIZE)) return 0;
    if (len && !send_all(fd, p, len)) return 0;
    return 1;
}

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

#define CHUNK_HDR 21u

static int send_chunk(int fd, uint8_t *tx, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len) {
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 16 + len);
    hop_put_be32(tx + 5, id);
    hop_put_be64(tx + 9, off);
    hop_put_be32(tx + 17, len);
    if (data != tx + CHUNK_HDR) memcpy(tx + CHUNK_HDR, data, len);
    return send_all(fd, tx, CHUNK_HDR + len);
}

static int send_file_entry(int fd, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xf_entry *e) {
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);
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

    int ok = send_small(fd, HOP_MSG_FILE_BEGIN, bp, blen);
    free(bp);
    if (!ok) return 0;

    if (!(e->flags & HOP_FILE_FLAG_DIR) && e->size > 0) {
        int f = open(e->full, O_RDONLY);
        if (f < 0) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, skipping %s (%s)",
                    e->rel, strerror(errno));
            uint8_t endp[5];
            hop_put_be32(endp, id);
            endp[4] = 1;
            send_small(fd, HOP_MSG_FILE_END, endp, 5);
            return 1;
        }

        uint64_t off = 0;
        for (;;) {
            ssize_t got = read(f, tx + CHUNK_HDR, s_x.chunk);
            if (got <= 0) break;
            if (!send_chunk(fd, tx, id, off, tx + CHUNK_HDR, (uint32_t)got)) {
                close(f);
                return 0;
            }
            off += (uint64_t)got;
        }
        close(f);
    }

    uint8_t endp[5];
    hop_put_be32(endp, id);
    endp[4] = 0;
    return send_small(fd, HOP_MSG_FILE_END, endp, 5);
}

static int run_job(int fd, uint8_t *tx, xf_job *j) {
    if (j->is_image) {
        uint8_t bp[20];
        hop_put_be32(bp, j->batch);
        hop_put_be32(bp + 4, 1);
        hop_put_be32(bp + 8, (uint32_t)j->w);
        hop_put_be32(bp + 12, (uint32_t)j->h);
        hop_put_be32(bp + 16, (uint32_t)j->stride);
        if (!send_small(fd, HOP_MSG_IMAGE_BEGIN, bp, 20)) return 0;

        uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
        uint64_t off = 0;
        while (off < total) {
            uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
            if (!send_chunk(fd, tx, 1, off, j->pixels + off, n)) return 0;
            off += n;
        }

        uint8_t endp[5];
        hop_put_be32(endp, 1);
        endp[4] = 0;
        if (!send_small(fd, HOP_MSG_FILE_END, endp, 5)) return 0;
    } else {
        for (int i = 0; i < j->n; ++i)
            if (!send_file_entry(fd, tx, j->batch, (uint32_t)(i + 1), &j->e[i]))
                return 0;
    }

    uint8_t dp[16];
    hop_put_be32(dp, j->batch);
    hop_put_be32(dp + 4, (uint32_t)j->n);
    hop_put_be64(dp + 8, 0);
    return send_small(fd, HOP_MSG_XFER_DONE, dp, 16);
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
    uint8_t *tx = (uint8_t *)malloc(CHUNK_HDR + s_x.chunk);
    if (!tx) { hop_log(HOP_LOG_ERROR, "xfer: tx alloc failed"); return NULL; }

    for (;;) {
        pthread_mutex_lock(&s_x.qlock);
        while (!s_x.q_head && !s_x.stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s_x.qcond, &s_x.qlock, &ts);
        }
        if (s_x.stop && !s_x.q_head) { pthread_mutex_unlock(&s_x.qlock); break; }

        xf_job *j = s_x.q_head;
        s_x.q_head = j->next;
        if (!s_x.q_head) s_x.q_tail = NULL;
        pthread_mutex_unlock(&s_x.qlock);   /* 发送期间绝不能持队列锁：
                                             * 主队列的提交会在这里阻塞 */

        pthread_mutex_lock(&s_x.slock);
        int fd = s_x.sock;
        pthread_mutex_unlock(&s_x.slock);

        if (fd >= 0 && !run_job(fd, tx, j))
            hop_log(HOP_LOG_WARN, "xfer: send aborted (batch %u)", j->batch);
        job_free(j);
    }

    free(tx);
    return NULL;
}

/* ---------------- 接收 ---------------- */

static struct {
    uint32_t batch;
    int      active;
    char     dir[4096];
    int      f;
    int      cur_is_image;
    uint8_t *img;
    size_t   img_cap, img_len;
    int      img_w, img_h, img_stride;
    char   **tops;
    int      ntop;
} s_rx;

static void rx_reset(void) {
    if (s_rx.f >= 0) { close(s_rx.f); s_rx.f = -1; }
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

static int rx_add_top(const char *p) {
    char **nv = (char **)realloc(s_rx.tops, (size_t)(s_rx.ntop + 1) * sizeof(char *));
    if (!nv) return 0;
    s_rx.tops = nv;
    s_rx.tops[s_rx.ntop] = strdup(p);
    if (!s_rx.tops[s_rx.ntop]) return 0;
    s_rx.ntop++;
    return 1;
}

static void rx_begin_batch(uint32_t batch) {
    if (s_rx.active && s_rx.batch == batch) return;
    rx_reset();

    snprintf(s_rx.dir, sizeof(s_rx.dir), "%s/HopDesk/%u", temp_root(), batch);
    if (mkdir_p(s_rx.dir) != 0) return;

    s_rx.active = 1;
    s_rx.batch = batch;
    s_rx.f = -1;
}

static void rx_finish_batch(void) {
    if (s_rx.f >= 0) { close(s_rx.f); s_rx.f = -1; }

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
            hop_log(HOP_LOG_INFO, "xfer: peer data channel ready");
            break;

        case HOP_MSG_FILE_BEGIN: {
            if (len < 27) return;
            uint32_t batch = hop_get_be32(p);
            uint32_t id    = hop_get_be32(p + 4);
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

            if (!rel_safe(rel)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", rel);
                return;
            }

            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", s_rx.dir, rel);

            if (s_rx.f >= 0) { close(s_rx.f); s_rx.f = -1; }
            s_rx.cur_is_image = 0;

            if (flags & HOP_FILE_FLAG_DIR) {
                mkdir_p(path);
                if (!strchr(rel, '/')) rx_add_top(path);
            } else {
                /* 建父目录 */
                char *sep = strrchr(path, '/');
                if (sep) { *sep = '\0'; mkdir_p(path); *sep = '/'; }
                s_rx.f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (s_rx.f < 0)
                    hop_log(HOP_LOG_WARN, "xfer: cannot create %s (%s)", path, strerror(errno));
                if (!strchr(rel, '/')) rx_add_top(path);
            }
            (void)id;
            break;
        }

        case HOP_MSG_IMAGE_BEGIN:
            if (len < 20) return;
            rx_begin_batch(hop_get_be32(p));
            if (!s_rx.active) return;
            s_rx.cur_is_image = 1;
            s_rx.img_w = (int)hop_get_be32(p + 8);
            s_rx.img_h = (int)hop_get_be32(p + 12);
            s_rx.img_stride = (int)hop_get_be32(p + 16);
            s_rx.img_len = 0;
            break;

        case HOP_MSG_FILE_CHUNK: {
            if (len < 16) return;
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

            if (s_rx.f < 0) return;
            if (lseek(s_rx.f, (off_t)off, SEEK_SET) < 0) return;
            size_t done = 0;
            while (done < n) {
                ssize_t w = write(s_rx.f, data + done, n - done);
                if (w <= 0) break;
                done += (size_t)w;
            }
            break;
        }

        case HOP_MSG_FILE_END:
            if (s_rx.f >= 0) { close(s_rx.f); s_rx.f = -1; }
            break;

        case HOP_MSG_XFER_DONE:
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

static void *conn_thread(void *arg) {
    (void)arg;
    hop_decoder *dec = hop_decoder_new(NULL, on_data_msg);
    if (!dec) return NULL;

    int backoff = 1;
    while (!s_x.stop) {
        int s = do_connect();
        if (s < 0) {
            sleep((unsigned)backoff);
            backoff = backoff < 10 ? backoff * 2 : 10;
            continue;
        }
        backoff = 1;

        int bufsz = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = s;
        pthread_mutex_unlock(&s_x.slock);

        hop_decoder_reset(dec);
        rx_reset();

        uint8_t hello = 1;
        send_small(s, HOP_MSG_XFER_HELLO, &hello, 1);
        hop_log(HOP_LOG_INFO, "xfer: data channel connected");

        char buf[65536];
        for (;;) {
            ssize_t r = read(s, buf, sizeof(buf));
            if (r <= 0) break;
            hop_decoder_feed(dec, (const uint8_t *)buf, (uint32_t)r);
        }

        pthread_mutex_lock(&s_x.slock);
        s_x.sock = -1;
        pthread_mutex_unlock(&s_x.slock);
        close(s);
        rx_reset();
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

    s_rx.f = -1;

    pthread_create(&s_x.th_send, NULL, send_thread, NULL);
    pthread_create(&s_x.th_conn, NULL, conn_thread, NULL);
    pthread_detach(s_x.th_send);
    pthread_detach(s_x.th_conn);

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
}

int xfer_connected(void) {
    int c;
    pthread_mutex_lock(&s_x.slock);
    c = (s_x.sock >= 0);
    pthread_mutex_unlock(&s_x.slock);
    return c;
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
