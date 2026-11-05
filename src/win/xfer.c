/* =====================================================================
 *  HopDesk - Windows 文件/图片数据通道实现
 * ===================================================================== */
/* winsock2 必须在 windows.h 之前 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include "app.h"
#include "xfer.h"
#include "../core/update_state.h"
#include "../core/xfer_tracking.h"
#include "../core/xfer_budget.h"
#include "../core/xfer_task.h"
#include "../core/xfer_rate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <time.h>

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
} xfer_job;

typedef struct {
    xfer_entry *v;
    int n, cap;
    int failed; /* incomplete/unsafe snapshots are never queued */
    int budget_rejected;
    hop_xfer_task task;
} elist;

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
static void status_begin(uint32_t batch,int direction,uint32_t files,uint64_t bytes,const char *name,int phase) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking *t = status_for(direction);
    /* The FIFO sender retains this snapshot while retrying the same job. */
    if (!hop_xfer_tracking_matches(t,batch)) {
        if (direction == HOP_XFER_DIR_SEND)
            for(int i=0;i<HOP_XFER_FILE_LIMIT;i++){free(s_retry_paths[i]);s_retry_paths[i]=NULL;}
        hop_xfer_tracking_begin(t,batch,direction,files,bytes,name,phase);
    } else t->status.phase=phase;
    t->revision=++s_status_revision;
    snprintf(t->status.detail,sizeof(t->status.detail),"%s",direction==HOP_XFER_DIR_SEND?"Sending to peer":"Receiving from peer");
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_chunk(int direction,uint32_t batch,uint32_t id,uint64_t off,uint32_t n) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking_chunk(status_for(direction),batch,id,off,n);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_file_phase(int direction,uint32_t batch,int index,int phase,uint32_t attempts) {
    AcquireSRWLockExclusive(&s_status_lock);
    hop_xfer_tracking_phase(status_for(direction),batch,index,phase,attempts);
    ReleaseSRWLockExclusive(&s_status_lock);
}
static void status_end(int direction,uint32_t batch,int phase,const char *detail) {
    int changed=0;
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
        if(s_history_count<HOP_XFER_HISTORY_LIMIT)s_history_count++;
    }
    ReleaseSRWLockExclusive(&s_status_lock);
    if(changed)history_save();
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
    status_begin(j->batch,HOP_XFER_DIR_SEND,hop_xfer_job_total(j->is_image,j->n),total_bytes,
                 j->is_image?"Image":j->e[0].name,HOP_XFER_SENDING);
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
    wchar_t *path=NULL;
    AcquireSRWLockExclusive(&s_status_lock);
    if(s_send_status.status.batch==batch && index>=0 && index<s_send_status.count &&
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
    uint32_t         cancel_batch;  /* 0 = 没有取消请求 */
    volatile LONG    cancel_rx;     /* 接收侧取消请求，由 listen 线程消费 */
    uint32_t         abort_batch;   /* 对端 ABORT 过的批次，0 = 无 */
} s_x;

void xfer_get_status(hop_xfer_status *out) {
    if(!out)return; uint32_t queued=0;
    EnterCriticalSection(&s_x.lock);
    for(xfer_job *j=s_x.q_head;j;j=j->next)queued++;
    LeaveCriticalSection(&s_x.lock);
    AcquireSRWLockShared(&s_status_lock);
    *out=hop_xfer_tracking_visible(&s_send_status,&s_receive_status)->status;
    out->queued_count=queued;
    ReleaseSRWLockShared(&s_status_lock);
}

void xfer_get_budget_snapshot(hop_xfer_budget_snapshot *out) {
    hop_xfer_budget_get_snapshot(out);
}

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
        xfer_entry *nv = (xfer_entry *)realloc(l->v, (size_t)nc * sizeof(*nv));
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

