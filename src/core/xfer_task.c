#include "xfer_task.h"
#include "hop.h"
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
#define HOP_TASK_INDEX_BUCKETS 8192u
typedef struct task_record {
    struct task_record *next, *hash_next;
    hop_xfer_task *owner;
    uint64_t id, offer_id;
    uint32_t request_id, transfer_id;
    int direction;
    hop_xfer_task_kind kind;
    uint8_t session[HOP_TASK_SESSION_BYTES];
    hop_xfer_task_state state;
    int cancel_requested, completing, rx_disconnected;
    uint64_t rx_disconnect_ms;
    /* User-facing summary, display only; finished_at is wall-clock ms. */
    uint32_t files_done, files_total;
    uint64_t bytes_done, bytes_total, finished_at;
    char name[HOP_XFER_STATUS_NAME];
    char detail[HOP_XFER_STATUS_DETAIL];
    hop_xfer_reservation_handle reservation;
} task_record;
static task_record *s_tasks;
static task_record *s_task_index[HOP_TASK_INDEX_BUCKETS];
static uint64_t s_next_task_id;
static unsigned s_task_count;
/* Descriptive snapshots only: never own handles, budget, or lifecycle state.
 * The manager appends exactly once at the terminal transition under its lock. */
static hop_xfer_task_snapshot s_history[HOP_TASK_HISTORY_CAPACITY];
static unsigned s_history_next, s_history_count;
/* Notified once per user-facing terminal task after the manager lock is
 * released; the manager never calls it while holding any lock. */
static hop_xfer_task_terminal_sink s_terminal_sink;

