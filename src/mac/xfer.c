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
#include "xfer_writer_io.h"
#include "../core/xfer_rx_queue.h"
#include "../core/update_state.h"
#include "../core/xfer_tracking.h"
#include "../core/xfer_budget.h"
#include "../core/xfer_task.h"
#include "../core/xfer_task_history.h"
#include "../core/xfer_rate.h"
#include "../core/xfer_ack_window.h"
#include "../core/xfer_buffer_pool.h"
#include "../core/xfer_rx_queue.h"

_Static_assert(HOP_TASK_SESSION_BYTES == HOP_SESSION_BYTES, "task and protocol session sizes differ");

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
    hop_session_id session;
    uint32_t  batch;
    int       is_image;
    hop_xfer_task task;
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
    uint64_t  scan_ns;        /* source enumeration duration, recorded on first visible attempt */
} xf_job;

typedef struct {
    xf_entry *v;
    int n, cap;
    int failed; /* incomplete/unsafe snapshots are never queued */
    int budget_rejected;
    hop_xfer_task task;
} elist;

/* ---------------- 状态 ---------------- */

static struct {
    int              sock;
    hop_session_id   session;
    unsigned long    epoch;
    int              peer_ready;     /* 数据通道版本与 token 握手通过 */
    pthread_mutex_t  slock;          /* 保护 sock */
    pthread_mutex_t  qlock;          /* 保护作业队列 */
    pthread_cond_t   qcond;
    _Atomic int      stop;
    xf_job          *q_head, *q_tail;
    uint32_t         batch_seq;
    uint32_t         chunk;
    hop_xfer_rate_limiter rate; /* sender thread only */
    pthread_t        th_conn, th_send;

    /* ---- 取消 ----
     * cur_batch 由发送线程在开始一批时设置；cancel_batch 由主线程设置。
     * 发送线程每块之后比一次，所以取消的响应延迟上限是一个分块（256 KB）。 */
    uint32_t         cur_batch;     /* 0 = 当前没有在发的批次 */
    hop_xfer_task_id cur_task_id;  /* same queue lock as cur_batch */
    uint32_t         cancel_batch;  /* 0 = 没有取消请求 */
    volatile int     cancel_rx;     /* 接收侧取消请求，由 conn 线程消费 */
    uint32_t         abort_batch;   /* 对端 ABORT 过的批次，0 = 无 */
    uint8_t          abort_reason;
} s_x;

/* ---------------- 小工具 ---------------- */

/* 发送互斥：send_thread 和接收线程（出错时回 ABORT）会同时往同一个
 * socket 写。一次 send_all 内部可能分多次 write，两线程交错就会把
 * 帧字节搅在一起 —— 锁的粒度必须是"整帧"，不是"单次 write"。 */