/* 递归收集。rel 是 UTF-8 相对路径（不含首尾斜杠）。 */
static BOOL guarded_find_next(HANDLE h, WIN32_FIND_DATAW *data) {
    int guard = hop_watchdog_begin("directory next", HOP_IO_DEADLINE_MS);
    BOOL ok = FindNextFileW(h, data);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    hop_watchdog_end(guard);
    if (!ok) SetLastError(error);
    return ok;
}
static void walk_into(elist *l, const wchar_t *full, const char *rel) {
    if (!l || l->failed) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int stat_guard = hop_watchdog_begin("file attributes", HOP_IO_DEADLINE_MS);
    int stat_ok = GetFileAttributesExW(full, GetFileExInfoStandard, &fad);
    hop_watchdog_end(stat_guard);
    if (!stat_ok) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: source path disappeared or is inaccessible");
        return;
    }
    /* Do not follow symlinks, junctions or other reparse points. */
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: reparse point rejected from file selection");
        return;
    }

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
        l->failed = 1;
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
    if (!pat) { l->failed = 1; return; }
    wcscpy(pat, full);
    if (dl && pat[dl - 1] != L'\\' && pat[dl - 1] != L'/') wcscat(pat, L"\\");
    wcscat(pat, L"*");

    WIN32_FIND_DATAW fd;
    int find_guard = hop_watchdog_begin("directory enumerate", HOP_IO_DEADLINE_MS);
    HANDLE h = FindFirstFileW(pat, &fd);
    hop_watchdog_end(find_guard);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        l->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: cannot enumerate selected directory");
        return;
    }

    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;

        size_t fl = wcslen(full);
        int need_sep = (fl && full[fl - 1] != L'\\' && full[fl - 1] != L'/');
        wchar_t *child = (wchar_t *)malloc((fl + wcslen(fd.cFileName) + 3) * sizeof(wchar_t));
        if (!child) { l->failed = 1; break; }
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
        if (!crel) l->failed = 1;
        else walk_into(l, child, crel);
        free(crel);
        free(child);
        if (l->failed) break;
    } while (guarded_find_next(h, &fd));

    if (!l->failed && GetLastError() != ERROR_NO_MORE_FILES) l->failed = 1;
    FindClose(h);
}

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
static int send_session_current(SOCKET sock) {
    EnterCriticalSection(&s_x.lock);
    int ok = s_x.client == sock && (!s_send_epoch || s_x.epoch == s_send_epoch) && hop_session_matches(&s_x.session);
    LeaveCriticalSection(&s_x.lock); return ok;
}

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
static int send_small(SOCKET c, uint8_t type, const uint8_t *p, uint32_t len, uint32_t batch) {
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    uint8_t hdr[HOP_HEADER_SIZE], prefix[HOP_DATA_PREFIX];
    EnterCriticalSection(&s_txlock);
    int ok = send_session_current(c);
    if (ok && type != HOP_MSG_XFER_HELLO) {
        EnterCriticalSection(&s_x.lock);
        hop_data_prefix(prefix, &s_x.session, batch);
        LeaveCriticalSection(&s_x.lock);
        hop_encode_header(hdr, type, len + HOP_DATA_PREFIX);
        ok = send_all(c, (const char *)hdr, HOP_HEADER_SIZE) && send_all(c, (const char *)prefix, HOP_DATA_PREFIX);
    } else if (ok) {
        hop_encode_header(hdr, type, len);
        ok = send_all(c, (const char *)hdr, HOP_HEADER_SIZE);
    }
    if (ok && len) ok = send_all(c, (const char *)p, len);
    LeaveCriticalSection(&s_txlock);
    hop_watchdog_end(send_guard);
    return ok;
}

/* 分块：一次 send 发完头 + 分块头 + 数据。
 * 数据直接读进发送缓冲的 CHUNK_HDR 处，省掉一次内存拷贝。
 * 每块带 CRC32：TCP 的 16 位校验和配合网卡 offload 有可能漏检，
 * GB 级传输漏检一次就是静默损坏。 */
#define CHUNK_HDR (25u + HOP_DATA_PREFIX)   /* 5 帧头 + 4 id + 8 off + 4 len + 4 crc */

