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
#include "../core/xfer_tracking.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
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
#include <stdatomic.h>

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
    unsigned long    epoch;
    int              peer_ready;     /* 数据通道版本与 token 握手通过 */
    pthread_mutex_t  slock;          /* 保护 sock */
    pthread_mutex_t  qlock;          /* 保护作业队列 */
    pthread_cond_t   qcond;
    _Atomic int      stop;
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
static _Thread_local unsigned long s_send_epoch;
static int send_session_current(int fd) {
    pthread_mutex_lock(&s_x.slock);
    int ok = s_x.sock == fd && (!s_send_epoch || s_x.epoch == s_send_epoch);
    pthread_mutex_unlock(&s_x.slock); return ok;
}
static pthread_mutex_t s_status_lock = PTHREAD_MUTEX_INITIALIZER;
static hop_xfer_tracking s_send_status, s_receive_status;
static uint64_t s_status_revision;
static char *s_retry_paths[HOP_XFER_FILE_LIMIT];
static hop_xfer_history_entry s_history[HOP_XFER_HISTORY_LIMIT];
static int s_history_count;
static pthread_mutex_t s_history_io_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t count;
} hop_history_disk_header;
static char s_history_path[PATH_MAX];

static int history_prepare_path(void) {
    const char *home=getenv("HOME");
    if(!home||!*home)return 0;
    char library[PATH_MAX],support[PATH_MAX],directory[PATH_MAX];
    if(snprintf(library,sizeof(library),"%s/Library",home)>=(int)sizeof(library) ||
       snprintf(support,sizeof(support),"%s/Application Support",library)>=(int)sizeof(support) ||
       snprintf(directory,sizeof(directory),"%s/HopDesk",support)>=(int)sizeof(directory) ||
       snprintf(s_history_path,sizeof(s_history_path),"%s/transfer-history.bin",directory)>=(int)sizeof(s_history_path))return 0;
    mkdir(library,0700);mkdir(support,0700);
    if(mkdir(directory,0700)!=0&&errno!=EEXIST)return 0;
    return 1;
}
static void history_load(void) {
    if(!history_prepare_path())return;
    FILE *f=fopen(s_history_path,"rb");if(!f)return;
    hop_history_disk_header h;memset(&h,0,sizeof(h));
    if(fread(&h,sizeof(h),1,f)==1&&memcmp(h.magic,"HOPXHST1",8)==0&&h.version==1&&
       h.record_size==sizeof(hop_xfer_history_entry)&&h.count<=HOP_XFER_HISTORY_LIMIT){
        hop_xfer_history_entry rows[HOP_XFER_HISTORY_LIMIT];memset(rows,0,sizeof(rows));
        size_t n=fread(rows,sizeof(rows[0]),h.count,f);
        if(n==h.count){
            for(uint32_t i=0;i<h.count;i++){
                rows[i].name[sizeof(rows[i].name)-1]=0;
                rows[i].detail[sizeof(rows[i].detail)-1]=0;
                if(rows[i].direction<HOP_XFER_DIR_SEND||rows[i].direction>HOP_XFER_DIR_RECEIVE||
                   rows[i].phase<HOP_XFER_COMPLETED||rows[i].phase>HOP_XFER_CANCELLED){n=0;break;}
            }
            if(n==h.count){
                pthread_mutex_lock(&s_status_lock);
                memcpy(s_history,rows,n*sizeof(rows[0]));s_history_count=(int)n;
                pthread_mutex_unlock(&s_status_lock);
            }
        }
    }
    fclose(f);
}
static void history_save(void) {
    pthread_mutex_lock(&s_history_io_lock);
    if(!s_history_path[0]&&!history_prepare_path()){pthread_mutex_unlock(&s_history_io_lock); return;}
    hop_xfer_history_entry rows[HOP_XFER_HISTORY_LIMIT];int count;
    pthread_mutex_lock(&s_status_lock);
    count=s_history_count;
    if(count>HOP_XFER_HISTORY_LIMIT)count=HOP_XFER_HISTORY_LIMIT;
    memcpy(rows,s_history,(size_t)count*sizeof(rows[0]));
    pthread_mutex_unlock(&s_status_lock);
    char temp[PATH_MAX];
    if(snprintf(temp,sizeof(temp),"%s.tmp",s_history_path)>=(int)sizeof(temp)){pthread_mutex_unlock(&s_history_io_lock); return;}
    FILE *f=fopen(temp,"wb");if(!f){pthread_mutex_unlock(&s_history_io_lock); return;}
    hop_history_disk_header h={{'H','O','P','X','H','S','T','1'},1,(uint32_t)sizeof(rows[0]),(uint32_t)count};
    int ok=fwrite(&h,sizeof(h),1,f)==1&&fwrite(rows,sizeof(rows[0]),(size_t)count,f)==(size_t)count;
    if(ok)ok=fflush(f)==0&&fsync(fileno(f))==0;
    if(fclose(f)!=0)ok=0;
    if(ok){if(rename(temp,s_history_path)!=0)unlink(temp);}
    else unlink(temp);
    pthread_mutex_unlock(&s_history_io_lock);
}