static int terminal(hop_xfer_task_state s) {
    return s == HOP_XFER_TASK_COMPLETED || s == HOP_XFER_TASK_PARTIAL ||
           s == HOP_XFER_TASK_FAILED || s == HOP_XFER_TASK_CANCELLED ||
           s == HOP_XFER_TASK_INTERRUPTED;
}
static task_record *find_id(uint64_t id) {
    for (task_record *r=s_task_index[id % HOP_TASK_INDEX_BUCKETS]; r; r=r->hash_next)
        if (r->id == id) return r;
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
    out->cancel_requested=r->cancel_requested; out->completing=r->completing;
    out->direction=r->direction; out->kind=r->kind;
    memcpy(out->session,r->session,sizeof(out->session));
    out->reservation_handle_id=r->reservation.handle_id;
    out->jobs=r->reservation.jobs; out->entries=r->reservation.entries;
    out->pending_bytes=r->reservation.pending_bytes; out->image_bytes=r->reservation.image_bytes;
    out->rx_queue_slots=r->reservation.rx_queue_slots; out->rx_queue_bytes=r->reservation.rx_queue_bytes;
    out->image_temp_bytes=r->reservation.image_temp_bytes;
    out->files_done=r->files_done; out->files_total=r->files_total;
    out->bytes_done=r->bytes_done; out->bytes_total=r->bytes_total;
    out->finished_at=r->finished_at;
    memcpy(out->name,r->name,sizeof(out->name));
    memcpy(out->detail,r->detail,sizeof(out->detail));
}
int hop_xfer_task_request_cancel(hop_xfer_task_id id) {
    if (!id) return 0;
    LOCK(); task_record *r=find_id(id);
    int ok=r && !terminal(r->state) && !r->completing &&
        (r->direction==HOP_XFER_DIR_SEND || r->direction==HOP_XFER_DIR_RECEIVE);
    if (ok) r->cancel_requested=1;
    UNLOCK(); return ok;
}
int hop_xfer_task_cancel_requested(hop_xfer_task_id id) {
    if (!id) return 0;
    LOCK(); task_record *r=find_id(id); int requested=r && r->cancel_requested;
    UNLOCK(); return requested;
}
int hop_xfer_task_begin_completion(const hop_xfer_task *task) {
    LOCK(); task_record *r=owned(task);
    int ok=r && !terminal(r->state) && !r->cancel_requested && !r->completing;
    if (ok) r->completing=1;
    UNLOCK(); return ok;
}
int hop_xfer_task_rx_disconnect_expired(const hop_xfer_task *task, int connected,
                                        uint64_t now_ms, uint64_t budget_ms) {
    if (!budget_ms) return 0;
    LOCK(); task_record *r=owned(task); int expired=0;
    if (r && r->direction==HOP_XFER_DIR_RECEIVE && r->state==HOP_XFER_TASK_ACTIVE && !r->completing && !r->cancel_requested) {
        if (connected) { r->rx_disconnected=0; r->rx_disconnect_ms=0; }
        else if (!r->rx_disconnected || now_ms<r->rx_disconnect_ms) {
            r->rx_disconnected=1; r->rx_disconnect_ms=now_ms;
        } else if (now_ms-r->rx_disconnect_ms>=budget_ms) {
            /* Claim terminal processing under the same lock as cancel. */
            r->completing=1; expired=1;
        }
    }
    UNLOCK(); return expired;
}
static void audit_add(uint64_t *total, uint64_t value, uint32_t *mismatches) {
    if (value > UINT64_MAX - *total) { *mismatches |= HOP_TASK_AUDIT_OVERFLOW; *total=UINT64_MAX; }
    else *total += value;
}
int hop_xfer_task_reconcile(hop_xfer_task_audit *out) {
    if (!out) return 0;
    memset(out,0,sizeof(*out));
    LOCK();
    for (task_record *r=s_tasks; r; r=r->next) {
        hop_xfer_reservation_handle *h=&r->reservation;
        ++out->task_count;
        if (h->task_id!=r->id || !h->handle_id || !r->owner || (h->active ? h->jobs!=1 : h->jobs!=0))
            out->mismatches |= HOP_TASK_AUDIT_OWNER;
        if (terminal(r->state) && (h->active || h->jobs || h->entries || h->pending_bytes ||
            h->image_bytes || h->image_temp_bytes || h->rx_queue_slots || h->rx_queue_bytes))
            out->mismatches |= HOP_TASK_AUDIT_TERMINAL;
        audit_add(&out->jobs,h->jobs,&out->mismatches);
        audit_add(&out->entries,h->entries,&out->mismatches);
        audit_add(&out->pending_bytes,h->pending_bytes,&out->mismatches);
        audit_add(&out->image_bytes,h->image_bytes,&out->mismatches);
        audit_add(&out->image_temp_bytes,h->image_temp_bytes,&out->mismatches);
        audit_add(&out->rx_queue_slots,h->rx_queue_slots,&out->mismatches);
        audit_add(&out->rx_queue_bytes,h->rx_queue_bytes,&out->mismatches);
    }
    hop_xfer_budget_get_snapshot(&out->ledger);
    if (out->jobs!=out->ledger.jobs || out->entries!=out->ledger.entries ||
        out->pending_bytes!=out->ledger.pending_bytes || out->image_bytes!=out->ledger.image_bytes ||
        out->image_temp_bytes!=out->ledger.image_temp_bytes || out->rx_queue_slots!=out->ledger.rx_queue_slots ||
        out->rx_queue_bytes!=out->ledger.rx_queue_bytes) out->mismatches |= HOP_TASK_AUDIT_USAGE;
    UNLOCK(); return 1;
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
int hop_xfer_task_history_list(hop_xfer_task_snapshot *out, int capacity,
                               int direction, hop_xfer_task_state state,
                               int newest_first, int *total) {
    if (capacity<0 || (capacity && !out) || (newest_first!=0 && newest_first!=1) ||
        (direction!=HOP_XFER_DIR_NONE && direction!=HOP_XFER_DIR_SEND && direction!=HOP_XFER_DIR_RECEIVE) ||
        (state!=HOP_XFER_TASK_EMPTY && !terminal(state))) return -1;
    int n=0, all=0;
    LOCK();
    for (unsigned i=0;i<s_history_count;++i) {
        unsigned oldest=(s_history_next+HOP_TASK_HISTORY_CAPACITY-s_history_count)%HOP_TASK_HISTORY_CAPACITY;
        unsigned offset=newest_first?s_history_count-1-i:i;
        const hop_xfer_task_snapshot *entry=&s_history[(oldest+offset)%HOP_TASK_HISTORY_CAPACITY];
        if (direction!=HOP_XFER_DIR_NONE && entry->direction!=direction) continue;
        if (state!=HOP_XFER_TASK_EMPTY && entry->state!=state) continue;
        if (n<capacity) out[n++]=*entry;
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
int hop_xfer_task_init_classified(hop_xfer_task *task, uint64_t offer, uint32_t request,
                                  uint32_t transfer, int direction, hop_xfer_task_kind kind) {
    if (!task || !transfer || direction < HOP_XFER_DIR_NONE || direction > HOP_XFER_DIR_RECEIVE ||
        kind < HOP_TASK_KIND_UNSPECIFIED || kind > HOP_TASK_KIND_MIXED) return 0;
    task_record *r=(task_record *)calloc(1,sizeof(*r)); if (!r) return 0;
    LOCK();
    if (task->initialized || task->id || s_task_count >= HOP_TASK_MANAGER_MAX_LIVE || s_next_task_id==UINT64_MAX) {
        UNLOCK(); free(r); return 0;
    }
    r->id=++s_next_task_id;
    if (!hop_xfer_budget_reservation_init(&r->reservation,r->id)) { UNLOCK(); free(r); return 0; }
    r->owner=task; r->offer_id=offer; r->request_id=request; r->transfer_id=transfer;
    r->direction=direction; r->kind=kind;
    r->state=HOP_XFER_TASK_CREATED; r->next=s_tasks; s_tasks=r;
    unsigned bucket=(unsigned)(r->id % HOP_TASK_INDEX_BUCKETS);
    r->hash_next=s_task_index[bucket]; s_task_index[bucket]=r; ++s_task_count;
    task->id=r->id; task->initialized=1;
    UNLOCK(); return 1;
}
int hop_xfer_task_init(hop_xfer_task *task, uint64_t offer, uint32_t request, uint32_t transfer) {
    return hop_xfer_task_init_classified(task,offer,request,transfer,HOP_XFER_DIR_NONE,HOP_TASK_KIND_UNSPECIFIED);
}
int hop_xfer_task_bind_session(const hop_xfer_task *task, const uint8_t session[HOP_TASK_SESSION_BYTES]) {
    if (!session) return 0;
    int valid=0; for (unsigned i=0; i<HOP_TASK_SESSION_BYTES; ++i) valid |= session[i];
    if (!valid) return 0;
    LOCK(); task_record *r=owned(task);
    int ok=r && !terminal(r->state);
    if (ok) memcpy(r->session,session,HOP_TASK_SESSION_BYTES);
    UNLOCK(); return ok;
}
int hop_xfer_task_set_kind(const hop_xfer_task *task, hop_xfer_task_kind kind) {
    if (kind < HOP_TASK_KIND_FILES || kind > HOP_TASK_KIND_IMAGE_WORK) return 0;
    LOCK(); task_record *r=owned(task);
    int ok=r && !terminal(r->state);
    if (ok) {
        if (r->kind==HOP_TASK_KIND_UNSPECIFIED) r->kind=kind;
        else if (r->kind!=kind) {
            if ((r->kind==HOP_TASK_KIND_FILES || r->kind==HOP_TASK_KIND_IMAGE || r->kind==HOP_TASK_KIND_MIXED) &&
                (kind==HOP_TASK_KIND_FILES || kind==HOP_TASK_KIND_IMAGE)) r->kind=HOP_TASK_KIND_MIXED;
            else ok=0;
        }
    }
    UNLOCK(); return ok;
}
static void copy_summary_text(char *dst, const char *src, size_t cap) {
    if (!src) { dst[0]=0; return; }
    size_t n=strlen(src); if (n>=cap) n=cap-1;
    memcpy(dst,src,n); dst[n]=0;
}
int hop_xfer_task_set_summary(const hop_xfer_task *task, uint32_t files_done,
                              uint32_t files_total, uint64_t bytes_done,
                              uint64_t bytes_total, const char *name,
                              const char *detail) {
    LOCK(); task_record *r=owned(task);
    int ok=r && !terminal(r->state);
    if (ok) {
        r->files_done=files_done; r->files_total=files_total;
        r->bytes_done=bytes_done; r->bytes_total=bytes_total;
        copy_summary_text(r->name,name,sizeof(r->name));
        copy_summary_text(r->detail,detail,sizeof(r->detail));
    }
    UNLOCK(); return ok;
}
int hop_xfer_task_set_terminal_sink(hop_xfer_task_terminal_sink sink) {
    /* NULL clears the registration (used when the durable store shuts down). */
    LOCK(); s_terminal_sink=sink; UNLOCK(); return 1;
}
hop_xfer_task_terminal_sink hop_xfer_task_get_terminal_sink(void) {
    LOCK(); hop_xfer_task_terminal_sink sink=s_terminal_sink; UNLOCK(); return sink;
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
static int finish(task_record *r, hop_xfer_task_state state,
                  hop_xfer_task_snapshot *fired) {
    if (!r || !terminal(state) || terminal(r->state) || r->reservation.rx_queue_slots ||
        r->reservation.rx_queue_bytes || r->reservation.image_temp_bytes) return 0;
    hop_xfer_budget_release(&r->reservation);
    r->state=state;
    if (r->direction==HOP_XFER_DIR_SEND || r->direction==HOP_XFER_DIR_RECEIVE) {
        r->finished_at=hop_wallclock_ms();
        snapshot(r,&s_history[s_history_next]);
        s_history_next=(s_history_next+1u)%HOP_TASK_HISTORY_CAPACITY;
        if (s_history_count<HOP_TASK_HISTORY_CAPACITY) ++s_history_count;
        if (fired) snapshot(r,fired);
    }
    return 1;
}
int hop_xfer_task_finish(hop_xfer_task *task, hop_xfer_task_state state) {
    hop_xfer_task_snapshot fired;
    memset(&fired,0,sizeof(fired));
    LOCK();
    int ok=finish(owned(task),state,&fired);
    hop_xfer_task_terminal_sink sink=s_terminal_sink;
    UNLOCK();
    if (ok && sink && fired.id) sink(&fired);
    return ok;
}
void hop_xfer_task_dispose(hop_xfer_task *task) {
    hop_xfer_task_snapshot fired;
    memset(&fired,0,sizeof(fired));
    LOCK();
    task_record *r=owned(task);
    if (!r || (!terminal(r->state) && !finish(r,HOP_XFER_TASK_INTERRUPTED,&fired))) { UNLOCK(); return; }
    task_record **p=&s_tasks; while (*p && *p!=r) p=&(*p)->next;
    if (*p) {
        task_record **h=&s_task_index[r->id % HOP_TASK_INDEX_BUCKETS];
        while (*h && *h!=r) h=&(*h)->hash_next;
        if (!*h) { UNLOCK(); return; } /* never free a record still in the index */
        *h=r->hash_next; *p=r->next; --s_task_count; memset(task,0,sizeof(*task)); free(r);
    }
    hop_xfer_task_terminal_sink sink=s_terminal_sink;
    UNLOCK();
    if (sink && fired.id) sink(&fired);
}