#define XFER_RATE_INITIAL_BPS (128ull * 1024ull * 1024ull)
#define XFER_RATE_MIN_BPS     (64ull * 1024ull * 1024ull)
#define XFER_RATE_MAX_BPS     (512ull * 1024ull * 1024ull)

static uint64_t rate_now_ns(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    uint64_t q = (uint64_t)counter.QuadPart;
    uint64_t f = (uint64_t)frequency.QuadPart;
    return (q / f) * 1000000000ull + ((q % f) * 1000000000ull) / f;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static void rate_sleep_ns(uint64_t ns) {
    if (s_x.rate_timer) {
        LARGE_INTEGER due;
        uint64_t units = (ns + 99ull) / 100ull; /* negative 100 ns relative due time */
        if (!units) units = 1;
        due.QuadPart = -(LONGLONG)units;
        if (SetWaitableTimer(s_x.rate_timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE waits[2] = {s_x.stop, s_x.rate_timer};
            (void)WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            return;
        }
    }
    /* Old Windows fallback: coarse waits for longer intervals, QPC yield for
     * sub-millisecond pacing. Modern Windows uses the high-resolution timer. */
    if (ns >= 1000000ull) {
        DWORD ms = (DWORD)(ns / 1000000ull);
        if (ms > 10) ms = 10;
        if (ms) Sleep(ms);
        return;
    }
    LARGE_INTEGER start, now, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    do {
        SwitchToThread();
        QueryPerformanceCounter(&now);
    } while (((uint64_t)(now.QuadPart - start.QuadPart) * 1000000000ull) /
             (uint64_t)frequency.QuadPart < ns);
}

static int rate_wait_for_chunk(uint32_t bytes) {
    for (;;) {
        uint64_t delay = hop_xfer_rate_wait_ns(&s_x.rate, bytes, rate_now_ns());
        if (delay == UINT64_MAX) {
            hop_log(HOP_LOG_ERROR, "xfer: chunk exceeds token-bucket burst");
            return 0;
        }
        if (!delay) return 1;
        if (WaitForSingleObject(s_x.stop, 0) == WAIT_OBJECT_0) return 0;
        rate_sleep_ns(delay);
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

static int send_chunk(SOCKET c, uint8_t *tx, uint32_t batch, uint32_t id, uint64_t off,
                      const uint8_t *data, uint32_t len, uint32_t crc) {
    if (!rate_wait_for_chunk(len)) return 0;
    int send_guard = hop_watchdog_begin("data frame send", 10000);
    hop_encode_header(tx, HOP_MSG_FILE_CHUNK, 20 + HOP_DATA_PREFIX + len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 5, id);
    hop_put_be64(tx + HOP_DATA_PREFIX + 9, off);
    hop_put_be32(tx + HOP_DATA_PREFIX + 17, len);
    hop_put_be32(tx + HOP_DATA_PREFIX + 21, crc);
    EnterCriticalSection(&s_txlock);
    EnterCriticalSection(&s_x.lock);
    hop_data_prefix(tx + HOP_HEADER_SIZE, &s_x.session, batch);
    LeaveCriticalSection(&s_x.lock);
    uint64_t send_started = rate_now_ns();
    int ok = send_session_current(c) &&
             (data == tx + CHUNK_HDR ?
              send_all(c, (const char *)tx, (int)(CHUNK_HDR + len)) :
              (send_all(c, (const char *)tx, (int)CHUNK_HDR) &&
               send_all(c, (const char *)data, (int)len)));
    uint64_t send_us = (rate_now_ns() - send_started) / 1000ull;
    hop_xfer_rate_note_send_time(&s_x.rate, len, send_us);
    LeaveCriticalSection(&s_txlock);
    if(ok) status_chunk(HOP_XFER_DIR_SEND,batch,id,off,len);
    hop_watchdog_end(send_guard);
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

    int ok = send_small(c, HOP_MSG_FILE_BEGIN, bp, blen, batch);
    free(bp);
    if (!ok) return XFER_SEND_DEAD;

    /* 整文件校验 = 对各块 CRC 再做一次累加 CRC。
     * 每块的 CRC 已经覆盖了它自己的数据，这里只需再覆盖"块的顺序与偏移"，
     * 不必把整份文件重算一遍 —— GB 级文件重算就是白白多一次全量读。
     * 这样既抓得到数据损坏（块 CRC），也抓得到错位/乱序（文件 CRC）。 */
    uint32_t fcrc = 0;
    uint8_t  end_status = HOP_FILE_OK;
    int      local_fail = 0;

    if (!(e->flags & HOP_FILE_FLAG_DIR)) {
        int open_guard = hop_watchdog_begin("file open", HOP_IO_DEADLINE_MS);
        HANDLE h = CreateFileW(e->full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        DWORD open_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        hop_watchdog_end(open_guard);
        if (h == INVALID_HANDLE_VALUE) {
            hop_log(HOP_LOG_WARN, "xfer: open failed, %s (err %lu)",
                    e->rel, open_error);
            end_status = HOP_FILE_IO_ERROR;
            local_fail = 1;
        } else {
            uint64_t off = 0;
            while (off < e->size) {
                uint64_t remaining = e->size - off;
                DWORD request = (DWORD)(remaining < s_x.chunk ? remaining : s_x.chunk);
                DWORD got = 0;
                int read_guard = hop_watchdog_begin("file chunk read", HOP_IO_DEADLINE_MS);
                int read_ok = ReadFile(h, tx + CHUNK_HDR, request, &got, NULL);
                DWORD read_error = read_ok ? ERROR_SUCCESS : GetLastError();
                hop_watchdog_end(read_guard);
                if (!read_ok) {
                    DWORD error = read_error;
                    hop_log(HOP_LOG_WARN, "xfer: read failed, %s (err %lu)", e->rel, error);
                    end_status = HOP_FILE_IO_ERROR;
                    local_fail = 1;
                    break;
                }
                if (got == 0) {
                    hop_log(HOP_LOG_WARN, "xfer: unexpected EOF, %s (%llu/%llu bytes)",
                            e->rel, (unsigned long long)off, (unsigned long long)e->size);
                    end_status = HOP_FILE_IO_ERROR;
                    local_fail = 1;
                    break;
                }
                uint32_t ccrc = hop_crc32(0, tx + CHUNK_HDR, got);
                if (!send_chunk(c, tx, batch, id, off, tx + CHUNK_HDR, got, ccrc)) {
                    CloseHandle(h);
                    return XFER_SEND_DEAD;
                }
                fcrc = hop_xfer_file_crc_step(fcrc, off, ccrc);
                off += got;
                yield_cpu();
                if (cancel_requested(batch)) {
                    CloseHandle(h);
                    return XFER_CANCELLED;
                }
            }
            if (!local_fail && off == e->size) {
                BYTE extra = 0; DWORD extra_got = 0;
                int read_guard = hop_watchdog_begin("file size probe", HOP_IO_DEADLINE_MS);
                int read_ok = ReadFile(h, &extra, 1, &extra_got, NULL);
                DWORD probe_error = read_ok ? ERROR_SUCCESS : GetLastError();
                hop_watchdog_end(read_guard);
                if (!read_ok || extra_got != 0) {
                    if (!read_ok) SetLastError(probe_error);
                    hop_log(HOP_LOG_WARN, "xfer: source changed while queued, %s", e->rel);
                    end_status = HOP_FILE_IO_ERROR;
                    local_fail = 1;
                }
            }
            CloseHandle(h);
        }
    }

    uint8_t endp[9];
    hop_put_be32(endp, id);
    endp[4] = end_status;
    hop_put_be32(endp + 5, fcrc);
    if (!send_small(c, HOP_MSG_FILE_END, endp, 9, batch)) return XFER_SEND_DEAD;

    /* 本地就没读成功，没必要再等回执：接收端那份注定是残缺的，
     * 等到的结论也只会是"失败"。直接进重试。 */
    if (local_fail) return XFER_LOCAL_IO;

    uint64_t ack_started = rate_now_ns();
    int ack_status = ack_wait(batch, id);
    rate_feedback((rate_now_ns() - ack_started) / 1000ull);
    return ack_status;
}

static int send_image_job(SOCKET c, uint8_t *tx, xfer_job *j) {
    uint8_t bp[20];
    hop_put_be32(bp, j->batch);
    hop_put_be32(bp + 4, 1);
    hop_put_be32(bp + 8, (uint32_t)j->w);
    hop_put_be32(bp + 12, (uint32_t)j->h);
    hop_put_be32(bp + 16, (uint32_t)j->stride);
    if (!send_small(c, HOP_MSG_IMAGE_BEGIN, bp, 20, j->batch)) return 0;

    uint64_t total = (uint64_t)j->stride * (uint64_t)j->h;
    uint64_t off = 0;
    while (off < total) {
        uint32_t n = (uint32_t)((total - off) < s_x.chunk ? (total - off) : s_x.chunk);
        uint32_t crc = hop_crc32(0, j->pixels + off, n);
        if (!send_chunk(c, tx, j->batch, 1, off, j->pixels + off, n, crc)) return 0;
        off += n;
        yield_cpu();
        if (cancel_requested(j->batch)) return -1;
        if (peer_aborted(j->batch)) return -2;
    }

    uint8_t endp[9];
    hop_put_be32(endp, 1);
    endp[4] = HOP_FILE_OK;
    hop_put_be32(endp + 5, 0);
    if (!send_small(c, HOP_MSG_FILE_END, endp, 9, j->batch)) return 0;
    rate_feedback(0);
    return 1;
}

/* 收尾：告诉接收端本批实际送达了多少。
 * ok < total 时接收端会报"部分成功" —— 这是用户唯一能拿到准确结论的地方，
 * 所以 count 必须是真实值，不能像以前那样写死 0。 */
static int send_done(SOCKET c, const xfer_job *j) {
    uint8_t dp[20];
    hop_put_be32(dp,      j->batch);
    hop_put_be32(dp + 4,  (uint32_t)j->ok);
    hop_put_be32(dp + 8,  hop_xfer_job_total(j->is_image,j->n));
    hop_put_be64(dp + 12, 0);
    return send_small(c, HOP_MSG_XFER_DONE, dp, 20, j->batch);
}

/* run_job 的结局 */
#define JOB_DONE        0   /* 全部条目都有结论了（可能部分失败），该送 DONE */
#define JOB_NET_DOWN    1   /* 链路断了，整批退回队列等重连 */
#define JOB_CANCELLED   2   /* 用户取消，通知对端丢弃，不送 DONE */
#define JOB_PEER_ABORT  3   /* 对端中止，什么都不用送 */

/* 实际发送（send 线程持有 tx 缓冲）。 */
static int run_job(SOCKET c, uint8_t *tx, xfer_job *j) {

    if(cancel_requested(j->batch))return JOB_CANCELLED;
    if(peer_aborted(j->batch))return JOB_PEER_ABORT;
    if (j->is_image) {
        int image_result = send_image_job(c, tx, j);
        if (image_result == -1) return JOB_CANCELLED;
        if (image_result == -2) return JOB_PEER_ABORT;
        if (!image_result) return JOB_NET_DOWN;
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

        const xfer_entry *e = &j->e[j->next_i];
        status_file_phase(HOP_XFER_DIR_SEND,j->batch,j->next_i,HOP_XFER_FILE_SENDING,0);
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
    hop_xfer_task_dispose(&j->task);
    free(j);
}

/* 发一个批量级 ABORT（取消时通知对端丢弃本批） */
static void send_abort_msg(SOCKET c, uint32_t batch, uint8_t reason) {
    uint8_t ab[5];
    hop_put_be32(ab, batch);
    ab[4] = reason;
    send_small(c, HOP_MSG_XFER_ABORT, ab, 5, batch);
}

/* 接收线程调用：把一个文件的结论真正发回对端。
 * 注意别和 ack_deliver 搞混 —— 那个是"收到 ACK"的入站路径。 */
static void send_ack(SOCKET c, uint32_t batch, uint32_t id, int status) {
    uint8_t b[9];
    hop_put_be32(b, batch);
    hop_put_be32(b + 4, id);
    b[8] = (uint8_t)status;
    send_small(c, HOP_MSG_FILE_ACK, b, 9, batch);
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
            SOCKET c = s_x.peer_ready ? s_x.client : INVALID_SOCKET;
            s_send_epoch = s_x.epoch;
            hop_session_id transport_session = s_x.session;
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
                if (cur->batch==s_x.cancel_batch || (cur->give_up_at && now >= cur->give_up_at)) {
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
                break; /* FIFO: do not overtake a paused batch. */
            }
            if (j) s_x.cur_batch = j->batch;
            LeaveCriticalSection(&s_x.lock);

            if (expired) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: batch %u abandoned, peer never came back (%d/%d delivered)",
                        expired->batch, expired->ok, expired->n);
                status_start_job(expired);
                int was_cancelled=cancel_requested(expired->batch);
                hop_xfer_task_finish(&expired->task,was_cancelled?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_FAILED);
                status_end(HOP_XFER_DIR_SEND,expired->batch,was_cancelled?HOP_XFER_CANCELLED:HOP_XFER_FAILED,"Transfer stopped while queued");
                job_free(expired);
                continue;
            }

            if (!j) break;
            job_bind_session(j, &transport_session);
            if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_ACTIVE)){
                hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);job_free(j);continue;
            }
            status_start_job(j);

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
                    if(!hop_xfer_task_set_state(&j->task,HOP_XFER_TASK_QUEUED)){
                        hop_log(HOP_LOG_ERROR,"xfer: task=%llu could not return to queued state",
                                (unsigned long long)j->task.id);
                        status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Invalid task retry state");
                        hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                        job_free(j);continue;
                    }
                    /* 退回队首，保持 FIFO，next_i 保留 —— 重连后从断点条目继续，不重传已确认的 */
                    j->next = NULL;
                    EnterCriticalSection(&s_x.lock);
                    j->next=s_x.q_head; s_x.q_head=j;
                    if(!s_x.q_tail)s_x.q_tail=j;
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
            if(r==JOB_DONE){
                int complete=j->ok==(int)hop_xfer_job_total(j->is_image,j->n);
                hop_xfer_task_finish(&j->task,complete?HOP_XFER_TASK_COMPLETED:HOP_XFER_TASK_PARTIAL);
                status_end(HOP_XFER_DIR_SEND,j->batch,complete?HOP_XFER_COMPLETED:HOP_XFER_PARTIAL,complete?"Transfer completed":"Some files failed");
            } else if(r==JOB_CANCELLED){
                hop_xfer_task_finish(&j->task,HOP_XFER_TASK_CANCELLED);
                status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_CANCELLED,"Cancelled");
            } else if(r==JOB_PEER_ABORT){
                hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Peer aborted transfer");
            } else if(r==JOB_NET_DOWN){
                hop_xfer_task_finish(&j->task,HOP_XFER_TASK_FAILED);
                status_end(HOP_XFER_DIR_SEND,j->batch,HOP_XFER_FAILED,"Reconnect attempts exhausted");
            }
            job_free(j);
        }
    }
    free(tx);
    return 0;
}