static hop_xfer_tracking *status_for(int direction) {
    return direction == HOP_XFER_DIR_SEND ? &s_send_status : &s_receive_status;
}
static void status_begin(uint32_t batch,int direction,uint32_t files,uint64_t bytes,const char *name,int phase) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t = status_for(direction);
    /* The FIFO sender retains this snapshot while retrying the same job. */
    if (!hop_xfer_tracking_matches(t,batch)) {
        if (direction == HOP_XFER_DIR_SEND)
            for(int i=0;i<HOP_XFER_FILE_LIMIT;i++){free(s_retry_paths[i]);s_retry_paths[i]=NULL;}
        hop_xfer_tracking_begin(t,batch,direction,files,bytes,name,phase);
    } else t->status.phase=phase;
    t->revision=++s_status_revision;
    snprintf(t->status.detail,sizeof(t->status.detail),"%s",direction==HOP_XFER_DIR_SEND?"Sending to peer":"Receiving from peer");
    pthread_mutex_unlock(&s_status_lock);
}
static void status_chunk(int direction,uint32_t batch,uint32_t id,uint64_t off,uint32_t n) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_chunk(status_for(direction),batch,id,off,n);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_file_phase(int direction,uint32_t batch,int index,int phase,uint32_t attempts) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_phase(status_for(direction),batch,index,phase,attempts);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_end(int direction,uint32_t batch,int phase,const char *detail) {
    int changed=0;
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    if(hop_xfer_tracking_end(t,batch,phase,detail)) {
        t->revision=++s_status_revision; changed=1;
        memmove(&s_history[1],&s_history[0],(size_t)(s_history_count<HOP_XFER_HISTORY_LIMIT?s_history_count:HOP_XFER_HISTORY_LIMIT-1)*sizeof(s_history[0]));
        hop_xfer_history_entry *h=&s_history[0];memset(h,0,sizeof(*h));
        h->batch=batch;h->direction=direction;h->phase=phase;
        h->files_done=t->status.files_done;h->files_total=t->status.files_total;
        h->bytes_done=t->status.bytes_done;h->bytes_total=t->status.bytes_total;h->finished_at=(uint64_t)time(NULL);
        snprintf(h->name,sizeof(h->name),"%s",t->status.name);
        snprintf(h->detail,sizeof(h->detail),"%s",t->status.detail);
        if(s_history_count<HOP_XFER_HISTORY_LIMIT)s_history_count++;
    }
    pthread_mutex_unlock(&s_status_lock);
    if(changed)history_save();
}
static void status_set_files(uint32_t batch,const xf_entry *entries,int n) {
    pthread_mutex_lock(&s_status_lock);
    if(hop_xfer_tracking_matches(&s_send_status,batch) && s_send_status.count==0) {
        s_send_status.status.bytes_total=0;
        for(int i=0;i<n && i<HOP_XFER_FILE_LIMIT;i++) {
            hop_xfer_tracking_file(&s_send_status,batch,(uint32_t)i+1,entries[i].rel?entries[i].rel:entries[i].name,
                entries[i].size,(entries[i].flags&HOP_FILE_FLAG_DIR)!=0,HOP_XFER_FILE_QUEUED);
            if(entries[i].full)s_retry_paths[i]=strdup(entries[i].full);
        }
    }
    pthread_mutex_unlock(&s_status_lock);
}
static void status_image(uint32_t batch,int direction,uint64_t size) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    if(hop_xfer_tracking_matches(t,batch)) {
        if(t->count==0)t->status.bytes_total=0;
        hop_xfer_tracking_file(t,batch,1,"Image",size,0,
            direction==HOP_XFER_DIR_SEND?HOP_XFER_FILE_SENDING:HOP_XFER_FILE_RECEIVING);
    }
    pthread_mutex_unlock(&s_status_lock);
}
static void status_add_received_file(uint32_t batch,uint32_t id,const char *path,uint64_t size,int directory) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_file(&s_receive_status,batch,id,path,size,directory,HOP_XFER_FILE_RECEIVING);
    if(hop_xfer_tracking_matches(&s_receive_status,batch) && path)
        snprintf(s_receive_status.status.name,sizeof(s_receive_status.status.name),"%s",path);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_start_job(const xf_job *j) {
    uint64_t total_bytes=0;
    for(int i=0;i<j->n;i++)total_bytes+=j->e[i].size;
    if(j->is_image)total_bytes=(uint64_t)j->stride*j->h;
    status_begin(j->batch,HOP_XFER_DIR_SEND,hop_xfer_job_total(j->is_image,j->n),total_bytes,
                 j->is_image?"Image":j->e[0].name,HOP_XFER_SENDING);
    if(j->is_image)status_image(j->batch,HOP_XFER_DIR_SEND,total_bytes);
    else status_set_files(j->batch,j->e,j->n);
}
int xfer_get_files(int direction,uint32_t batch,hop_xfer_file_status *out,int capacity) {
    if(!out||capacity<=0)return 0;
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    int n=t->status.batch==batch?t->count:0;if(n>capacity)n=capacity;
    memcpy(out,t->files,(size_t)n*sizeof(*out));
    pthread_mutex_unlock(&s_status_lock);
    return n;
}
int xfer_retry_file(uint32_t batch,int index) {
    char *path=NULL;
    pthread_mutex_lock(&s_status_lock);
    if(s_send_status.status.batch==batch && index>=0 && index<s_send_status.count &&
       s_send_status.files[index].phase==HOP_XFER_FILE_FAILED &&
       !s_send_status.files[index].is_directory && s_retry_paths[index])path=strdup(s_retry_paths[index]);
    pthread_mutex_unlock(&s_status_lock);
    if(!path)return 0;
    const char *paths[1]={path};int ok=xfer_submit_paths(paths,1);free(path);return ok;
}
int xfer_get_history(hop_xfer_history_entry *out,int capacity) {
    if(!out||capacity<=0)return 0;
    pthread_mutex_lock(&s_status_lock);
    int n=s_history_count<capacity?s_history_count:capacity;memcpy(out,s_history,(size_t)n*sizeof(*out));
    pthread_mutex_unlock(&s_status_lock);
    return n;
}
void xfer_get_status(hop_xfer_status *out) {
    if(!out)return; uint32_t queued=0;
    pthread_mutex_lock(&s_x.qlock);
    for(xf_job *j=s_x.q_head;j;j=j->next)queued++;
    pthread_mutex_unlock(&s_x.qlock);
    pthread_mutex_lock(&s_status_lock);
    *out=hop_xfer_tracking_visible(&s_send_status,&s_receive_status)->status;
    out->queued_count=queued;
    pthread_mutex_unlock(&s_status_lock);
}



