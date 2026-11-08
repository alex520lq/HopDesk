/* =====================================================================
 *  HopDesk - 统一持久任务历史存储
 *
 *  设计约束：
 *    - Task Manager 仍是任务生命周期的唯一管理者；本模块只是终态
 *      快照的持久化观察者，通过 terminal sink 挂接，绝不反向影响
 *      任务结果或资源账本。
 *    - 磁盘格式自带 CRC32 与逐字段校验；损坏的文件直接弃用并保持
 *      原样（不删除），下一次成功追加时才会被覆盖。
 *    - 写入永远是“临时文件 + 全量 + fsync/_commit + rename”的原子
 *      替换，崩溃不会留下半截记录。
 *    - record_id 在文件内单调递增，跨重启从 max+1 继续；旧版
 *      transfer-history.bin 首次迁移时按最旧→最新分配 1..n。
 * ===================================================================== */
#include "xfer_task_history.h"
#include "hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
static SRWLOCK s_store_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_store_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_store_lock)
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_store_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_store_lock)
#define UNLOCK() pthread_mutex_unlock(&s_store_lock)
#endif

#define HOP_TASK_HISTORY_PATH_MAX 1024u

typedef struct {
    char magic[8];      /* "HPTSKHS2" */
    uint32_t version;   /* 2 */
    uint32_t record_size;
    uint32_t capacity;
    uint32_t count;
    uint32_t crc32;     /* over the count records that follow the header */
} hop_task_history_disk_header;

static hop_xfer_task_history_record s_records[HOP_TASK_HISTORY_CAPACITY];
static unsigned s_next, s_count;
static uint64_t s_next_record_id = 1;
static int s_open;
static char s_path[HOP_TASK_HISTORY_PATH_MAX];

/* ---------- 平台文件工具：路径按 UTF-8 传入，Windows 下转宽字符 ---------- */

#ifdef _WIN32
static size_t utf8_to_wide_buf(const char *src, wchar_t *dst, size_t cap) {
    if (!src || !dst || !cap) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n <= 0) return 0;
    return (size_t)n;
}
#endif