static pthread_mutex_t s_txlock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned long s_send_epoch;
static int send_session_current(int fd) {
    pthread_mutex_lock(&s_x.slock);
    int ok = s_x.sock == fd && (!s_send_epoch || s_x.epoch == s_send_epoch) && hop_session_matches(&s_x.session);
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
static void task_history_store_open(void) {
    if (!s_history_path[0]) return;
    char path[PATH_MAX];
    char *slash=strrchr(s_history_path,'/');
    if (!slash) return;
    size_t dir=(size_t)(slash-s_history_path+1);
    if (dir+sizeof("task-history.bin")>sizeof(path)) return;
    memcpy(path,s_history_path,dir);
    memcpy(path+dir,"task-history.bin",sizeof("task-history.bin"));
    if (!hop_xfer_task_history_store_open(path,s_history_path))
        hop_log(HOP_LOG_WARN,"xfer: durable task history unavailable; using session history only");
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
static int status_begin(uint32_t batch,int direction,uint32_t files,uint64_t bytes,const char *name,int phase) {
    int created=0;
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t = status_for(direction);
    /* The FIFO sender retains this snapshot while retrying the same job. */
    if (!hop_xfer_tracking_matches(t,batch)) {
        if (direction == HOP_XFER_DIR_SEND)
            for(int i=0;i<HOP_XFER_FILE_LIMIT;i++){free(s_retry_paths[i]);s_retry_paths[i]=NULL;}
        hop_xfer_tracking_begin(t,batch,direction,files,bytes,name,phase);
        created=1;
    } else t->status.phase=phase;
    t->sample_ms=hop_monotonic_ms();
    t->sample_bytes=t->status.bytes_done;
    t->status.speed_bytes_per_sec=0;
    t->status.last_progress_ms=0;
    t->status.eta_seconds=0;
    t->revision=++s_status_revision;
    snprintf(t->status.detail,sizeof(t->status.detail),"%s",direction==HOP_XFER_DIR_SEND?"Sending to peer":"Receiving from peer");
    pthread_mutex_unlock(&s_status_lock);
    return created;
}
static void status_chunk_timed(int direction,uint32_t batch,uint32_t id,uint64_t off,uint32_t n,
        uint64_t read_ns,uint64_t crc_ns,uint64_t send_ns,uint64_t receive_ns) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    hop_xfer_tracking_chunk(t,batch,id,off,n,hop_monotonic_ms());
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_READ,read_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_CRC,crc_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_SEND,send_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_RECEIVE,receive_ns);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_stage_time(int direction,uint32_t batch,int stage,uint64_t elapsed_ns) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_add_time(status_for(direction),batch,stage,elapsed_ns);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_file_phase(int direction,uint32_t batch,int index,int phase,uint32_t attempts) {
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_phase(status_for(direction),batch,index,phase,attempts);
    pthread_mutex_unlock(&s_status_lock);
}
static void status_end(int direction,uint32_t batch,int phase,const char *detail) {
    int changed=0;
    uint64_t timing[HOP_XFER_TIME_COUNT]={0};
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
        memcpy(timing,t->stage_ns,sizeof(timing));
        if(s_history_count<HOP_XFER_HISTORY_LIMIT)s_history_count++;
    }
    pthread_mutex_unlock(&s_status_lock);
    if(changed) {
        hop_log(HOP_LOG_INFO,
                "xfer timing batch=%u dir=%s scan=%llums read=%llums crc=%llums send=%llums receive=%llums write=%llums clipboard_dispatch=%llums",
                batch,direction==HOP_XFER_DIR_SEND?"send":"receive",
                (unsigned long long)(timing[HOP_XFER_TIME_SCAN]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_READ]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_CRC]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_SEND]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_RECEIVE]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_WRITE]/1000000ull),
                (unsigned long long)(timing[HOP_XFER_TIME_CLIPBOARD_DISPATCH]/1000000ull));
        history_save();
    }
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
static void job_bind_session(xf_job *j, const hop_session_id *current) {
    if (!hop_session_id_valid(current) || hop_session_id_equal(&j->session,current)) return;
    j->session=*current; j->next_i=0; j->ok=0;
    pthread_mutex_lock(&s_status_lock);
    memset(&s_send_status,0,sizeof(s_send_status));
    pthread_mutex_unlock(&s_status_lock);
}
static void status_start_job(const xf_job *j) {
    uint64_t total_bytes=0;
    for(int i=0;i<j->n;i++)total_bytes+=j->e[i].size;
    if(j->is_image)total_bytes=(uint64_t)j->stride*j->h;
    int created=status_begin(j->batch,HOP_XFER_DIR_SEND,hop_xfer_job_total(j->is_image,j->n),total_bytes,
                 j->is_image?"Image":j->e[0].name,HOP_XFER_SENDING);
    if(created && j->scan_ns)status_stage_time(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_TIME_SCAN,j->scan_ns);
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
    /* A paste batch carries a request and clipboard-version destination. A new
     * ordinary transfer cannot reproduce it; reject rather than overwrite the
     * peer clipboard or send to a changed target. Retry must be initiated as a
     * fresh, validated paste request on the receiving device. */
    if (!batch || (batch & HOP_XFER_PASTE_BATCH_FLAG)) return 0;
    char *path=NULL;
    pthread_mutex_lock(&s_status_lock);
    if(s_send_status.status.batch==batch && !s_send_status.status.active && index>=0 && index<s_send_status.count &&
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
void xfer_get_status_pair(hop_xfer_status_pair *out) {
    if (!out) return;
    uint32_t queued=0;
    pthread_mutex_lock(&s_x.qlock);
    for (xf_job *j=s_x.q_head; j; j=j->next) queued++;
    pthread_mutex_unlock(&s_x.qlock);
    pthread_mutex_lock(&s_status_lock);
    hop_xfer_tracking_snapshot_pair(&s_send_status,&s_receive_status,queued,out);
    pthread_mutex_unlock(&s_status_lock);
}

void xfer_get_status(hop_xfer_status *out) {
    if (!out) return;
    hop_xfer_status_pair pair;
    xfer_get_status_pair(&pair);
    *out=pair.primary_direction==HOP_XFER_DIR_SEND ? pair.send : pair.receive;
}

void xfer_get_budget_snapshot(hop_xfer_budget_snapshot *out) {
    hop_xfer_budget_get_snapshot(out);
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

static int send_small(int fd, uint8_t type, const uint8_t *p, uint32_t len, uint32_t batch) {
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    uint8_t hdr[HOP_HEADER_SIZE], prefix[HOP_DATA_PREFIX];
    pthread_mutex_lock(&s_txlock);
    int ok = send_session_current(fd);
    if (ok && type != HOP_MSG_XFER_HELLO) {
        pthread_mutex_lock(&s_x.slock);
        hop_data_prefix(prefix, &s_x.session, batch);
        pthread_mutex_unlock(&s_x.slock);
        hop_encode_header(hdr, type, len + HOP_DATA_PREFIX);
        ok = send_all(fd, hdr, HOP_HEADER_SIZE) && send_all(fd, prefix, HOP_DATA_PREFIX);
    } else if (ok) {
        hop_encode_header(hdr, type, len);
        ok = send_all(fd, hdr, HOP_HEADER_SIZE);
    }
    if (ok && len) ok = send_all(fd, p, len);
    pthread_mutex_unlock(&s_txlock);
    hop_watchdog_end(send_guard);
    return ok;
}

/* 每块之后让一次 CPU。传输是后台工作，键鼠才是前台 ——
 * 真正起作用的保障是 send_thread 里的 QOS_CLASS_UTILITY，
 * 这里让一次是给同优先级的其他工作减压。 */
static void yield_cpu(void) { sched_yield(); }

static int mkdir_component(const char *path) {
    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int mkdir_p(const char *path) {
    char tmp[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            int ok = mkdir_component(tmp);
            *p = '/';
            if (ok != 0) return -1;
        }
    }
    return mkdir_component(tmp);
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
    if (!e || !e->name || !e->rel ||
        strlen(e->name) > HOP_NAME_LEN_MAX ||
        strlen(e->rel) > HOP_PATH_LEN_MAX ||
        l->n >= XFER_MAX_ENTRIES) {
        l->failed = 1;
        return 0;
    }
    if (!hop_xfer_task_reserve_entry(&l->task, e->size)) {
        l->budget_rejected = 1;
        l->failed = 1;
        return 0;
    }
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        xf_entry *nv = (xf_entry *)realloc(l->v, (size_t)nc * sizeof(*nv));
        if (!nv) { l->failed = 1; return 0; }
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
    l->failed = 0;
    hop_xfer_task_dispose(&l->task);
}

/* 递归收集。lstat 而不是 stat：符号链接跳过，避免环和越界。 */
static struct dirent *guarded_readdir(DIR *directory) {
    int guard = hop_watchdog_begin("directory next", HOP_IO_DEADLINE_MS);
    struct dirent *entry = readdir(directory); hop_watchdog_end(guard); return entry;
}
static void walk_into(elist *l, const char *full, const char *rel) {
    if (!l || l->failed) return;
    if (hop_xfer_task_cancel_requested(l->task.id)) { l->failed=1; return; }
    struct stat st;
    int stat_guard = hop_watchdog_begin("file attributes", HOP_IO_DEADLINE_MS);
    int stat_ok = lstat(full, &st); hop_watchdog_end(stat_guard);
    if (stat_ok != 0) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: source path disappeared or is inaccessible");
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: symbolic link rejected from file selection");
        return;
    }

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
        l->failed = 1;
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {
            free(e.full); free(e.name); free(e.rel);
            l->failed = 1;
            hop_log(HOP_LOG_WARN, "xfer: non-regular source entry rejected");
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

    int open_guard = hop_watchdog_begin("directory open", HOP_IO_DEADLINE_MS);
    DIR *d = opendir(full); hop_watchdog_end(open_guard);
    if (!d) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: cannot enumerate selected directory");
        return;
    }

    struct dirent *de;
    while ((de = guarded_readdir(d)) != NULL) {
        if (l->failed) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        size_t fl = strlen(full), nl = strlen(de->d_name), rl = strlen(rel);
        char *child = (char *)malloc(fl + nl + 2);
        char *crel  = (char *)malloc(rl + nl + 2);
        if (!child || !crel) {
            free(child); free(crel);
            l->failed = 1;
            break;
        }

        snprintf(child, fl + nl + 2, "%s/%s", full, de->d_name);
        snprintf(crel,  rl + nl + 2, "%s/%s", rel, de->d_name);

        walk_into(l, child, crel);
        free(child);
        free(crel);
        if (l->failed) break;
    }
    closedir(d);
}

/* ---------------- 发送 ---------------- */

/* 分块：一次 write 发完头 + 分块头 + 数据。
 * 数据直接读进发送缓冲的 CHUNK_HDR 处，省掉一次内存拷贝。
 * 每块带 CRC32：TCP 的 16 位校验和配合网卡 offload 有可能漏检，
 * GB 级传输漏检一次就是静默损坏。 */
#define CHUNK_HDR (25u + HOP_DATA_PREFIX)   /* 5 帧头 + 4 id + 8 off + 4 len + 4 crc */

#define XFER_RATE_MIN_MIB       4ull
#define XFER_RATE_INITIAL_MIB   64ull
#define XFER_RATE_MIB_BYTES     (1024ull * 1024ull)

static void rate_init_from_config(hop_xfer_rate_limiter *rate, int configured_max_mib,
                                  uint64_t burst_bytes, uint64_t now_ns) {
    uint64_t max_mib = configured_max_mib < 16 ? 16u :
                       configured_max_mib > 4096 ? 4096u :
                       (uint64_t)configured_max_mib;
    uint64_t max_bps = max_mib * XFER_RATE_MIB_BYTES;
    uint64_t min_bps = XFER_RATE_MIN_MIB * XFER_RATE_MIB_BYTES;
    uint64_t initial_bps = XFER_RATE_INITIAL_MIB * XFER_RATE_MIB_BYTES;
    if (min_bps > max_bps) min_bps = max_bps;
    if (initial_bps > max_bps) initial_bps = max_bps;
    hop_xfer_rate_init(rate, initial_bps, min_bps, max_bps, burst_bytes, now_ns);
}

static uint64_t rate_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int rate_wait_for_chunk(uint32_t bytes) {
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(&s_x.rate, bytes, rate_now_ns());
        if (delay == UINT64_MAX) {
            hop_log(HOP_LOG_ERROR, "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return 1;
        if (s_x.stop) return 0;
        if (delay > 10000000ull) delay = 10000000ull; /* check shutdown at least every 10 ms */
        struct timespec ts = {(time_t)(delay / 1000000000ull),(long)(delay % 1000000000ull)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
}

static uint32_t rate_queue_pressure_permille(void) {
    hop_xfer_budget_snapshot snap;
    hop_xfer_budget_get_snapshot(&snap);
    uint64_t bytes = snap.rx_queue_bytes * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_BYTES;
    uint64_t slots = (uint64_t)snap.rx_queue_slots * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_SLOTS;
    uint64_t pressure = bytes > slots ? bytes : slots;
    return (uint32_t)(pressure > 1000ull ? 1000ull : pressure);
}

static void rate_feedback(uint64_t ack_rtt_us) {
    uint64_t before = s_x.rate.current_rate_bytes_per_sec;
    uint32_t pressure = rate_queue_pressure_permille();
    hop_xfer_rate_feedback(&s_x.rate, ack_rtt_us, pressure);
    if (s_x.rate.current_rate_bytes_per_sec < before) {
        hop_log(HOP_LOG_INFO, "xfer: adaptive send rate %.1f MB/s (ack %.1f ms, local rx queue %u/1000)",
                (double)s_x.rate.current_rate_bytes_per_sec / 1048576.0,
                (double)ack_rtt_us / 1000.0, pressure);
    }
}

static int send_chunk(int fd, uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc,
                      uint64_t read_ns,uint64_t crc_ns) {
    if (!rate_wait_for_chunk(len)) {
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_CRC,crc_ns);
        return 0;
    }
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + HOP_DATA_PREFIX + len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 5, id);
    hop_put_be64(tx + HOP_DATA_PREFIX + 9, off);
    hop_put_be32(tx + HOP_DATA_PREFIX + 17, len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 21, crc);
    pthread_mutex_lock(&s_txlock);
    pthread_mutex_lock(&s_x.slock);
    hop_data_prefix(tx + HOP_HEADER_SIZE, &s_x.session, batch);
    pthread_mutex_unlock(&s_x.slock);
    uint64_t send_started = rate_now_ns();
    int ok = send_session_current(fd) &&
             (data == tx + CHUNK_HDR ?
              send_all(fd, tx, CHUNK_HDR + len) :
              (send_all(fd, tx, CHUNK_HDR) && send_all(fd, data, len)));
    uint64_t send_us = (rate_now_ns() - send_started) / 1000ull;
    hop_xfer_rate_note_send_time(&s_x.rate, len, send_us);
    int feedback_due = (s_x.rate.send_samples & 7u) == 0;
    pthread_mutex_unlock(&s_txlock);
    if (feedback_due) rate_feedback(0);
    uint64_t send_ns=send_us*1000ull;
    if(ok) status_chunk_timed(HOP_XFER_DIR_SEND,batch,id,off,len,read_ns,crc_ns,send_ns,0);
    else {
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_CRC,crc_ns);
        status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_SEND,send_ns);
    }
    hop_watchdog_end(send_guard);
    return ok;
}

/* ---------------- 有界单文件 ACK 窗口 ----------------
 *
 * 最多允许 4 个文件等待接收端落盘 ACK。数据帧仍然串行写入同一 TCP
 * stream；ACK 由接收端 writer 异步返回，按 batch/id 独立归位。 */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    hop_xfer_ack_window window;
} s_ack;

