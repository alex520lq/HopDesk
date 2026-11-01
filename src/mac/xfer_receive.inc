/* Included by xfer.c in the same translation unit; not compiled separately. */
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
    size_t   img_cap, img_len, img_expected;
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
    s_rx.img_cap = s_rx.img_len = s_rx.img_expected = 0;
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
    if(s_rx.active)status_end(HOP_XFER_DIR_RECEIVE,s_rx.batch,HOP_XFER_FAILED,"Receive interrupted");
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
    status_begin(batch,HOP_XFER_DIR_RECEIVE,0,0,"Incoming files",HOP_XFER_RECEIVING);
}

static void rx_finish_batch(uint32_t batch, uint32_t ok, uint32_t total) {
    if (!s_rx.active || s_rx.batch != batch) {
        hop_log(HOP_LOG_WARN, "xfer: stale DONE for batch %u ignored (active=%u)",
                batch, s_rx.active ? s_rx.batch : 0);
        return;
    }

    rx_task t;
    memset(&t, 0, sizeof(t));
    t.kind = RX_CLOSE;
    wq_push(&t);
    wq_drain();

    if (ok != total) {
        /* Keep the staged data for diagnosis/recovery, but never expose a partial
         * folder as a successful clipboard item. */
        hop_log(HOP_LOG_ERROR,
                "xfer: batch %u incomplete (%u/%u entries); staged data kept at %s; clipboard not updated",
                batch, ok, total, s_rx.dir);
    } else if (s_rx.cur_is_image) {
        if (s_rx.img && s_rx.img_len == s_rx.img_expected)
            clipboard_apply_image(s_rx.img, s_rx.img_w, s_rx.img_h, s_rx.img_stride);
        else
            hop_log(HOP_LOG_ERROR, "xfer: incomplete image rejected (%llu/%llu bytes)",
                    (unsigned long long)s_rx.img_len, (unsigned long long)s_rx.img_expected);
    } else if (s_rx.ntop > 0) {
        clipboard_apply_paths((const char *const *)s_rx.tops, s_rx.ntop);
    }

    hop_log(HOP_LOG_INFO, "xfer: batch %u done (%d items, %u/%u ACKed)",
            batch, s_rx.ntop, ok, total);
    pthread_mutex_lock(&s_status_lock);
    s_receive_status.status.files_total=total;
    pthread_mutex_unlock(&s_status_lock);
    if(s_rx.cur_is_image && ok==total && s_rx.img_len==s_rx.img_expected)
        status_file_phase(HOP_XFER_DIR_RECEIVE,batch,0,HOP_XFER_FILE_COMPLETED,1);
    int complete=(ok==total) && (!s_rx.cur_is_image ||
        (s_rx.img && s_rx.img_len==s_rx.img_expected));
    status_end(HOP_XFER_DIR_RECEIVE,batch,complete?HOP_XFER_COMPLETED:
               (s_rx.cur_is_image?HOP_XFER_FAILED:HOP_XFER_PARTIAL),
               complete?"Receive completed":"Receive incomplete; clipboard not updated");
    rx_reset();
}

