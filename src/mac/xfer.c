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
#include "xfer_rx_fs.h"
#include "../core/xfer_rx_receipt.h"
#include "../core/xfer_rx_lifecycle.h"
#include "../core/xfer_rx_batch_rules.h"
#include "../core/xfer_rx_entry_reservations.h"
#include "../core/xfer_rx_file_state.h"
#include "../core/xfer_rx_dispatch.h"
#include "../core/xfer_rx_platform_dispatch.h"
#include "../core/xfer_rx_services.h"
#include "../core/xfer_rx_context.h"
#include "../core/xfer_rx_message_handlers.h"
#include "../core/xfer_rx_ack_handler.h"
#include "../core/xfer_rx_protocol_adapter.h"
#include "../core/xfer_rx_batch_state.h"
#include "../core/xfer_rx_image_state.h"
#include "../core/xfer_rx_image_buffer.h"
#include "xfer_writer_queue.h"
#include "../core/xfer_rx_queue.h"
#include "../core/update_state.h"
#include "../core/xfer_tracking.h"
#include "../core/xfer_budget.h"
#include "../core/xfer_task.h"
#include "../core/xfer_task_history.h"
#include "../core/xfer_rate.h"
#include "../core/xfer_ack_window.h"
#include "../core/xfer_buffer_pool.h"

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

#include "xfer_sender_protocol.inc"

#include "xfer_sender_worker.inc"

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
    s_ack = hop_xfer_ack_coordinator_create();
    if (!s_ack) {
        hop_log(HOP_LOG_ERROR, "xfer: ACK coordinator initialization failed");
        return;
    }

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

    hop_xfer_ack_coordinator_wake(s_ack);

    /* 必须 join：两个线程都会碰写盘队列，销毁队列时它们还活着就是释放后使用。
     * read/write 都是阻塞的，靠上面 shutdown(sock) 和 SO_SNDTIMEO 让它们返回。 */
    pthread_join(s_x.th_conn, NULL);
    pthread_join(s_x.th_send, NULL);

    rx_reset(&s_rx);      /* 先把写盘队列排空并关掉文件，再停写线程 */
    wq_stop();
    hop_xfer_ack_coordinator_destroy(s_ack);
    s_ack = NULL;
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

    hop_xfer_ack_coordinator_wake(s_ack);

    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

int xfer_cancel_task(hop_xfer_task_id id) {
    if (!hop_xfer_task_request_cancel(id)) return 0;
    pthread_mutex_lock(&s_x.qlock); pthread_cond_signal(&s_x.qcond); pthread_mutex_unlock(&s_x.qlock);
    hop_xfer_ack_coordinator_wake(s_ack);
    return 1;
}

#include "xfer_submit.inc"