/* 返回值：>=0 是接收端 ACK 的 status；负数是本地/链路层面的失败 */
#define XFER_ACK_TIMEOUT  (-1)
#define XFER_SEND_DEAD    (-2)
#define XFER_LOCAL_IO     (-3)
#define XFER_CANCELLED    (-4)
#define XFER_PEER_ABORT   (-5)
#define XFER_ACK_PENDING  (-6)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static int ack_register(uint32_t batch, uint32_t id) {
    pthread_mutex_lock(&s_ack.lock);
    int ok = hop_xfer_ack_window_register(&s_ack.window, batch, id);
    pthread_mutex_unlock(&s_ack.lock);
    return ok;
}

static int ack_mark_sent(uint32_t batch, uint32_t id) {
    pthread_mutex_lock(&s_ack.lock);
    int ok = hop_xfer_ack_window_mark_sent(&s_ack.window, batch, id, rate_now_ns());
    pthread_mutex_unlock(&s_ack.lock);
    return ok;
}

static int ack_set_deadline(uint32_t batch, uint32_t id, uint64_t deadline_ns) {
    pthread_mutex_lock(&s_ack.lock);
    int ok = hop_xfer_ack_window_set_deadline(&s_ack.window,batch,id,deadline_ns);
    pthread_mutex_unlock(&s_ack.lock);
    return ok;
}

static void ack_unregister(uint32_t batch, uint32_t id) {
    pthread_mutex_lock(&s_ack.lock);
    hop_xfer_ack_window_unregister(&s_ack.window, batch, id);
    pthread_mutex_unlock(&s_ack.lock);
}

static void ack_unregister_batch(uint32_t batch) {
    pthread_mutex_lock(&s_ack.lock);
    hop_xfer_ack_window_unregister_batch(&s_ack.window, batch);
    pthread_mutex_unlock(&s_ack.lock);
}

/* 接收线程只填写与当前 batch/id 匹配的槽；过期 ACK 不得覆盖其他文件。 */
static void ack_deliver(uint32_t batch, uint32_t id, int status) {
    pthread_mutex_lock(&s_ack.lock);
    int delivered = hop_xfer_ack_window_deliver(&s_ack.window, batch, id, status, rate_now_ns());
    if (delivered) pthread_cond_broadcast(&s_ack.cond);
    pthread_mutex_unlock(&s_ack.lock);
}

/* 用户取消了这个批次？每块之后查一次，锁开销可以忽略。 */
static int cancel_requested(uint32_t batch) {
    pthread_mutex_lock(&s_x.qlock);
    int c = (s_x.cancel_batch == batch);
    hop_xfer_task_id id = s_x.cur_batch == batch ? s_x.cur_task_id : 0;
    pthread_mutex_unlock(&s_x.qlock);
    return c || (id && hop_xfer_task_cancel_requested(id));
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
static int ack_wait(uint32_t batch, uint32_t id, uint64_t deadline_ns, uint64_t *rtt_us) {
    if (rtt_us) *rtt_us = 0;
    for (;;) {
        pthread_mutex_lock(&s_ack.lock);
        int status = 0, deadline_expired = 0;
        uint64_t rtt_ns = 0;
        int hit = hop_xfer_ack_window_take_ex(&s_ack.window,batch,id,&status,&rtt_ns,
                                              &deadline_expired);
        if (!hit) {
            uint64_t n = rate_now_ns();
            if (n < deadline_ns) {
                uint64_t slice_ns = deadline_ns - n;
                if (slice_ns > 100000000ull) slice_ns = 100000000ull;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += (long)slice_ns;
                while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&s_ack.cond, &s_ack.lock, &ts);
            }
        }
        pthread_mutex_unlock(&s_ack.lock);
        if (hit) {
            /* ACK arrival may race with FILE_END send completion. The
             * window stores the exact receive timestamp and the deadline is
             * anchored immediately after FILE_END is fully written. */
            if (deadline_expired) return XFER_ACK_TIMEOUT;
            if (rtt_us) *rtt_us = rtt_ns / 1000ull;
            return status;
        }

        if (peer_aborted(batch)) { ack_unregister(batch,id); return XFER_PEER_ABORT; }
        if (cancel_requested(batch)) { ack_unregister(batch,id); return XFER_CANCELLED; }
        if (s_x.stop) { ack_unregister(batch,id); return XFER_SEND_DEAD; }
        pthread_mutex_lock(&s_x.slock);
        int peer_ready = s_x.peer_ready;
        pthread_mutex_unlock(&s_x.slock);
        if (!peer_ready) { ack_unregister(batch,id); return XFER_SEND_DEAD; }
        if (rate_now_ns() >= deadline_ns) { ack_unregister(batch,id); return XFER_ACK_TIMEOUT; }
    }
}

