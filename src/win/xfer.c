/* =====================================================================
 *  HopDesk - Windows 文件/图片数据通道实现
 * ===================================================================== */
/* winsock2 必须在 windows.h 之前 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include "app.h"
#include "xfer.h"
#include "../core/hop.h"
#include "xfer_scanner.h"
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
#include "../core/xfer_sender_context.h"
#include "../core/xfer_sender_engine.h"
#include "../core/xfer_ack_window.h"
#include "../core/xfer_buffer_pool.h"

#if HOP_TASK_SESSION_BYTES != HOP_SESSION_BYTES
#error Task and protocol session sizes differ
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <io.h>
#include <time.h>

#define XFER_RX_ROOT      L"HopDesk"

/* ---------------- 条目 / 作业 ---------------- */

typedef struct xfer_job {
    struct xfer_job *next;
    hop_session_id session;
    uint32_t    batch;
    int         is_image;
    hop_xfer_task task;
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
    uint64_t    scan_ns;        /* source enumeration duration, recorded on first visible attempt */
} xfer_job;

/* ---------------- 全局状态 ---------------- */

static SRWLOCK s_status_lock = SRWLOCK_INIT;
static hop_xfer_tracking s_send_status, s_receive_status;
static uint64_t s_status_revision;
static wchar_t *s_retry_paths[HOP_XFER_FILE_LIMIT];
static hop_xfer_history_entry s_history[HOP_XFER_HISTORY_LIMIT];
static int s_history_count;
static SRWLOCK s_history_io_lock = SRWLOCK_INIT;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t count;
} hop_history_disk_header;
static wchar_t s_history_path[MAX_PATH];

static int history_prepare_path(void) {
    wchar_t base[MAX_PATH],directory[MAX_PATH];
    DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    if(!n||n>=MAX_PATH)return 0;
    if(swprintf(directory,MAX_PATH,L"%ls\\HopDesk",base)<0||
       swprintf(s_history_path,MAX_PATH,L"%ls\\transfer-history.bin",directory)<0)return 0;
    if(!CreateDirectoryW(directory,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return 0;
    return 1;
}
static void history_load(void) {
    if(!history_prepare_path())return;
    FILE *f=_wfopen(s_history_path,L"rb");if(!f)return;
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
                AcquireSRWLockExclusive(&s_status_lock);
                memcpy(s_history,rows,n*sizeof(rows[0]));s_history_count=(int)n;
                ReleaseSRWLockExclusive(&s_status_lock);
            }
        }
    }
    fclose(f);
}
static void task_history_store_open(void) {
    if (!s_history_path[0]) return;
    wchar_t path_w[MAX_PATH], directory[MAX_PATH];
    wcsncpy(directory,s_history_path,MAX_PATH-1); directory[MAX_PATH-1]=0;
    wchar_t *slash=wcsrchr(directory,L'\\\\');
    if (!slash) return;
    *slash=0;
    if (swprintf(path_w,MAX_PATH,L"%ls\\\\task-history.bin",directory)<0) return;
    char path[MAX_PATH*3], legacy[MAX_PATH*3];
    if (!WideCharToMultiByte(CP_UTF8,0,path_w,-1,path,sizeof(path),NULL,NULL) ||
        !WideCharToMultiByte(CP_UTF8,0,s_history_path,-1,legacy,sizeof(legacy),NULL,NULL)) return;
    if (!hop_xfer_task_history_store_open(path,legacy))
        hop_log(HOP_LOG_WARN,"xfer: durable task history unavailable; using session history only");
}
static void history_save(void) {
    AcquireSRWLockExclusive(&s_history_io_lock);
    if(!s_history_path[0]&&!history_prepare_path()){ReleaseSRWLockExclusive(&s_history_io_lock); return;}
    hop_xfer_history_entry rows[HOP_XFER_HISTORY_LIMIT];int count;
    AcquireSRWLockShared(&s_status_lock);
    count=s_history_count;
    if(count>HOP_XFER_HISTORY_LIMIT)count=HOP_XFER_HISTORY_LIMIT;
    memcpy(rows,s_history,(size_t)count*sizeof(rows[0]));
    ReleaseSRWLockShared(&s_status_lock);
    wchar_t temp[MAX_PATH];
    if(swprintf(temp,MAX_PATH,L"%ls.tmp",s_history_path)<0){ReleaseSRWLockExclusive(&s_history_io_lock); return;}
    FILE *f=_wfopen(temp,L"wb");if(!f){ReleaseSRWLockExclusive(&s_history_io_lock); return;}
    hop_history_disk_header h={{'H','O','P','X','H','S','T','1'},1,(uint32_t)sizeof(rows[0]),(uint32_t)count};
    int ok=fwrite(&h,sizeof(h),1,f)==1&&fwrite(rows,sizeof(rows[0]),(size_t)count,f)==(size_t)count;
    if(ok)ok=fflush(f)==0&&_commit(_fileno(f))==0;
    if(fclose(f)!=0)ok=0;
    if(ok){if(!MoveFileExW(temp,s_history_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))DeleteFileW(temp);}
    else DeleteFileW(temp);
    ReleaseSRWLockExclusive(&s_history_io_lock);
}

