#include "xfer_task.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_manager_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_manager_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_manager_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_manager_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_manager_lock)
#define UNLOCK() pthread_mutex_unlock(&s_manager_lock)
#endif

/* Manager lock always precedes the budget lock. No budget operation calls back
 * into this manager. A record owns exactly one reservation and one handle. */
#define HOP_TASK_MANAGER_MAX_LIVE 4096u
typedef struct task_record {
    struct task_record *next;
    hop_xfer_task *owner;
    uint64_t id, offer_id;
    uint32_t request_id, transfer_id;
    hop_xfer_task_state state;
    hop_xfer_reservation_handle reservation;
} task_record;
static task_record *s_tasks;
static uint64_t s_next_task_id;
static unsigned s_task_count;

static int terminal(hop_xfer_task_state s) {
    return s == HOP_XFER_TASK_COMPLETED || s == HOP_XFER_TASK_PARTIAL ||
           s == HOP_XFER_TASK_FAILED || s == HOP_XFER_TASK_CANCELLED ||
           s == HOP_XFER_TASK_INTERRUPTED;
}
static task_record *find_id(uint64_t id) {
    for (task_record *r=s_tasks; r; r=r->next) if (r->id == id) return r;
    return NULL;
}
static task_record *owned(const hop_xfer_task *t) {
    if (!t || !t->initialized || !t->id) return NULL;
    task_record *r=find_id(t->id);
    return r && r->owner==t ? r : NULL;
}
static void snapshot(const task_record *r, hop_xfer_task_snapshot *out) {
    memset(out,0,sizeof(*out));
    out->id=r->id; out->offer_id=r->offer_id; out->request_id=r->request_id;
    out->transfer_id=r->transfer_id; out->state=r->state;
    out->reservation_handle_id=r->reservation.handle_id;
    out->jobs=r->reservation.jobs; out->entries=r->reservation.entries;
    out->pending_bytes=r->reservation.pending_bytes; out->image_bytes=r->reservation.image_bytes;
    out->rx_queue_slots=r->reservation.rx_queue_slots; out->rx_queue_bytes=r->reservation.rx_queue_bytes;
    out->image_temp_bytes=r->reservation.image_temp_bytes;
}
int hop_xfer_task_get_snapshot(hop_xfer_task_id id, hop_xfer_task_snapshot *out) {
    if (!id || !out) return 0;
    LOCK(); task_record *r=find_id(id); int found=r != NULL;
    if (r) snapshot(r,out);
    UNLOCK(); return found;
}
int hop_xfer_task_list(hop_xfer_task_snapshot *out, int capacity, int *total) {
    if (capacity < 0 || (capacity && !out)) return -1;
    int n=0, all=0;
    LOCK();
    for (task_record *r=s_tasks; r; r=r->next) {
        if (n<capacity) snapshot(r,&out[n++]);
        ++all;
    }
    if (total) *total=all;
    UNLOCK(); return n;
}
uint64_t hop_xfer_task_image_temp_bytes(const hop_xfer_task *task) {
    LOCK(); task_record *r=owned(task); uint64_t n=r?r->reservation.image_temp_bytes:0; UNLOCK(); return n;
}
int hop_xfer_task_is_terminal(const hop_xfer_task *task) {
    LOCK(); task_record *r=owned(task); int result=!r || terminal(r->state); UNLOCK(); return result;
}
int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer, uint32_t request, uint32_t transfer) {
    if (!task || !transfer) return 0;
    task_record *r=(task_record *)calloc(1,sizeof(*r)); if (!r) return 0;
    LOCK();
    if (task->initialized || task->id || s_task_count >= HOP_TASK_MANAGER_MAX_LIVE || s_next_task_id==UINT64_MAX) {
        UNLOCK(); free(r); return 0;
    }
    r->id=++s_next_task_id;
    if (!hop_xfer_budget_reservation_init(&r->reservation,r->id)) { UNLOCK(); free(r); return 0; }
    r->owner=task; r->offer_id=offer; r->request_id=request; r->transfer_id=transfer;
    r->state=HOP_XFER_TASK_CREATED; r->next=s_tasks; s_tasks=r; ++s_task_count;
    task->id=r->id; task->initialized=1;
    UNLOCK(); return 1;
}
int hop_xfer_task_move(hop_xfer_task *dst, hop_xfer_task *src) {
    if (!dst || !src || dst==src) return 0;
    LOCK(); task_record *r=owned(src);
    if (!r || dst->initialized || dst->id) { UNLOCK(); return 0; }
    *dst=*src; memset(src,0,sizeof(*src)); r->owner=dst;
    UNLOCK(); return 1;
}
int hop_xfer_task_set_state(hop_xfer_task *task, hop_xfer_task_state state) {
    LOCK(); task_record *r=owned(task); int ok=0;
    if (r && !terminal(r->state)) {
        ok=(r->state==HOP_XFER_TASK_CREATED && (state==HOP_XFER_TASK_SCANNING || state==HOP_XFER_TASK_QUEUED || state==HOP_XFER_TASK_ACTIVE)) ||
           (r->state==HOP_XFER_TASK_SCANNING && state==HOP_XFER_TASK_QUEUED) ||
           (r->state==HOP_XFER_TASK_QUEUED && state==HOP_XFER_TASK_ACTIVE) ||
           (r->state==HOP_XFER_TASK_ACTIVE && state==HOP_XFER_TASK_QUEUED);
        if (ok) r->state=state;
    }
    UNLOCK(); return ok;
}
#define RESERVE_WRAPPER(name,fn) \
int name(hop_xfer_task *task) { LOCK(); task_record *r=owned(task); int ok=r && !terminal(r->state) && fn(&r->reservation); UNLOCK(); return ok; }
#define BYTES_WRAPPER(name,fn,live) \
int name(hop_xfer_task *task, uint64_t bytes) { LOCK(); task_record *r=owned(task); int ok=r && (!(live) || !terminal(r->state)) && fn(&r->reservation,bytes); UNLOCK(); return ok; }
RESERVE_WRAPPER(hop_xfer_task_reserve_job,hop_xfer_budget_reserve_job)
BYTES_WRAPPER(hop_xfer_task_reserve_entry,hop_xfer_budget_reserve_entry,1)
BYTES_WRAPPER(hop_xfer_task_reserve_image,hop_xfer_budget_reserve_image,1)
BYTES_WRAPPER(hop_xfer_task_reserve_rx_buffer,hop_xfer_budget_reserve_rx_buffer,1)
BYTES_WRAPPER(hop_xfer_task_release_rx_buffer,hop_xfer_budget_release_rx_buffer,0)
BYTES_WRAPPER(hop_xfer_task_reserve_image_temp,hop_xfer_budget_reserve_image_temp,1)
BYTES_WRAPPER(hop_xfer_task_release_image_temp,hop_xfer_budget_release_image_temp,0)
int hop_xfer_task_transfer_image_temp(hop_xfer_task *from, hop_xfer_task *to, uint64_t bytes) {
    LOCK(); task_record *a=owned(from), *b=owned(to);
    int ok=a && b && a!=b && !terminal(a->state) && !terminal(b->state) &&
        hop_xfer_budget_transfer_image_temp(&a->reservation,&b->reservation,bytes);
    UNLOCK(); return ok;
}
static int finish(task_record *r, hop_xfer_task_state state) {
    if (!r || !terminal(state) || terminal(r->state) || r->reservation.rx_queue_slots ||
        r->reservation.rx_queue_bytes || r->reservation.image_temp_bytes) return 0;
    hop_xfer_budget_release(&r->reservation);
    r->state=state; return 1;
}
int hop_xfer_task_finish(hop_xfer_task *task, hop_xfer_task_state state) {
    LOCK(); int ok=finish(owned(task),state); UNLOCK(); return ok;
}
void hop_xfer_task_dispose(hop_xfer_task *task) {
    LOCK(); task_record *r=owned(task);
    if (!r || (!terminal(r->state) && !finish(r,HOP_XFER_TASK_INTERRUPTED))) { UNLOCK(); return; }
    task_record **p=&s_tasks; while (*p && *p!=r) p=&(*p)->next;
    if (*p) { *p=r->next; --s_task_count; memset(task,0,sizeof(*task)); free(r); }
    UNLOCK();
}