/* 发送一个条目。成功写出 FILE_END 后返回 XFER_ACK_PENDING；
 * run_job 在最多 4 个已发送文件组成的窗口中统一收取 ACK。 */
static int send_file_entry(int fd, uint8_t *tx, uint32_t batch,
                           uint32_t id, const xf_entry *e, uint64_t *ack_deadline_ns) {
    if (!ack_deadline_ns || !tx) return XFER_LOCAL_IO;
    *ack_deadline_ns = 0;
    uint32_t nlen = (uint32_t)strlen(e->name);
    uint32_t plen = (uint32_t)strlen(e->rel);
    uint32_t blen = 4 + 4 + 8 + 8 + 1 + 2 + nlen + 2 + plen;

    /* FILE_BEGIN is small and send_small is synchronous. Reuse the sender's
     * fixed TX buffer instead of malloc/free once per file (notably 10k-file
     * batches); send_chunk overwrites this region only after FILE_BEGIN ends. */
    if (blen > s_x.chunk) return XFER_LOCAL_IO;
    if (!ack_register(batch,id)) return XFER_LOCAL_IO;
    uint8_t *bp = tx + CHUNK_HDR;
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

    int ok = send_small(fd, HOP_MSG_FILE_BEGIN, bp, blen, batch);
    /* TX buffer remains owned by the sender thread. */
    if (!ok) { ack_unregister(batch,id); return XFER_SEND_DEAD; }

    uint32_t fcrc = 0;
    uint8_t end_status = HOP_FILE_OK;
    int local_fail = 0;
    if (!(e->flags & HOP_FILE_FLAG_DIR)) {
        int open_guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
        int f = open(e->full, O_RDONLY);
        hop_watchdog_end(open_guard);
        if (f < 0) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (%s)", e->rel, strerror(errno));
            end_status = HOP_FILE_IO_ERROR;
            local_fail = 1;
        } else {
            uint64_t off = 0;
            while (off < e->size) {
                uint64_t remaining = e->size - off;
                size_t request = (size_t)(remaining < s_x.chunk ? remaining : s_x.chunk);
                ssize_t got;
                uint64_t read_started_ns=rate_now_ns();
                do {
                    int read_guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
                    got = read(f, tx + CHUNK_HDR, request);
                    hop_watchdog_end(read_guard);
                } while (got < 0 && errno == EINTR);
                uint64_t read_elapsed_ns=rate_now_ns()-read_started_ns;
                if (got < 0) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: read failed, %s (%s)", e->rel, strerror(errno));
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                if (got == 0) {
                    status_stage_time(HOP_XFER_DIR_SEND,batch,HOP_XFER_TIME_READ,read_elapsed_ns);
                    hop_log(HOP_LOG_WARN, "xfer: unexpected EOF, %s (%llu/%llu bytes)",
                            e->rel, (unsigned long long)off, (unsigned long long)e->size);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1; break;
                }
                uint64_t crc_started_ns=rate_now_ns();
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, (size_t)got);
                uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
                if (!send_chunk(fd,tx,batch,id,off,tx+CHUNK_HDR,(uint32_t)got,ccrc,
                                read_elapsed_ns,crc_elapsed_ns)) {
                    close(f); ack_unregister(batch,id); return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, off, ccrc);
                off += (uint64_t)got;
                yield_cpu();
                if (cancel_requested(batch)) {
                    close(f); ack_unregister(batch,id); return XFER_CANCELLED;
                }
            }
            if (!local_fail && off == e->size) {
                uint8_t extra = 0;
                ssize_t probe;
                do {
                    int read_guard = hop_watchdog_begin("file size probe", HOP_IO_DEADLINE_MS);
                    probe = read(f, &extra, 1);
                    hop_watchdog_end(read_guard);
                } while (probe < 0 && errno == EINTR);
                if (probe != 0) {
                    hop_log(HOP_LOG_WARN, "xfer: source changed while queued, %s", e->rel);
                    end_status = HOP_FILE_IO_ERROR; local_fail = 1;
                }
            }
            close(f);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    /* Mark before FILE_END so an immediate peer ACK cannot race registration. */
    if (!ack_mark_sent(batch,id)) { ack_unregister(batch,id); return XFER_LOCAL_IO; }
    if (!send_small(fd, HOP_MSG_FILE_END, endp, 9, batch)) {
        ack_unregister(batch,id); return XFER_SEND_DEAD;
    }
    (void)local_fail;
    /* The ACK slot was armed before FILE_END to avoid a fast-ACK race. Set its
     * independent deadline now that the complete frame has been written. */
    uint64_t deadline_ns = rate_now_ns() +
        (uint64_t)HOP_XFER_ACK_TIMEOUT_MS * 1000000ull;
    *ack_deadline_ns = deadline_ns;
    if (!ack_set_deadline(batch,id,deadline_ns)) {
        ack_unregister(batch,id); return XFER_LOCAL_IO;
    }
    return XFER_ACK_PENDING;
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */
#define JOB_COMMIT_UNKNOWN 4 /* receiver outcome is unknown */
#define JOB_COMMIT_REJECTED 5 /* receiver explicitly rejected completion */

/* 收尾：告诉接收端本批实际送达了多少。
 * ok < total 时接收端会报"部分成功" —— 这是用户唯一能拿到准确结论的地方，
 * 所以 count 必须是真实值，不能像以前那样写死 0。 */
static int send_done(int fd, const xf_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp,      j->batch);
    hop_put_be32(dp + 4,  (uint32_t)j->ok);
    hop_put_be32(dp + 8,  hop_xfer_job_total(j->is_image,j->n));
    hop_put_be64(dp + 12, 0);
    return send_small(fd, HOP_MSG_XFER_DONE, dp, 20, j->batch);
}

/* Re-send only DONE, never file contents, while the same control session
 * survives. Receiver caches its last receipt and never reapplies effects. */