static hop_xfer_tracking *status_for(int direction) {
    return direction == HOP_XFER_DIR_SEND ? &s_send_status : &s_receive_status;
}
static int status_begin(uint32_t batch,int direction,uint32_t files,uint64_t bytes,const char *name,int phase) {
    int created=0;
    AcquireSRWLockExclusive(&s_status_lock);
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
    ReleaseSRWLockExclusive(&s_status_lock);
    return created;
}
static void status_chunk_timed(int direction,uint32_t batch,uint32_t id,uint64_t off,uint32_t n,
        uint64_t read_ns,uint64_t crc_ns,uint64_t send_ns,uint64_t receive_ns) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    hop_xfer_tracking_chunk(t,batch,id,off,n,hop_monotonic_ms());
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_READ,read_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_CRC,crc_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_SEND,send_ns);
    hop_xfer_tracking_add_time(t,batch,HOP_XFER_TIME_RECEIVE,receive_ns);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_stage_time(int direction,uint32_t batch,int stage,uint64_t elapsed_ns) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking_add_time(status_for(direction),batch,stage,elapsed_ns);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_file_phase(int direction,uint32_t batch,int index,int phase,uint32_t attempts) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking_phase(status_for(direction),batch,index,phase,attempts);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_end(int direction,uint32_t batch,int phase,const char *detail) {
    int changed=0;
    uint64_t timing[HOP_XFER_TIME_COUNT]={0};
    AcquireSRWLockExclusive(&s_status_lock);
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
    ReleaseSRWLockExclusive(&s_status_lock);
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
static void status_set_files(uint32_t batch,const xfer_entry *entries,int n) {
    AcquireSRWLockExclusive(&s_status_lock);
    if(hop_xfer_tracking_matches(&s_send_status,batch) && s_send_status.count==0) {
        s_send_status.status.bytes_total=0;
        for(int i=0;i<n && i<HOP_XFER_FILE_LIMIT;i++) {
            hop_xfer_tracking_file(&s_send_status,batch,(uint32_t)i+1,entries[i].rel?entries[i].rel:entries[i].name,
                entries[i].size,(entries[i].flags&HOP_FILE_FLAG_DIR)!=0,HOP_XFER_FILE_QUEUED);
            if(entries[i].full)s_retry_paths[i]=_wcsdup(entries[i].full);
        }
    }
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_image(uint32_t batch,int direction,uint64_t size) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    if(hop_xfer_tracking_matches(t,batch)) {
        if(t->count==0)t->status.bytes_total=0;
        hop_xfer_tracking_file(t,batch,1,"Image",size,0,
            direction==HOP_XFER_DIR_SEND?HOP_XFER_FILE_SENDING:HOP_XFER_FILE_RECEIVING);
    }
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_add_received_file(uint32_t batch,uint32_t id,const char *path,uint64_t size,int directory) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking_file(&s_receive_status,batch,id,path,size,directory,HOP_XFER_FILE_RECEIVING);
    if(hop_xfer_tracking_matches(&s_receive_status,batch) && path)
        snprintf(s_receive_status.status.name,sizeof(s_receive_status.status.name),"%s",path);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void job_bind_session(xfer_job *j, const hop_session_id *current) {
    if (!hop_session_id_valid(current) || hop_session_id_equal(&j->session,current)) return;
    j->session=*current; j->next_i=0; j->ok=0;
    AcquireSRWLockExclusive(&s_status_lock);
    memset(&s_send_status,0,sizeof(s_send_status));
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_start_job(const xfer_job *j) {
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
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking *t=status_for(direction);
    int n=t->status.batch==batch?t->count:0;if(n>capacity)n=capacity;
    memcpy(out,t->files,(size_t)n*sizeof(*out));
    ReleaseSRWLockExclusive(&s_status_lock);
    return n;
}
int xfer_retry_file(uint32_t batch,int index) {
    /* A paste batch carries a request and clipboard-version destination. A new
     * ordinary transfer cannot reproduce it; reject rather than overwrite the
     * peer clipboard or send to a changed target. Retry must be initiated as a
     * fresh, validated paste request on the receiving device. */
    if (!batch || (batch & HOP_XFER_PASTE_BATCH_FLAG)) return 0;
    wchar_t *path=NULL;
    AcquireSRWLockExclusive(&s_status_lock);
    if(s_send_status.status.batch==batch && !s_send_status.status.active && index>=0 && index<s_send_status.count &&
       s_send_status.files[index].phase==HOP_XFER_FILE_FAILED &&
       !s_send_status.files[index].is_directory && s_retry_paths[index])path=_wcsdup(s_retry_paths[index]);
    ReleaseSRWLockExclusive(&s_status_lock);
    if(!path)return 0;
    const wchar_t *paths[1]={path};int ok=xfer_submit_paths(paths,1);free(path);return ok;
}
int xfer_get_history(hop_xfer_history_entry *out,int capacity) {
    if(!out||capacity<=0)return 0;
    AcquireSRWLockExclusive(&s_status_lock);
    int n=s_history_count<capacity?s_history_count:capacity;memcpy(out,s_history,(size_t)n*sizeof(*out));
    ReleaseSRWLockExclusive(&s_status_lock);
    return n;
}

static struct {
    HWND             hwnd;
    hop_config       cfg;
    SOCKET           listen_s;
    SOCKET           client;
    volatile LONG    listening;
    hop_session_id   session;
    unsigned long    epoch;
    int              peer_ready;    /* 数据通道版本与 token 握手通过 */
    CRITICAL_SECTION lock;      /* 保护 client + 作业队列 + 取消标记 */
    HANDLE           stop;
    HANDLE           q_evt;
    HANDLE           rate_timer; /* high-resolution pacing wait; sender thread only */
    HANDLE           th_listen;
    HANDLE           th_send;
    xfer_job        *q_head, *q_tail;
    uint32_t         batch_seq;
    uint32_t         chunk;     /* 实际分块字节数 */
    hop_xfer_rate_limiter rate; /* sender thread only */

    /* ---- 取消 ----
     * cur_batch 由发送线程在开始一批时设置；cancel_batch 由 UI 线程设置。
     * 发送线程每块之后比一次，所以取消的响应延迟上限是一个分块（256 KB）。 */
    uint32_t         cur_batch;     /* 0 = 当前没有在发的批次 */
    hop_xfer_task_id cur_task_id;  /* same queue lock as cur_batch */
    uint32_t         cancel_batch;  /* 0 = 没有取消请求 */
    volatile LONG    cancel_rx;     /* 接收侧取消请求，由 listen 线程消费 */
    uint32_t         abort_batch;   /* 对端 ABORT 过的批次，0 = 无 */
    uint8_t          abort_reason;
} s_x;

void xfer_get_status_pair(hop_xfer_status_pair *out) {
    if (!out) return;
    uint32_t queued=0;
    EnterCriticalSection(&s_x.lock);
    for (xfer_job *j=s_x.q_head; j; j=j->next) queued++;
    LeaveCriticalSection(&s_x.lock);
    AcquireSRWLockShared(&s_status_lock);
    hop_xfer_tracking_snapshot_pair(&s_send_status,&s_receive_status,queued,out);
    ReleaseSRWLockShared(&s_status_lock);
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

/* Source scanning is implemented by xfer_scanner.c. */

/* ---------------- 发送 ---------------- */

/* 发送互斥：send_thread 和接收线程（出错时回 ABORT）会同时往同一个
 * socket 写。一次 send_all 内部可能分多次 send，两线程交错就会把
 * 帧字节搅在一起 —— 锁的粒度必须是"整帧"，不是"单次 send"。 */
static CRITICAL_SECTION s_txlock;
#ifdef _MSC_VER
static __declspec(thread) unsigned long s_send_epoch;
#else
static __thread unsigned long s_send_epoch;
#endif
static hop_xfer_sender_context s_sender_context;


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

static int sender_context_session_current(void *user, intptr_t socket_handle,
                                          uint64_t epoch) {
    (void)user;
    SOCKET sock = (SOCKET)socket_handle;
    EnterCriticalSection(&s_x.lock);
    int current = s_x.client == sock && (!epoch || s_x.epoch == epoch) &&
                  hop_session_matches(&s_x.session);
    LeaveCriticalSection(&s_x.lock);
    return current;
}

static int sender_context_write_frame(void *user, intptr_t socket_handle,
                                      uint64_t epoch,
                                      const hop_xfer_sender_frame_part *parts,
                                      size_t part_count) {
    (void)user;
    if (!parts || !part_count) return 0;
    SOCKET sock = (SOCKET)socket_handle;
    EnterCriticalSection(&s_txlock);
    int ok = sender_context_session_current(NULL, socket_handle, epoch);
    for (size_t i = 0; ok && i < part_count; ++i) {
        if ((!parts[i].data && parts[i].size) || parts[i].size > (size_t)INT_MAX) {
            ok = 0;
            break;
        }
        if (parts[i].size)
            ok = send_all(sock, (const char *)parts[i].data, (int)parts[i].size);
    }
    LeaveCriticalSection(&s_txlock);
    return ok;
}

static int sender_context_snapshot_session(SOCKET sock, hop_session_id *session,
                                           uint64_t *epoch) {
    if (!session || !epoch) return 0;
    EnterCriticalSection(&s_x.lock);
    int current = s_x.client == sock &&
                  (!s_send_epoch || s_x.epoch == s_send_epoch) &&
                  hop_session_matches(&s_x.session);
    if (current) {
        *session = s_x.session;
        *epoch = s_x.epoch;
    }
    LeaveCriticalSection(&s_x.lock);
    return current;
}

/* 每块之后让一次 CPU。传输是后台工作，键鼠才是前台 ——
 * 真正起作用的保障是 send_thread 里的 BELOW_NORMAL 优先级（输入线程
 * 是 NORMAL，会直接抢占）；这里让一次是给同优先级的其他工作减压。 */
static void yield_cpu(void) { Sleep(0); }

/* 小帧：头 + 载荷两次 send。载荷都在 1 KB 级别，不值得拼缓冲。 */
static int send_small(SOCKET c, uint8_t type, const uint8_t *p, uint32_t len, uint32_t batch) {
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    uint8_t hdr[HOP_HEADER_SIZE], prefix[HOP_DATA_PREFIX];
    hop_session_id session;
    uint64_t epoch = 0;
    int ok = s_sender_context.write_frame != NULL &&
             sender_context_snapshot_session(c, &session, &epoch);
    hop_xfer_sender_frame_part parts[3];
    size_t part_count = 0;
    if (ok && type != HOP_MSG_XFER_HELLO) {
        if (len > UINT32_MAX - HOP_DATA_PREFIX) ok = 0;
        else {
            hop_data_prefix(prefix, &session, batch);
            hop_encode_header(hdr, type, len + HOP_DATA_PREFIX);
            parts[part_count++] = (hop_xfer_sender_frame_part){hdr, sizeof(hdr)};
            parts[part_count++] = (hop_xfer_sender_frame_part){prefix, sizeof(prefix)};
        }
    } else if (ok) {
        hop_encode_header(hdr, type, len);
        parts[part_count++] = (hop_xfer_sender_frame_part){hdr, sizeof(hdr)};
    }
    if (ok && len) parts[part_count++] = (hop_xfer_sender_frame_part){p, len};
    if (ok) ok = s_sender_context.write_frame(s_sender_context.user, c, epoch,
                                               parts, part_count);
    hop_watchdog_end(send_guard);
    return ok;
}

/* 分块：一次 send 发完头 + 分块头 + 数据。
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

int xfer_init(HWND hwnd, const hop_config *cfg) {
    history_load();
    task_history_store_open();
    memset(&s_x, 0, sizeof(s_x));
    sender_context_init();
    s_x.hwnd = hwnd;
    s_x.cfg = *cfg;
    s_x.listen_s = INVALID_SOCKET;
    s_x.client = INVALID_SOCKET;

    int ck = cfg->xfer_chunk_kb;
    if (ck < 64) ck = 64;
    if (ck > 4096) ck = 4096;
    s_x.chunk = (uint32_t)ck * 1024u;
    rate_init_from_config(&s_x.rate, cfg->xfer_rate_limit_mib,
                          (uint64_t)s_x.chunk * 2u, rate_now_ns());
    hop_log(HOP_LOG_INFO, "xfer: adaptive rate initial %.1f MiB/s, ceiling %.1f MiB/s",
            (double)s_x.rate.current_rate_bytes_per_sec / XFER_RATE_MIB_BYTES,
            (double)s_x.rate.max_rate_bytes_per_sec / XFER_RATE_MIB_BYTES);

    InitializeCriticalSection(&s_x.lock);
    InitializeCriticalSection(&s_txlock);
    s_ack = hop_xfer_ack_coordinator_create();
    if (!s_ack) goto fail;
    s_x.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_x.q_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_x.rate_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_x.rate_timer)
        hop_log(HOP_LOG_WARN, "xfer: high-resolution rate timer unavailable; using fallback pacing");

    if (!s_x.stop || !s_x.q_evt) goto fail;

    /* 写盘线程要先起来，接收路径一进来就会往队列里塞 */
    if (!wq_start()) goto fail;

    s_x.th_listen = CreateThread(NULL, 0, cfg->role==HOP_ROLE_SERVER?listen_thread:data_client_thread, NULL, 0, NULL);
    s_x.th_send = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    if (!s_x.th_listen || !s_x.th_send) goto fail;

    hop_log(HOP_LOG_INFO, "xfer: listening on port %u, chunk %u KB",
            (unsigned)cfg->data_port, ck);
    return 1;

fail:
    if (s_x.stop) SetEvent(s_x.stop);
    if (s_x.th_listen) { WaitForSingleObject(s_x.th_listen, INFINITE); CloseHandle(s_x.th_listen); s_x.th_listen = NULL; }
    if (s_x.th_send) { WaitForSingleObject(s_x.th_send, INFINITE); CloseHandle(s_x.th_send); s_x.th_send = NULL; }
    if (s_x.listen_s != INVALID_SOCKET) closesocket(s_x.listen_s);
    wq_stop();
    hop_xfer_task_history_store_close();
    if (s_x.stop) CloseHandle(s_x.stop);
    if (s_x.q_evt) CloseHandle(s_x.q_evt);
    if (s_x.rate_timer) { CloseHandle(s_x.rate_timer); s_x.rate_timer = NULL; }
    hop_xfer_ack_coordinator_destroy(s_ack);
    s_ack = NULL;
    DeleteCriticalSection(&s_x.lock);
    DeleteCriticalSection(&s_txlock);
    return 0;
}

void xfer_shutdown(void) {
    SetEvent(s_x.stop);
    hop_xfer_ack_coordinator_wake(s_ack); /* 踢醒可能正卡在等 ACK 的发送线程 */
    /* listener/connection thread owns closesocket; shutdown only wakes I/O. */

    EnterCriticalSection(&s_x.lock);
    if (s_x.client != INVALID_SOCKET) {
        /* shutdown 让对端 recv 立刻返回，send 也会立刻失败 ——
         * 这样两个线程都能迅速退出，不会卡在阻塞调用里 */
        shutdown(s_x.client, SD_BOTH);

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

    rx_reset(&s_rx);      /* 先把写盘队列排空并关掉文件，再停写线程 */
    wq_stop();
    hop_xfer_task_history_store_close();
    CloseHandle(s_x.stop);
    CloseHandle(s_x.q_evt);
    if (s_x.rate_timer) { CloseHandle(s_x.rate_timer); s_x.rate_timer = NULL; }
    hop_xfer_ack_coordinator_destroy(s_ack);
    s_ack = NULL;
    DeleteCriticalSection(&s_x.lock);
    DeleteCriticalSection(&s_txlock);
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
    AcquireSRWLockExclusive(&s_status_lock);
    /* Include a sender between dequeue/completion and retry requeue. */
    b=b||s_send_status.status.active||s_receive_status.status.active;
    ReleaseSRWLockExclusive(&s_status_lock);
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
    hop_xfer_ack_coordinator_wake(s_ack); /* 唤醒可能正卡在等 ACK 的发送线程 */
    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

int xfer_cancel_task(hop_xfer_task_id id) {
    if (!hop_xfer_task_request_cancel(id)) return 0;
    SetEvent(s_x.q_evt);
    hop_xfer_ack_coordinator_wake(s_ack);
    return 1;
}

#include "xfer_submit.inc"