/* Receiver and disk writer share this translation unit and its private state. */
#include "xfer_receive.inc"

/* ---------------- 对外接口 ---------------- */

int xfer_init(HWND hwnd, const hop_config *cfg) {
    history_load();
    memset(&s_x, 0, sizeof(s_x));
    s_x.hwnd = hwnd;
    s_x.cfg = *cfg;
    s_x.listen_s = INVALID_SOCKET;
    s_x.client = INVALID_SOCKET;

    int ck = cfg->xfer_chunk_kb;
    if (ck < 64) ck = 64;
    if (ck > 4096) ck = 4096;
    s_x.chunk = (uint32_t)ck * 1024u;
    hop_xfer_rate_init(&s_x.rate, XFER_RATE_INITIAL_BPS, XFER_RATE_MIN_BPS,
                       XFER_RATE_MAX_BPS, (uint64_t)s_x.chunk * 2u, rate_now_ns());

    InitializeCriticalSection(&s_x.lock);
    InitializeCriticalSection(&s_txlock);
    InitializeCriticalSection(&s_ack.lock);
    s_ack.ev = CreateEventW(NULL, FALSE, FALSE, NULL);   /* 自动重置 */
    if (!s_ack.ev) goto fail;
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
    if (s_x.stop) CloseHandle(s_x.stop);
    if (s_x.q_evt) CloseHandle(s_x.q_evt);
    if (s_x.rate_timer) { CloseHandle(s_x.rate_timer); s_x.rate_timer = NULL; }
    if (s_ack.ev) { CloseHandle(s_ack.ev); s_ack.ev = NULL; }
    DeleteCriticalSection(&s_x.lock);
    DeleteCriticalSection(&s_txlock);
    DeleteCriticalSection(&s_ack.lock);
    return 0;
}