static int send_done_result(int fd, const xf_job *job) {
    uint64_t until=now_ms()+HOP_XFER_RECONNECT_BUDGET_MS;
    while (now_ms()<until) {
        if (!hop_session_matches(&job->session)) return JOB_COMMIT_UNKNOWN;
        if (s_x.stop) return JOB_COMMIT_UNKNOWN;
        pthread_mutex_lock(&s_x.slock);
        fd=s_x.peer_ready?s_x.sock:-1;
        hop_session_id session=s_x.session;
        s_send_epoch=s_x.epoch;
        pthread_mutex_unlock(&s_x.slock);
        if (fd<0) { usleep(100000); continue; }
        if (!hop_session_id_equal(&session,&job->session)) return JOB_COMMIT_UNKNOWN;
        if (!ack_register(job->batch,HOP_DONE_ACK_SLOT)) return JOB_COMMIT_UNKNOWN;
        if (!ack_mark_sent(job->batch,HOP_DONE_ACK_SLOT)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN;
        }
        if (!send_done(fd,job)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); usleep(100000); continue;
        }
        uint64_t now=now_ms();
        if (now>=until) { ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN; }
        uint64_t wait=until-now;
        if (wait>HOP_XFER_ACK_TIMEOUT_MS) wait=HOP_XFER_ACK_TIMEOUT_MS;
        uint64_t deadline=rate_now_ns()+wait*1000000ull;
        if (!ack_set_deadline(job->batch,HOP_DONE_ACK_SLOT,deadline)) {
            ack_unregister(job->batch,HOP_DONE_ACK_SLOT); return JOB_COMMIT_UNKNOWN;
        }
        int result=ack_wait(job->batch,HOP_DONE_ACK_SLOT,deadline,NULL);
        if (result==HOP_DONE_ACK_FAILED) return JOB_COMMIT_REJECTED;
        int expected=job->ok==(int)hop_xfer_job_total(job->is_image,job->n)
            ? HOP_DONE_ACK_COMPLETE : HOP_DONE_ACK_PARTIAL;
        if (result==expected) return JOB_DONE;
        if (result==XFER_PEER_ABORT) return JOB_PEER_ABORT;
        if (result!=XFER_ACK_TIMEOUT && result!=XFER_SEND_DEAD) return JOB_COMMIT_UNKNOWN;
        usleep(100000);
    }
    return JOB_COMMIT_UNKNOWN;
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(int fd, uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = HOP_ABORT_TO_RECEIVER;
    send_small(fd, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 取当前连接再发 ABORT。sock 由 slock 保护，不能裸读。
 * 锁序固定 slock -> txlock，不会和发送线程反序。 */
static void send_abort(uint32_t batch, uint8_t reason) {
    uint8_t ab[6];
    hop_put_be32(ab, batch);
    ab[4] = reason; ab[5] = HOP_ABORT_TO_SENDER;

    pthread_mutex_lock(&s_x.slock);
    int fd = s_x.sock;
    pthread_mutex_unlock(&s_x.slock);
    if (fd >= 0) send_small(fd, HOP_MSG_XFER_ABORT, ab, sizeof(ab), batch);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
static void send_ack(int fd, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(fd, HOP_MSG_FILE_ACK, b, 9, batch);
}

static int run_job(int fd, uint8_t *tx, xf_job *j) {

    ack_unregister_batch(j->batch);
    if(cancel_requested(j->batch))return JOB_CANCELLED;
    if(peer_aborted(j->batch))return JOB_PEER_ABORT;
    if (j->is_image) {
        uint8_t bp[20];
        hop_put_be32(bp, j->batch);
        hop_put_be32(bp + 4, 1);
        hop_put_be32(bp + 8, (uint32_t)j->w);
        hop_put_be32(bp + 12, (uint32_t)j->h);
        hop_put_be32(bp + 16, (uint32_t)j->stride);
        if (!send_small(fd, HOP_MSG_IMAGE_BEGIN, bp, 20, j->batch)) return JOB_NET_DOWN;

        uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
        uint64_t off = 0;
        while (off < total) {
            uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
            uint64_t crc_started_ns=rate_now_ns();
            uint32_t crc = hop_crc32(0, j->pixels + off, n);
            uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
            if (!send_chunk(fd,tx,j->batch,1,off,j->pixels+off,n,crc,0,crc_elapsed_ns))
                return JOB_NET_DOWN;
            off += n;
            yield_cpu();
            if (cancel_requested(j->batch)) return JOB_CANCELLED;
            if (peer_aborted(j->batch)) return JOB_PEER_ABORT;
            if (cancel_requested(j->batch)) return JOB_CANCELLED;
        }

        uint8_t endp[9];
        hop_put_be32(endp, 1);
        endp[4] = HOP_FILE_OK;
        hop_put_be32(endp + 5, 0);      /* 图片不做整文件 CRC，块 CRC 已足够 */
        if (!send_small(fd, HOP_MSG_FILE_END, endp, 9, j->batch)) return JOB_NET_DOWN;
        rate_feedback(0);

        j->ok = 1;
        j->next_i = j->n;
        status_file_phase(HOP_XFER_DIR_SEND,j->batch,0,HOP_XFER_FILE_COMPLETED,1);
        return JOB_DONE;
    }

    typedef struct {
        const xf_entry *entry;
        uint32_t id;
        int index;
        int attempts;
        int pending;
        int acknowledged;
        uint64_t deadline_ns;
    } window_item;

    while (j->next_i < j->n) {
        window_item window[HOP_XFER_ACK_WINDOW_CAPACITY];
        uint32_t window_count = 0;
        int send_index = j->next_i;
        int fill_result = JOB_DONE;

        /* Fill a bounded window using a local cursor. j->next_i is the
         * durable resume point and must advance only after each entry has a
         * final ACK/result; a disconnect during fill must not skip sent files. */
        while (window_count < HOP_XFER_ACK_WINDOW_CAPACITY && send_index < j->n) {
            if (cancel_requested(j->batch)) {
                ack_unregister_batch(j->batch);
                return JOB_CANCELLED;
            }
            if (peer_aborted(j->batch)) {
                ack_unregister_batch(j->batch);
                return JOB_PEER_ABORT;
            }

            int index = send_index;
            const xf_entry *e = &j->e[index];
            uint32_t id = (uint32_t)(index + 1);
            window_item *item = &window[window_count];
            memset(item,0,sizeof(*item));
            item->entry = e;
            item->id = id;
            item->index = index;
            status_file_phase(HOP_XFER_DIR_SEND,j->batch,index,HOP_XFER_FILE_SENDING,0);

            int attempts = 0;
            int st = XFER_LOCAL_IO;
            uint64_t deadline_ns = 0;
            for (;;) {
                ++attempts;
                st = send_file_entry(fd,tx,j->batch,id,e,&deadline_ns);
                if (st == XFER_ACK_PENDING) break;
                if (st == XFER_SEND_DEAD || st == XFER_PEER_ABORT ||
                    st == XFER_CANCELLED || st == HOP_FILE_ACK_BADPATH) break;
                if (attempts >= HOP_XFER_FILE_RETRY_MAX) break;
                unsigned d = hop_xfer_file_retry_delay_ms((unsigned)attempts);
                hop_log(HOP_LOG_WARN, "xfer: retry %d/%d for %s in %u ms (send status %d)",
                        attempts,HOP_XFER_FILE_RETRY_MAX,e->rel,d,st);
                usleep((useconds_t)d * 1000);
            }

            if (st == XFER_SEND_DEAD) {
                /* Do not discard earlier ACKs already in this partially-filled
                 * window. Drain the contiguous prefix before reconnecting. */
                fill_result = JOB_NET_DOWN;
                break;
            }
            if (st == XFER_PEER_ABORT) {
                ack_unregister_batch(j->batch);
                return JOB_PEER_ABORT;
            }
            if (st == XFER_CANCELLED) {
                ack_unregister_batch(j->batch);
                return JOB_CANCELLED;
            }

            if (st != XFER_ACK_PENDING) {
                /* No remote FILE_ACK exists for this entry. Stop filling here;
                 * drain only earlier in-flight entries, then reconnect from
                 * this exact checkpoint instead of treating local failure as
                 * a completed result. */
                status_file_phase(HOP_XFER_DIR_SEND,j->batch,index,HOP_XFER_FILE_FAILED,(uint32_t)attempts);
                hop_log(HOP_LOG_ERROR, "xfer: no ACK for %s after %d attempt(s) (status %d); keep checkpoint",
                        e->rel,attempts,st);
                fill_result = JOB_NET_DOWN;
                break;
            }
            item->attempts = attempts;
            item->pending = 1;
            item->deadline_ns = deadline_ns;
            ++window_count;
            ++send_index;
        }

        /* Drain in file order. If the link dies, j->next_i still points at
         * the first unresolved entry, while earlier confirmed entries remain
         * committed and are not retransmitted after reconnect. */
        for (uint32_t wi = 0; wi < window_count; ++wi) {
            window_item *item = &window[wi];
            if (item->pending) {
                uint64_t rtt_us = 0;
                int st = ack_wait(j->batch,item->id,item->deadline_ns,&rtt_us);
                if (st >= 0) item->acknowledged = 1;
                if (rtt_us) rate_feedback(rtt_us);
                int attempts = item->attempts;

                while (st != HOP_FILE_ACK_OK && st != HOP_FILE_ACK_BADPATH) {
                    if (st == XFER_ACK_TIMEOUT) {
                        /* A timeout is not a peer result. Reset this data
                         * connection before retrying the same batch/id so a
                         * delayed ACK from the old attempt cannot satisfy the
                         * new one. The contiguous checkpoint stays unchanged. */
                        hop_log(HOP_LOG_WARN,
                                "xfer: ACK timeout for %s; reconnect before retrying entry %d",
                                item->entry->rel,item->index);
                        ack_unregister_batch(j->batch);
                        xfer_kick();
                        return JOB_NET_DOWN;
                    }
                    if (st == XFER_SEND_DEAD) {
                        ack_unregister_batch(j->batch);
                        return JOB_NET_DOWN;
                    }
                    if (st == XFER_PEER_ABORT) {
                        ack_unregister_batch(j->batch);
                        return JOB_PEER_ABORT;
                    }
                    if (st == XFER_CANCELLED) {
                        ack_unregister_batch(j->batch);
                        return JOB_CANCELLED;
                    }
                    if (attempts >= HOP_XFER_FILE_RETRY_MAX) break;

                    unsigned d = hop_xfer_file_retry_delay_ms((unsigned)attempts);
                    hop_log(HOP_LOG_WARN, "xfer: retry %d/%d for %s in %u ms (ACK status %d)",
                            attempts,HOP_XFER_FILE_RETRY_MAX,item->entry->rel,d,st);
                    usleep((useconds_t)d * 1000);

                    ++attempts;
                    uint64_t retry_deadline_ns = 0;
                    st = send_file_entry(fd,tx,j->batch,item->id,item->entry,&retry_deadline_ns);
                    if (st == XFER_ACK_PENDING) {
                        rtt_us = 0;
                        st = ack_wait(j->batch,item->id,retry_deadline_ns,&rtt_us);
                        if (st >= 0) item->acknowledged = 1;
                        if (rtt_us) rate_feedback(rtt_us);
                    }
                }

                if (st == HOP_FILE_ACK_OK) {
                    j->ok++;
                    status_file_phase(HOP_XFER_DIR_SEND,j->batch,item->index,
                                      HOP_XFER_FILE_COMPLETED,(uint32_t)attempts);
                } else {
                    status_file_phase(HOP_XFER_DIR_SEND,j->batch,item->index,
                                      HOP_XFER_FILE_FAILED,(uint32_t)attempts);
                    hop_log(HOP_LOG_ERROR, "xfer: giving up on %s after %d attempt(s) (status %d)",
                            item->entry->rel,attempts,st);
                }
            }
            /* This is the only place that advances the reconnect checkpoint. */
            if (!item->acknowledged) {
                hop_log(HOP_LOG_ERROR, "xfer: entry %d has no remote ACK; checkpoint remains %d",
                        item->index,j->next_i);
                ack_unregister_batch(j->batch);
                return JOB_NET_DOWN;
            }
            if (!hop_xfer_ack_window_advance_checkpoint(&j->next_i,item->index,1)) {
                hop_log(HOP_LOG_ERROR, "xfer: non-contiguous checkpoint batch=%u current=%d resolved=%d",
                        j->batch,j->next_i,item->index);
                ack_unregister_batch(j->batch);
                return JOB_NET_DOWN;
            }
        }
        if (fill_result != JOB_DONE) {
            ack_unregister_batch(j->batch);
            return fill_result;
        }
    }
    ack_unregister_batch(j->batch);
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

/* Caller holds the queue lock. Removing a cancelled later entry must not
 * change FIFO order or lose the tail of the remaining queue. */
static xf_job *queue_take_cancelled(void) {
    xf_job *prev=NULL, *cur=s_x.q_head;
    while (cur && !hop_xfer_task_cancel_requested(cur->task.id)) { prev=cur; cur=cur->next; }
    if (cur) { queue_unlink(prev,cur); cur->next=NULL; }
    return cur;
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
    hop_xfer_task_dispose(&j->task);
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

        /* Do not consume job retry attempts while the data channel is down.
         * Snapshot outside qlock to keep the slock/qlock order non-nested. */
        pthread_mutex_lock(&s_x.slock);
        int peer_ready_snapshot = s_x.peer_ready;
        pthread_mutex_unlock(&s_x.slock);

        pthread_mutex_lock(&s_x.qlock);
        int waited_for_queue = 0;
        /* Wait for an expired/cancelled job even offline, but only dequeue a
         * retry when the data channel was ready at the start of this pass.
         * A 200 ms poll refreshes the snapshot after reconnect. */
        for (;;) {
            if (s_x.stop) break;
            uint64_t n = now_ms();
            xf_job *p = s_x.q_head;
            if (p && (p->batch == s_x.cancel_batch ||
                      (p->give_up_at && n >= p->give_up_at) ||
                      (peer_ready_snapshot && p->retry_at <= n)))
                break;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s_x.qcond, &s_x.qlock, &ts);
            waited_for_queue = 1;
            break; /* refresh connection state outside qlock after every poll */
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
        expired=queue_take_cancelled();
        xf_job *prev=NULL, *cur=expired ? NULL : s_x.q_head;
        while (cur) {
            /* 重连预算用完：连接在不在都放弃 */
            if (cur->batch==s_x.cancel_batch || (cur->give_up_at && now >= cur->give_up_at)) {
                expired = cur;
                queue_unlink(prev, cur);
                expired->next = NULL;
                break;
            }
            if (!waited_for_queue && peer_ready_snapshot && cur->retry_at <= now) {
                j = cur;
                queue_unlink(prev, cur);
                break;
            }
            break; /* FIFO: do not overtake a paused batch. */
        }
        if (j) { s_x.cur_batch = j->batch; s_x.cur_task_id = j->task.id; s_x.abort_batch=0; s_x.abort_reason=0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (expired) {
            hop_log(HOP_LOG_ERROR,
                    "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                    expired->batch, expired->ok, expired->n);
            int was_cancelled=cancel_requested(expired->batch) || hop_xfer_task_cancel_requested(expired->task.id);
            pthread_mutex_lock(&s_x.slock);
            int abort_fd=s_x.peer_ready && hop_session_id_equal(&expired->session,&s_x.session) ? s_x.sock : -1;
            s_send_epoch=s_x.epoch;
            pthread_mutex_unlock(&s_x.slock);
            if (was_cancelled && abort_fd>=0) send_abort_msg(abort_fd,expired->batch,HOP_ABORT_CANCEL);
            pthread_mutex_lock(&s_status_lock);
            int show_status=!s_send_status.status.active || s_send_status.status.batch==expired->batch;
            pthread_mutex_unlock(&s_status_lock);
            if (show_status) status_start_job(expired);
            hop_xfer_task_finish(&expired->task,was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
            if (show_status) status_end(HOP_XFER_DIR_SEND,expired->batch,was_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued");
            job_free(expired);
            continue;
        }
        if (!j) continue;
        pthread_mutex_lock(&s_x.slock);
        fd = s_x.peer_ready ? s_x.sock : -1;
        s_send_epoch = s_x.epoch;
        hop_session_id transport_session = s_x.session;
        pthread_mutex_unlock(&s_x.slock);
        job_bind_session(j, &transport_session);
        if (hop_session_id_valid(&j->session)) (void)hop_xfer_task_bind_session(&j->task,j->session.bytes);
        if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_ACTIVE)){
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);job_free(j);continue;
        }
        status_start_job(j);

        int r = (fd >= 0) ? run_job(fd, tx, j) : JOB_NET_DOWN;

        pthread_mutex_lock(&s_x.qlock);
        int peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
        int completion_path=(r==JOB_DONE);
        if (!completion_path) { s_x.cur_batch = 0; s_x.cur_task_id = 0; }
        pthread_mutex_unlock(&s_x.qlock);

        if (r != JOB_DONE && hop_xfer_task_cancel_requested(j->task.id)) r=JOB_CANCELLED;
        if (r == JOB_DONE && !hop_xfer_task_begin_completion(&j->task)) r=JOB_CANCELLED;
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
                if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_QUEUED)){
                    hop_log(HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                            (unsigned long long)j->task.id);
                    status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Invalid task retry state");
                    hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                    job_free(j);continue;
                }
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
            r=send_done_result(fd, j);
        }
        if (completion_path) {
            pthread_mutex_lock(&s_x.qlock);
            peer_cancelled=(s_x.abort_batch==j->batch && s_x.abort_reason==HOP_ABORT_CANCEL);
            s_x.cur_batch=0; s_x.cur_task_id=0;
            pthread_mutex_unlock(&s_x.qlock);
        }
        if (r == JOB_DONE) {
            int complete=j->ok==(int)hop_xfer_job_total(j->is_image,j->n);
            hop_xfer_task_finish(&j->task,complete?HOP_XFER_TASK_COMPLETED:HOP_XFER_TASK_PARTIAL);
            status_end(HOP_XFER_DIR_SEND,j->batch,complete?HOP_XFER_COMPLETED:HOP_XFER_PARTIAL,
                       complete?"Transfer completed":"Some files failed");
        } else if (r == JOB_CANCELLED) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_CANCELLED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_CANCELLED,"Cancelled");
        } else if (r == JOB_PEER_ABORT) {
            hop_xfer_task_finish(&j->task,peer_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,peer_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,peer_cancelled?"Cancelled by receiver":"Peer aborted transfer");
        } else if (r == JOB_COMMIT_REJECTED) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Receiver rejected completion");
        } else if (r == JOB_COMMIT_UNKNOWN) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_INTERRUPTED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Completion delivery uncertain; peer may have received it");
        } else if (r == JOB_NET_DOWN) {
            hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
            status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Reconnect attempts exhausted");
        }
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
    task_history_store_open();
    memset(&s_x, 0, sizeof(s_x));
    s_x.sock = -1;

    int ck = cfg->xfer_chunk_kb;
    if (ck < 64) ck = 64;
    if (ck > 4096) ck = 4096;
    s_x.chunk = (uint32_t)ck * 1024u;
    rate_init_from_config(&s_x.rate, cfg->xfer_rate_limit_mib,
                          (uint64_t)s_x.chunk * 2u, rate_now_ns());
    hop_log(HOP_LOG_INFO, "xfer: adaptive rate initial %.1f MiB/s, ceiling %.1f MiB/s",
            (double)s_x.rate.current_rate_bytes_per_sec / XFER_RATE_MIB_BYTES,
            (double)s_x.rate.max_rate_bytes_per_sec / XFER_RATE_MIB_BYTES);

    pthread_mutex_init(&s_x.slock, NULL);
    pthread_mutex_init(&s_x.qlock, NULL);
    pthread_cond_init(&s_x.qcond, NULL);
    pthread_mutex_init(&s_ack.lock, NULL);
    pthread_cond_init(&s_ack.cond, NULL);
    hop_xfer_ack_window_reset(&s_ack.window);

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
    hop_xfer_task_history_store_close();
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
    /* Include a sender between dequeue/completion and retry requeue. */
    b=b||s_send_status.status.active||s_receive_status.status.active;
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
    __sync_lock_test_and_set(&s_x.cancel_rx, 1);
    pthread_cond_signal(&s_x.qcond);      /* 唤醒发送线程，别让它等满 200 ms */
    pthread_mutex_unlock(&s_x.qlock);

    pthread_mutex_lock(&s_ack.lock);
    pthread_cond_broadcast(&s_ack.cond);  /* 唤醒可能正卡在等 ACK 的发送线程 */
    pthread_mutex_unlock(&s_ack.lock);

    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