static FILE *open_path(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[HOP_TASK_HISTORY_PATH_MAX], wmode[8];
    if (!utf8_to_wide_buf(path, wpath, HOP_TASK_HISTORY_PATH_MAX)) return NULL;
    if (!utf8_to_wide_buf(mode, wmode, sizeof(wmode)/sizeof(wmode[0]))) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static int sync_file(FILE *f) {
    if (fflush(f) != 0) return 0;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    wchar_t wtmp[HOP_TASK_HISTORY_PATH_MAX], wdst[HOP_TASK_HISTORY_PATH_MAX];
    if (!utf8_to_wide_buf(tmp, wtmp, HOP_TASK_HISTORY_PATH_MAX) ||
        !utf8_to_wide_buf(dst, wdst, HOP_TASK_HISTORY_PATH_MAX)) return 0;
    return MoveFileExW(wtmp, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

static void remove_file(const char *path) {
#ifdef _WIN32
    wchar_t wpath[HOP_TASK_HISTORY_PATH_MAX];
    if (utf8_to_wide_buf(path, wpath, HOP_TASK_HISTORY_PATH_MAX)) DeleteFileW(wpath);
#else
    unlink(path);
#endif
}

/* ---------- 记录校验 ---------- */

static int record_valid(const hop_xfer_task_history_record *r) {
    if (!r->record_id || !r->finished_at) return 0;
    if (r->direction != HOP_XFER_DIR_SEND && r->direction != HOP_XFER_DIR_RECEIVE) return 0;
    switch (r->state) {
    case HOP_XFER_TASK_COMPLETED: case HOP_XFER_TASK_PARTIAL:
    case HOP_XFER_TASK_FAILED: case HOP_XFER_TASK_CANCELLED:
    case HOP_XFER_TASK_INTERRUPTED:
        break;
    default:
        return 0;
    }
    if (r->files_total && r->files_done > r->files_total) return 0;
    if (r->bytes_total && r->bytes_done > r->bytes_total) return 0;
    if (r->name[sizeof(r->name)-1] != 0 || r->detail[sizeof(r->detail)-1] != 0) return 0;
    return 1;
}

static void ring_store(hop_xfer_task_history_record *slot) {
    /* caller holds s_store_lock */
    s_records[s_next]=*slot;
    s_next=(s_next+1u)%HOP_TASK_HISTORY_CAPACITY;
    if (s_count<HOP_TASK_HISTORY_CAPACITY) ++s_count;
}
static void ring_append(const hop_xfer_task_history_record *rec) {
    /* caller holds s_store_lock; assigns the next durable record id */
    hop_xfer_task_history_record slot=*rec;
    slot.record_id=s_next_record_id++;
    ring_store(&slot);
}
static void ring_restore(hop_xfer_task_history_record *rec) {
    /* caller holds s_store_lock; keeps the persisted record id */
    if (rec->record_id>=s_next_record_id) s_next_record_id=rec->record_id+1;
    ring_store(rec);
}

/* ---------- 持久化 ---------- */

static int persist_locked(void) {
    /* caller holds s_store_lock; writes the whole ring oldest-first */
    unsigned count=s_count;
    hop_xfer_task_history_record rows[HOP_TASK_HISTORY_CAPACITY];
    for (unsigned i=0;i<count;++i) {
        unsigned oldest=(s_next+HOP_TASK_HISTORY_CAPACITY-count)%HOP_TASK_HISTORY_CAPACITY;
        rows[i]=s_records[(oldest+i)%HOP_TASK_HISTORY_CAPACITY];
    }
    uint32_t crc=hop_crc32(0,rows,(size_t)count*sizeof(rows[0]));
    hop_task_history_disk_header h;
    memset(&h,0,sizeof(h));
    memcpy(h.magic,"HPTSKHS2",8);
    h.version=2; h.record_size=(uint32_t)sizeof(rows[0]);
    h.capacity=HOP_TASK_HISTORY_CAPACITY; h.count=count; h.crc32=crc;

    char tmp[HOP_TASK_HISTORY_PATH_MAX+8];
    if ((size_t)snprintf(tmp,sizeof(tmp),"%s.tmp",s_path)>=sizeof(tmp)) return 0;
    FILE *f=open_path(tmp,"wb");
    if (!f) return 0;
    int ok=fwrite(&h,sizeof(h),1,f)==1 &&
           fwrite(rows,sizeof(rows[0]),count,f)==count &&
           sync_file(f);
    if (fclose(f)!=0) ok=0;
    if (ok) ok=replace_file(tmp,s_path);
    if (!ok) remove_file(tmp);
    return ok;
}

/* ---------- 加载与迁移 ---------- */

static void load_store_file(void) {
    /* caller holds s_store_lock */
    FILE *f=open_path(s_path,"rb");
    if (!f) return;
    hop_task_history_disk_header h;
    if (fread(&h,sizeof(h),1,f)!=1 || memcmp(h.magic,"HPTSKHS2",8)!=0 ||
        h.version!=2 || h.record_size!=(uint32_t)sizeof(hop_xfer_task_history_record) ||
        h.capacity!=HOP_TASK_HISTORY_CAPACITY || h.count>HOP_TASK_HISTORY_CAPACITY) {
        fclose(f); return;
    }
    hop_xfer_task_history_record rows[HOP_TASK_HISTORY_CAPACITY];
    memset(rows,0,sizeof(rows));
    if (h.count && fread(rows,sizeof(rows[0]),h.count,f)!=h.count) {
        fclose(f); return;
    }
    uint32_t crc=hop_crc32(0,rows,(size_t)h.count*sizeof(rows[0]));
    if (crc!=h.crc32) { fclose(f); return; }
    fclose(f);
    for (uint32_t i=0;i<h.count;++i) {
        if (!record_valid(&rows[i])) return; /* corrupt: keep disk, start empty */
        ring_restore(&rows[i]);
    }
}

static int legacy_phase_to_state(int phase, int *state) {
    switch (phase) {
    case HOP_XFER_COMPLETED: *state=HOP_XFER_TASK_COMPLETED; return 1;
    case HOP_XFER_PARTIAL: *state=HOP_XFER_TASK_PARTIAL; return 1;
    case HOP_XFER_FAILED: *state=HOP_XFER_TASK_FAILED; return 1;
    case HOP_XFER_CANCELLED: *state=HOP_XFER_TASK_CANCELLED; return 1;
    default: return 0;
    }
}

static void migrate_legacy_locked(const char *legacy_path) {
    /* caller holds s_store_lock; the legacy file is newest-first on disk */
    if (!legacy_path || !*legacy_path) return;
    FILE *f=open_path(legacy_path,"rb");
    if (!f) return;
    char magic[8]; uint32_t version, record_size, count;
    if (fread(magic,8,1,f)!=1 || memcmp(magic,"HOPXHST1",8)!=0 ||
        fread(&version,sizeof(version),1,f)!=1 || version!=1 ||
        fread(&record_size,sizeof(record_size),1,f)!=1 ||
        record_size!=(uint32_t)sizeof(hop_xfer_history_entry) ||
        fread(&count,sizeof(count),1,f)!=1 || count>HOP_XFER_HISTORY_LIMIT) {
        fclose(f); return;
    }
    hop_xfer_history_entry rows[HOP_XFER_HISTORY_LIMIT];
    memset(rows,0,sizeof(rows));
    if (count && fread(rows,sizeof(rows[0]),count,f)!=count) { fclose(f); return; }
    fclose(f);
    for (uint32_t i=0;i<count;++i) {
        rows[i].name[sizeof(rows[i].name)-1]=0;
        rows[i].detail[sizeof(rows[i].detail)-1]=0;
    }
    /* rows[0] is the newest; import oldest-first so record ids ascend with time */
    for (int idx=(int)count-1; idx>=0; --idx) {
        const hop_xfer_history_entry *src=&rows[idx];
        int state=0;
        if (!legacy_phase_to_state(src->phase,&state)) continue;
        if (src->direction!=HOP_XFER_DIR_SEND && src->direction!=HOP_XFER_DIR_RECEIVE) continue;
        hop_xfer_task_history_record rec;
        memset(&rec,0,sizeof(rec));
        rec.task_id=0; rec.request_id=0; rec.transfer_id=src->batch;
        rec.direction=src->direction; rec.state=state;
        rec.files_done=src->files_done; rec.files_total=src->files_total;
        rec.bytes_done=src->bytes_done; rec.bytes_total=src->bytes_total;
        rec.finished_at=src->finished_at ? src->finished_at*1000ull : 1ull;
        memcpy(rec.name,src->name,sizeof(rec.name));
        memcpy(rec.detail,src->detail,sizeof(rec.detail));
        rec.name[sizeof(rec.name)-1]=0; rec.detail[sizeof(rec.detail)-1]=0;
        ring_append(&rec);
    }
    /* Persist immediately so the legacy file is never imported twice, even if
     * nothing terminates during this run. */
    persist_locked();
}

/* ---------- sink ---------- */

static void on_terminal_snapshot(const hop_xfer_task_snapshot *snap) {
    hop_xfer_task_history_record rec;
    memset(&rec,0,sizeof(rec));
    rec.task_id=snap->id; rec.request_id=snap->request_id; rec.transfer_id=snap->transfer_id;
    rec.direction=snap->direction; rec.state=snap->state;
    rec.files_done=snap->files_done; rec.files_total=snap->files_total;
    rec.bytes_done=snap->bytes_done; rec.bytes_total=snap->bytes_total;
    rec.finished_at=snap->finished_at;
    memcpy(rec.name,snap->name,sizeof(rec.name));
    memcpy(rec.detail,snap->detail,sizeof(rec.detail));
    LOCK();
    if (s_open) {
        ring_append(&rec);
        persist_locked();
    }
    UNLOCK();
}

/* ---------- 公开 API ---------- */

int hop_xfer_task_history_store_open(const char *path, const char *legacy_path) {
    if (!path || !*path) return 0;
    hop_crc32_init();
    LOCK();
    if (s_open) { UNLOCK(); return 1; }
    if (strlen(path)>=sizeof(s_path)) { UNLOCK(); return 0; }
    memcpy(s_path,path,strlen(path)+1);
    s_next=0; s_count=0; s_next_record_id=1;
    load_store_file();
    if (!s_count) migrate_legacy_locked(legacy_path);
    s_open=1;
    UNLOCK();
    hop_xfer_task_set_terminal_sink(on_terminal_snapshot);
    return 1;
}

int hop_xfer_task_history_store_is_open(void) {
    LOCK(); int open=s_open; UNLOCK(); return open;
}

int hop_xfer_task_history_store_list(hop_xfer_task_history_record *out, int capacity,
                                     int direction, int state, int newest_first, int *total) {
    if (capacity<0 || (capacity && !out) || (newest_first!=0 && newest_first!=1) ||
        (direction!=HOP_XFER_DIR_NONE && direction!=HOP_XFER_DIR_SEND && direction!=HOP_XFER_DIR_RECEIVE) ||
        (state!=HOP_XFER_TASK_EMPTY && (state<HOP_XFER_TASK_COMPLETED || state>HOP_XFER_TASK_INTERRUPTED)))
        return -1;
    int n=0, all=0;
    LOCK();
    for (unsigned i=0;i<s_count;++i) {
        unsigned oldest=(s_next+HOP_TASK_HISTORY_CAPACITY-s_count)%HOP_TASK_HISTORY_CAPACITY;
        unsigned offset=newest_first ? s_count-1-i : i;
        const hop_xfer_task_history_record *rec=&s_records[(oldest+offset)%HOP_TASK_HISTORY_CAPACITY];
        if (direction!=HOP_XFER_DIR_NONE && rec->direction!=direction) continue;
        if (state!=HOP_XFER_TASK_EMPTY && rec->state!=state) continue;
        if (n<capacity) out[n++]=*rec;
        ++all;
    }
    UNLOCK();
    if (total) *total=all;
    return n;
}

void hop_xfer_task_history_store_close(void) {
    hop_xfer_task_set_terminal_sink(NULL);
    LOCK();
    s_open=0; s_next=0; s_count=0; s_next_record_id=1; s_path[0]=0;
    UNLOCK();
}