static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            /* 部分帧不可重用；唤醒 conn 线程并让作业走断线重试。 */
            shutdown(fd, SHUT_RDWR);
            return 0;
        }
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_small(int fd, uint8_t type, const uint8_t *p, uint32_t len) {
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);
    pthread_mutex_lock(&s_txlock);
    int ok = send_session_current(fd) && send_all(fd, hdr, HOP_HEADER_SIZE);
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

/* $TMPDIR 在 macOS 上带尾部斜杠（/var/folders/…/T/），直接拼 "/HopDesk"
 * 就得到 "T//HopDesk"。POSIX 下双斜杠无害，但日志和剪贴板里的路径
 * 会变成两个不同字符串表示同一个文件，排查时容易看花眼。去掉尾斜杠。 */
static const char *temp_root(void) {
    static char buf[1024];
    const char *t = getenv("TMPDIR");
    if (!t || !*t) return "/tmp";

    size_t n = strlen(t);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, t, n);
    buf[n] = '\0';
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    return buf;
}

/* Relative path validation is shared in src/core/xfer_validate.c. */

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

static int send_chunk(int fd, uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc) {
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + len);
    hop_put_be32(tx + 5, id);
    hop_put_be64(tx + 9, off);
    hop_put_be32(tx + 17, len);
    hop_put_be32(tx + 21, crc);
    if (data != tx + CHUNK_HDR) memcpy(tx + CHUNK_HDR, data, len);
    pthread_mutex_lock(&s_txlock);
    int ok = send_session_current(fd) && send_all(fd, tx, CHUNK_HDR + len);
    pthread_mutex_unlock(&s_txlock);
    if(ok) status_chunk(HOP_XFER_DIR_SEND,batch,id,off,len);
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
                if (!send_chunk(fd, tx, batch, id, off, tx + CHUNK_HDR, (uint32_t)got, ccrc)) {
                    close(f);
                    return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, off, ccrc);
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
    hop_put_be32(dp + 8,  hop_xfer_job_total(j->is_image,j->n));
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
    if(cancel_requested(j->batch))return JOB_CANCELLED;
    if(peer_aborted(j->batch))return JOB_PEER_ABORT;
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
            if (!send_chunk(fd, tx, j->batch, 1, off, j->pixels + off, n, crc)) return JOB_NET_DOWN;
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
        status_file_phase(HOP_XFER_DIR_SEND,j->batch,0,HOP_XFER_FILE_COMPLETED,1);
        return JOB_DONE;
    }

    while (j->next_i < j->n) {
        if (peer_aborted(j->batch)) {
            hop_log(HOP_LOG_WARN, "xfer: batch %u aborted by peer at entry %d",
                    j->batch, j->next_i);
            return JOB_PEER_ABORT;
        }

        const xf_entry *e = &j->e[j->next_i];
        status_file_phase(HOP_XFER_DIR_SEND,j->batch,j->next_i,HOP_XFER_FILE_SENDING,0);
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
            status_file_phase(HOP_XFER_DIR_SEND,j->batch,j->next_i,HOP_XFER_FILE_COMPLETED,(uint32_t)(tries+1));
        } else {
            status_file_phase(HOP_XFER_DIR_SEND,j->batch,j->next_i,HOP_XFER_FILE_FAILED,(uint32_t)(tries+1));
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
            p=s_x.q_head;
            if(p && !(p->batch==s_x.cancel_batch || (p->give_up_at && n>=p->give_up_at)||p->retry_at<=n))p=NULL;
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
            if (cur->batch==s_x.cancel_batch || (cur->give_up_at && now >= cur->give_up_at)) {
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
            break; /* FIFO: do not overtake a paused batch. */
        }
        if (j) s_x.cur_batch = j->batch;
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                    expired->batch, expired->ok, expired->n);
            status_start_job(expired);
            status_end(HOP_XFER_DIR_SEND,expired->batch,cancel_requested(expired->batch)?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued");
            job_free(expired);
            continue;
        }
        if (!j) continue;
        status_start_job(j);

        pthread_mutex_lock(&s_x.slock);
        fd = s_x.peer_ready ? s_x.sock : -1;
        s_send_epoch = s_x.epoch;
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
                /* 退回队首，保持 FIFO，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                j->next = NULL;
                pthread_mutex_lock(&s_x.qlock);
                j->next=s_x.q_head; s_x.q_head=j;
                if(!s_x.q_tail)s_x.q_tail=j;
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
        if (r == JOB_DONE) status_end(HOP_XFER_DIR_SEND,j->batch,j->ok==(int)hop_xfer_job_total(j->is_image,j->n)?HOP_XFER_COMPLETED:HOP_XFER_PARTIAL,
                                      j->ok==(int)hop_xfer_job_total(j->is_image,j->n)?"Transfer completed":"Some files failed");
        else if (r == JOB_CANCELLED) status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_CANCELLED,"Cancelled");
        else if (r == JOB_PEER_ABORT) status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Peer aborted transfer");
        if(r==JOB_NET_DOWN)status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Reconnect attempts exhausted");
        job_free(j);
    }

    free(tx);
    return NULL;
}

/* Receiver and disk writer share this translation unit and its private state. */
#include "xfer_receive.inc"

/* ---------------- 对外接口 ---------------- */

void xfer_init(const hop_config *cfg) {
    history_load();
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
    if (s_x.sock >= 0) shutdown(s_x.sock, SHUT_RDWR);
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
    c = (s_x.sock >= 0 && s_x.peer_ready);
    pthread_mutex_unlock(&s_x.slock);
    return c;
}

/* 有没有正在进行的传输？菜单项据此决定是否可点。 */
int xfer_busy(void) {
    int b;
    pthread_mutex_lock(&s_x.qlock);
    b = (s_x.cur_batch != 0 || s_x.q_head != NULL);
    pthread_mutex_unlock(&s_x.qlock);
    pthread_mutex_lock(&s_status_lock);
    b=b||s_receive_status.status.active;
    pthread_mutex_unlock(&s_status_lock);
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

void xfer_kick(void) {
    pthread_mutex_lock(&s_x.slock);
    if (s_x.sock >= 0) shutdown(s_x.sock, SHUT_RDWR);
    pthread_mutex_unlock(&s_x.slock);
}