int xfer_cancel_task(hop_xfer_task_id id) {
    if (!hop_xfer_task_request_cancel(id)) return 0;
    pthread_mutex_lock(&s_x.qlock); pthread_cond_signal(&s_x.qcond); pthread_mutex_unlock(&s_x.qlock);
    pthread_mutex_lock(&s_ack.lock); pthread_cond_broadcast(&s_ack.cond); pthread_mutex_unlock(&s_ack.lock);
    return 1;
}

static int submit(xf_job *j) {
    int duplicate = 0;
    pthread_mutex_lock(&s_x.qlock);
    if (s_x.cur_batch == j->batch) duplicate = 1;
    for (xf_job *q = s_x.q_head; q && !duplicate; q = q->next)
        if (q->batch == j->batch) duplicate = 1;
    if (!duplicate) {
        if (s_x.q_tail) s_x.q_tail->next = j;
        else            s_x.q_head = j;
        s_x.q_tail = j;
        pthread_cond_signal(&s_x.qcond);
    }
    pthread_mutex_unlock(&s_x.qlock);
    return !duplicate;
}

static int xfer_submit_paths_impl(const char *const *paths, int n, uint32_t requested_batch, uint64_t offer_id) {
    if (!paths || n <= 0) return 0;
    if (requested_batch && !(requested_batch & HOP_XFER_PASTE_BATCH_FLAG)) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop %d path(s)", n);
        return 0;
    }

    uint32_t batch = requested_batch;
    if (!batch) { do { batch = __sync_add_and_fetch(&s_x.batch_seq, 1); } while (!batch); }

    elist l;
    memset(&l, 0, sizeof(l));
    if (!hop_xfer_task_init_classified(&l.task,offer_id,requested_batch,batch,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES)) return 0;
    if (!hop_xfer_task_reserve_job(&l.task)) {
        uint64_t task_id=l.task.id;
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        hop_log(HOP_LOG_WARN, "xfer: global job budget exhausted (task=%llu)",
                (unsigned long long)task_id);
        return 0;
    }
    if (!hop_xfer_task_set_state(&l.task, HOP_XFER_TASK_SCANNING)) {
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }

    uint64_t scan_started_ns=rate_now_ns();
    for (int i = 0; i < n; ++i) {
        if (!paths[i] || !paths[i][0]) { l.failed = 1; break; }
        const char *bn = strrchr(paths[i], '/');
        bn = bn ? bn + 1 : paths[i];
        walk_into(&l, paths[i], bn);
        if (l.failed) break;
    }
    uint64_t scan_elapsed_ns=rate_now_ns()-scan_started_ns;

    if (l.failed || l.n == 0 || hop_xfer_task_cancel_requested(l.task.id)) {
        if (l.budget_rejected)
            hop_log(HOP_LOG_WARN, "xfer: global entries/pending-bytes budget exceeded; batch rejected (task=%llu)",
                    (unsigned long long)l.task.id);
        else
            hop_log(HOP_LOG_WARN, "xfer: source tree rejected; no partial transfer queued (task=%llu)",
                    (unsigned long long)l.task.id);
        hop_xfer_task_finish(&l.task, hop_xfer_task_cancel_requested(l.task.id) ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }

    xf_job *job = (xf_job *)calloc(1, sizeof(*job));
    if (!job) {
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }

    job->batch = batch;
    job->scan_ns = scan_elapsed_ns;
    job->n = l.n;
    job->e = l.v;
    if (!hop_xfer_task_set_state(&l.task, HOP_XFER_TASK_QUEUED) ||
        !hop_xfer_task_move(&job->task, &l.task)) {
        free(job);
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }
    l.v = NULL; l.n = l.cap = 0;

    uint64_t total = 0;
    for (int i = 0; i < job->n; ++i) total += job->e[i].size;

    hop_log(HOP_LOG_INFO, "xfer: queued batch %u task=%llu, %d entries, %.1f MB",
            job->batch, (unsigned long long)job->task.id, job->n, (double)total / 1048576.0);

    if (!submit(job)) {
        hop_xfer_task_finish(&job->task, HOP_XFER_TASK_FAILED);
        job_free(job);
        return 0;
    }
    return 1;
}