void xfer_shutdown(void) {
    SetEvent(s_x.stop);
    SetEvent(s_ack.ev);          /* 踢醒可能正卡在等 ACK 的发送线程 */
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

    rx_reset();      /* 先把写盘队列排空并关掉文件，再停写线程 */
    wq_stop();
    CloseHandle(s_x.stop);
    CloseHandle(s_x.q_evt);
    if (s_x.rate_timer) { CloseHandle(s_x.rate_timer); s_x.rate_timer = NULL; }
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
    SetEvent(s_ack.ev);       /* 唤醒可能正卡在等 ACK 的发送线程 */
    hop_log(HOP_LOG_INFO, "xfer: cancel requested");
}

static int submit(xfer_job *j) {
    int ok = 0, duplicate = 0;
    EnterCriticalSection(&s_x.lock);
    if (s_x.cur_batch == j->batch) duplicate = 1;
    for (xfer_job *q = s_x.q_head; q && !duplicate; q = q->next)
        if (q->batch == j->batch) duplicate = 1;
    if (!duplicate && s_x.client != INVALID_SOCKET && s_x.peer_ready) {
        if (s_x.q_tail) s_x.q_tail->next = j;
        else            s_x.q_head = j;
        s_x.q_tail = j;
        ok = 1;
    }
    LeaveCriticalSection(&s_x.lock);
    if (ok) SetEvent(s_x.q_evt);
    return ok;
}