static void on_data_msg(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    (void)user;

    if (type == HOP_MSG_XFER_HELLO) {
        size_t want = strlen(g_mac.cfg.token);
        int valid = hop_xfer_hello_valid(p, len, g_mac.cfg.token);
        if (!valid) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: peer handshake rejected (protocol/token mismatch)");
            pthread_mutex_lock(&s_x.slock);
            if (s_x.sock >= 0) shutdown(s_x.sock, SHUT_RDWR);
            s_x.peer_ready = 0;
            pthread_mutex_unlock(&s_x.slock);
            return;
        }
        pthread_mutex_lock(&s_x.slock);
        s_x.peer_ready = 1;
        pthread_mutex_unlock(&s_x.slock);
        hop_log(HOP_LOG_INFO,
                want ? "xfer: authenticated peer data channel (v%u)" :
                       "xfer: peer data channel ready (v%u)",
                (unsigned)HOP_XFER_PROTO_VER);
        return;
    }

    pthread_mutex_lock(&s_x.slock);
    int ready = s_x.peer_ready;
    int active_sock = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (!ready) {
        hop_log(HOP_LOG_WARN, "xfer: frame received before handshake; closing peer");
        if (active_sock >= 0) shutdown(active_sock, SHUT_RDWR);
        return;
    }

    switch (type) {

        case HOP_MSG_FILE_BEGIN: {
            hop_xfer_file_begin_msg msg;
            if (!hop_xfer_decode_file_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
                pthread_mutex_lock(&s_x.slock);
                int active = s_x.sock;
                pthread_mutex_unlock(&s_x.slock);
                if (active >= 0) shutdown(active, SHUT_RDWR);
                return;
            }
            uint32_t batch = msg.batch;
            uint32_t id = msg.id;
            uint64_t size = msg.size;
            uint32_t flags = msg.flags;
            uint32_t plen = msg.relpath_len;

            rx_begin_batch(batch);
            if (!s_rx.active) return;

            /* 先登记当前条目，确保坏路径也能在 FILE_END 时回 ACK。 */
            free(s_rx.cur_path);
            s_rx.cur_path     = NULL;
            s_rx.cur_id       = id;
            s_rx.cur_is_image = 0;
            s_rx.cur_is_dir   = (flags & HOP_FILE_FLAG_DIR) ? 1 : 0;
            s_rx.cur_fail     = -1;
            s_rx.cur_size     = 0;
            s_rx.fcrc         = 0;
            s_wq.err          = 0;

            char rel[HOP_PATH_LEN_MAX + 1];
            if (!hop_xfer_relpath_bytes_safe(msg.relpath, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe relative path rejected");
                s_rx.cur_fail = HOP_FILE_ACK_BADPATH;
                break;
            }
            memcpy(rel, msg.relpath, plen);
            rel[plen] = '\0';


            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", s_rx.dir, rel);

            s_rx.cur_path = strdup(path);
            s_rx.cur_size = size;
            status_add_received_file(batch,id,rel,size,s_rx.cur_is_dir);

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
            hop_xfer_image_begin_msg msg;
            if (!hop_xfer_decode_image_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed IMAGE_BEGIN rejected");
                return;
            }
            uint32_t batch = msg.batch;
            rx_begin_batch(batch);
            if (!s_rx.active) return;

            int w = (int)msg.width;
            int h = (int)msg.height;
            int stride = (int)msg.stride;

            /* 对端声称的尺寸必须自己验：不然 w=h=100000 就能把内存吃光。
             * 这正是"避免内存不断增长"的边界。 */
            if (!hop_xfer_image_valid(w, h, stride)) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                send_abort(s_rx.batch, HOP_ABORT_TOO_LARGE);
                return;
            }

            s_rx.cur_is_image = 1;
            s_rx.cur_id = msg.id;
            s_rx.img_w = w;
            s_rx.img_h = h;
            s_rx.img_stride = stride;
            s_rx.img_expected = (size_t)stride * (size_t)h;
            status_image(batch,HOP_XFER_DIR_RECEIVE,s_rx.img_expected);
            s_rx.img_len = 0;
            break;
        }

        case HOP_MSG_FILE_CHUNK: {
            if (len < 20) return;
            uint64_t off = hop_get_be64(p + 4);
            uint32_t n   = hop_get_be32(p + 12);
            uint32_t crc = hop_get_be32(p + 16);
            if (!hop_xfer_chunk_valid(p, len,
                    s_rx.cur_is_image ? s_rx.img_expected :
                        (s_rx.cur_fail >= 0 ? UINT64_MAX : s_rx.cur_size),
                    s_rx.cur_is_image)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed or out-of-range chunk; closing data channel");
                pthread_mutex_lock(&s_x.slock);
                int active = s_x.sock;
                pthread_mutex_unlock(&s_x.slock);
                if (active >= 0) shutdown(active, SHUT_RDWR);
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
                if (s_rx.cur_is_image) {
                    send_abort(s_rx.batch, HOP_ABORT_CRC);
                    s_rx.img_len = 0;
                } else {
                    s_rx.cur_fail = HOP_FILE_ACK_CRC;
                }
                return;
            }

            if (s_rx.cur_is_image) {
                size_t need = (size_t)off + n;
                if (off != s_rx.img_len || need > s_rx.img_expected) {
                    hop_log(HOP_LOG_ERROR, "xfer: image chunk gap/overlap or length overflow");
                    send_abort(s_rx.batch, HOP_ABORT_TOO_LARGE);
                    s_rx.img_len = 0;
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
                status_chunk(HOP_XFER_DIR_RECEIVE,s_rx.batch,s_rx.cur_id,off,n);
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
            else status_chunk(HOP_XFER_DIR_RECEIVE,s_rx.batch,s_rx.cur_id,off,n);

            /* 文件 CRC 折入本块 CRC：既覆盖数据（块 CRC 已验证），
             * 也覆盖顺序与偏移，且不需要把整份文件重算一遍 */
            s_rx.fcrc = hop_xfer_file_crc_step(s_rx.fcrc, off, crc);
            break;
        }

        case HOP_MSG_FILE_END: {
            hop_xfer_file_end_msg end_msg;
            if (!hop_xfer_decode_file_end(p, len, &end_msg)) return;
            uint32_t id = end_msg.id;
            uint32_t want = end_msg.file_crc;
            uint8_t status = end_msg.status;

            /* 图片整批走，没有逐文件回执：发送端不会等 */
            if (s_rx.cur_is_image) break;

            status_file_phase(HOP_XFER_DIR_RECEIVE,s_rx.batch,(int)id-1,HOP_XFER_FILE_VERIFYING,0);
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


            status_file_phase(HOP_XFER_DIR_RECEIVE,s_rx.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

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

        case HOP_MSG_FILE_ACK: {
            hop_xfer_file_ack_msg ack;
            if (hop_xfer_decode_file_ack(p, len, &ack))
                ack_deliver(ack.batch, ack.id, ack.status);
            break;
        }

        case HOP_MSG_XFER_ABORT: {
            hop_xfer_abort_msg abort_msg;
            if (!hop_xfer_decode_abort(p, len, &abort_msg)) return;
            uint32_t batch = abort_msg.batch;
            uint8_t reason = abort_msg.reason;

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
                status_end(HOP_XFER_DIR_RECEIVE,batch,reason==HOP_ABORT_CANCEL?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Peer aborted receive");
                s_rx.dead_batch = batch;
                rx_clear_state();
            }
            break;
        }

        case HOP_MSG_XFER_DONE: {
            hop_xfer_done_msg done;
            if (!hop_xfer_decode_done(p, len, &done)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed XFER_DONE ignored");
                return;
            }
            if (done.ok < done.total)
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u PARTIAL - %u/%u delivered, %u failed",
                        done.batch, done.ok, done.total, done.total - done.ok);
            rx_finish_batch(done.batch, done.ok, done.total);
            break;
        }

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
        int no_sigpipe = 1;
        if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            close(s);
            s = -1;
            continue;
        }
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
    status_end(HOP_XFER_DIR_RECEIVE,batch,HOP_XFER_CANCELLED,"Receive cancelled");
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

        /* 数据通道空闲时间最长：不开 keepalive，链路中间设备丢掉空闲
         * 连接表项后这条 socket 就变黑洞 —— 对端发来的字节永远到不了，
         * 而本端 read 一直 EAGAIN，连"断开"都感知不到。 */
        net_set_keepalive(s);

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
        s_x.peer_ready = 0;
        pthread_mutex_unlock(&s_x.slock);

        pthread_mutex_lock(&s_x.qlock);
        s_x.abort_batch = 0;          /* 新会话，上一轮的 ABORT 不再算数 */
        pthread_mutex_unlock(&s_x.qlock);

        hop_decoder_reset(dec);
        rx_close_file(); /* keep the batch and ACKed paths across reconnect */

        uint8_t hello[2 + HOP_TOKEN_MAX];
        size_t token_len = strlen(g_mac.cfg.token);
        hello[0] = HOP_XFER_PROTO_VER;
        hello[1] = (uint8_t)token_len;
        if (token_len) memcpy(hello + 2, g_mac.cfg.token, token_len);
        if (!send_small(s, HOP_MSG_XFER_HELLO, hello, (uint32_t)(2 + token_len))) {
            shutdown(s, SHUT_RDWR);
        }
        hop_log(HOP_LOG_INFO, "xfer: data channel connected; waiting for peer authentication");

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
        s_x.peer_ready = 0;
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