static void release_image_source_reservation(hop_xfer_task *source_task, uint64_t bytes) {
    if (source_task && bytes &&
        !hop_xfer_task_release_image_temp(source_task, bytes)) {
        hop_log(HOP_LOG_ERROR, "xfer: source image reservation release failed (task=%llu, bytes=%llu)",
                (unsigned long long)source_task->id, (unsigned long long)bytes);
    }
}

static int xfer_submit_image_impl(const uint8_t *bgra, uint8_t *owned, int w, int h, int stride,
                                  hop_xfer_task *source_task, uint64_t source_reserved_bytes) {
    uint64_t min_stride,total64;
    if (!bgra || w <= 0 || h <= 0 || stride <= 0) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    min_stride=(uint64_t)(uint32_t)w*4u;
    if ((uint64_t)(uint32_t)stride<min_stride ||
        (uint64_t)(uint32_t)stride>UINT64_MAX/(uint64_t)(uint32_t)h) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    total64=(uint64_t)(uint32_t)stride*(uint64_t)(uint32_t)h;
    if (source_task && (!owned || source_reserved_bytes != total64)) {
        if (owned) free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    if(total64>(uint64_t)SIZE_MAX||!xfer_connected()){
        if(owned)free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }

    uint32_t batch;
    do { batch = __sync_add_and_fetch(&s_x.batch_seq, 1); } while (!batch);
    hop_xfer_task task;
    memset(&task,0,sizeof(task));
    if(!hop_xfer_task_init_classified(&task,0,0,batch,HOP_XFER_DIR_SEND,HOP_TASK_KIND_IMAGE)){
        if(owned)free(owned);
        release_image_source_reservation(source_task, source_reserved_bytes);
        return 0;
    }
    int job_reserved = hop_xfer_task_reserve_job(&task);
    int image_reserved = job_reserved &&
        (source_task ?
         hop_xfer_task_transfer_image_temp(source_task, &task, total64) :
         hop_xfer_task_reserve_image(&task, total64));
    if (!image_reserved) {
        uint64_t task_id=task.id;
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        if(owned)free(owned);
        /* A successful transfer moves the source reservation; a failed one
         * leaves it untouched, so this is safe only on the failure path. */
        release_image_source_reservation(source_task, source_reserved_bytes);
        hop_log(HOP_LOG_WARN,"xfer: global image memory/queue budget exhausted (task=%llu)",
                (unsigned long long)task_id);
        return 0;
    }

    xf_job *job=(xf_job*)calloc(1,sizeof(*job));
    if(!job){
        if(owned)free(owned);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    size_t total=(size_t)total64;
    uint8_t *copy=owned ? owned : (uint8_t*)malloc(total);
    if(!copy){
        free(job);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    if(!owned)memcpy(copy,bgra,total);

    if(!hop_xfer_task_set_state(&task,HOP_XFER_TASK_QUEUED)||
       !hop_xfer_task_move(&job->task,&task)){
        free(copy);free(job);
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        return 0;
    }
    job->batch=batch;
    job->is_image=1;
    job->pixels=copy;
    job->w=w;job->h=h;job->stride=stride;

    hop_log(HOP_LOG_INFO, "xfer: queued image %dx%d batch=%u task=%llu (%.1f MB)",
            w, h, job->batch, (unsigned long long)job->task.id, (double)total / 1048576.0);

    if (!submit(job)) {
        hop_xfer_task_finish(&job->task,HOP_XFER_TASK_FAILED);
        job_free(job);
        return 0;
    }
    return 1;
}

void xfer_kick(void) {
    pthread_mutex_lock(&s_x.slock);
    s_x.peer_ready = 0;
    if (s_x.sock >= 0) shutdown(s_x.sock, SHUT_RDWR);
    pthread_mutex_unlock(&s_x.slock);
}

int xfer_submit_paths(const char *const *paths, int n) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, 0, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_paths_for_paste(const char *const *paths, int n, uint32_t request_id, uint64_t offer_id) {
    if (!(request_id & HOP_XFER_PASTE_BATCH_FLAG) || !hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, request_id, offer_id);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_image_impl(bgra, NULL, w, h, stride, NULL, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image_owned(uint8_t *bgra, int w, int h, int stride) {
    if (!bgra) return 0;
    if (!hop_update_work_enter()) { free(bgra); return 0; }
    int result = xfer_submit_image_impl(bgra, bgra, w, h, stride, NULL, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image_owned_from_task(uint8_t *bgra, int w, int h, int stride,
                                       hop_xfer_task *source_task, uint64_t reserved_bytes) {
    if (!bgra) {
        release_image_source_reservation(source_task, reserved_bytes);
        return 0;
    }
    if (!hop_update_work_enter()) {
        free(bgra);
        release_image_source_reservation(source_task, reserved_bytes);
        return 0;
    }
    int result = xfer_submit_image_impl(bgra, bgra, w, h, stride, source_task, reserved_bytes);
    hop_update_work_leave();
    return result;
}