static int xfer_submit_paths_impl(const wchar_t *const *paths, int n, uint32_t requested_batch, uint64_t offer_id) {
    if (!paths || n <= 0) return 0;
    if (requested_batch && !(requested_batch & HOP_XFER_PASTE_BATCH_FLAG)) return 0;
    if (!xfer_connected()) {
        hop_log(HOP_LOG_WARN, "xfer: data channel not connected, drop %d path(s)", n);
        return 0;
    }

    uint32_t batch = requested_batch;
    if (!batch) { do { batch = InterlockedIncrement((LONG *)&s_x.batch_seq); } while (!batch); }

    elist l;
    memset(&l, 0, sizeof(l));
    if (!hop_xfer_task_init(&l.task,offer_id,requested_batch,batch)) return 0;
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

    for (int i = 0; i < n; ++i) {
        if (!paths[i] || !paths[i][0]) { l.failed = 1; break; }
        const wchar_t *bn = base_name(paths[i]);
        char *root = w2u(bn);
        if (!root) { l.failed = 1; break; }
        walk_into(&l, paths[i], root);
        free(root);
        if (l.failed) break;
    }

    if (l.failed || l.n == 0) {
        if (l.budget_rejected)
            hop_log(HOP_LOG_WARN, "xfer: global entries/pending-bytes budget exceeded; batch rejected (task=%llu)",
                    (unsigned long long)l.task.id);
        else
            hop_log(HOP_LOG_WARN, "xfer: source tree rejected; no partial transfer queued (task=%llu)",
                    (unsigned long long)l.task.id);
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }

    xfer_job *job = (xfer_job *)calloc(1, sizeof(*job));
    if (!job) {
        hop_xfer_task_finish(&l.task, HOP_XFER_TASK_FAILED);
        elist_free(&l);
        return 0;
    }

    job->batch = batch;
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

static int xfer_submit_image_impl(const uint8_t *bgra, uint8_t *owned, int w, int h, int stride) {
    uint64_t min_stride,total64;
    if (!bgra || w <= 0 || h <= 0 || stride <= 0) { if (owned) free(owned); return 0; }
    min_stride=(uint64_t)(uint32_t)w*4u;
    if ((uint64_t)(uint32_t)stride<min_stride ||
        (uint64_t)(uint32_t)stride>UINT64_MAX/(uint64_t)(uint32_t)h) {
        if (owned) free(owned);
        return 0;
    }
    total64=(uint64_t)(uint32_t)stride*(uint64_t)(uint32_t)h;
    if(total64>(uint64_t)SIZE_MAX||!xfer_connected()){if(owned)free(owned);return 0;}

    uint32_t batch;
    do { batch = InterlockedIncrement((LONG *)&s_x.batch_seq); } while (!batch);
    hop_xfer_task task;
    memset(&task,0,sizeof(task));
    if(!hop_xfer_task_init(&task,0,0,batch)){if(owned)free(owned);return 0;}
    if(!hop_xfer_task_reserve_job(&task)||!hop_xfer_task_reserve_image(&task,total64)){
        uint64_t task_id=task.id;
        hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&task);
        if(owned)free(owned);
        hop_log(HOP_LOG_WARN,"xfer: global image memory budget exhausted (task=%llu)",
                (unsigned long long)task_id);
        return 0;
    }

    xfer_job *job=(xfer_job*)calloc(1,sizeof(*job));
    if(!job){if(owned)free(owned);hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);hop_xfer_task_dispose(&task);return 0;}
    size_t total=(size_t)total64;
    uint8_t *copy=owned ? owned : (uint8_t*)malloc(total);
    if(!copy){free(job);hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);hop_xfer_task_dispose(&task);return 0;}
    if(!owned)memcpy(copy,bgra,total);

    if(!hop_xfer_task_set_state(&task,HOP_XFER_TASK_QUEUED)||
       !hop_xfer_task_move(&job->task,&task)){
        free(copy);free(job);hop_xfer_task_finish(&task,HOP_XFER_TASK_FAILED);hop_xfer_task_dispose(&task);return 0;
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
    EnterCriticalSection(&s_x.lock);
    if (s_x.client != INVALID_SOCKET) shutdown(s_x.client, SD_BOTH);
    LeaveCriticalSection(&s_x.lock);
}
int xfer_listening(void) { return InterlockedCompareExchange(&s_x.listening, 0, 0) != 0; }

int xfer_submit_paths(const wchar_t *const *paths, int n) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, 0, 0);
    hop_update_work_leave();
    return result;
}

int xfer_submit_paths_for_paste(const wchar_t *const *paths, int n, uint32_t request_id, uint64_t offer_id) {
    if (!(request_id & HOP_XFER_PASTE_BATCH_FLAG) || !hop_update_work_enter()) return 0;
    int result = xfer_submit_paths_impl(paths, n, request_id, offer_id);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image(const uint8_t *bgra, int w, int h, int stride) {
    if (!hop_update_work_enter()) return 0;
    int result = xfer_submit_image_impl(bgra, NULL, w, h, stride);
    hop_update_work_leave();
    return result;
}

int xfer_submit_image_owned(uint8_t *bgra, int w, int h, int stride) {
    if (!bgra) return 0;
    if (!hop_update_work_enter()) { free(bgra); return 0; }
    int result = xfer_submit_image_impl(bgra, bgra, w, h, stride);
    hop_update_work_leave();
    return result;
}
