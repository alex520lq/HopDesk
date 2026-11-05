#include "xfer_budget.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_budget_lock=SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_budget_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_budget_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_budget_lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_budget_lock)
#define UNLOCK() pthread_mutex_unlock(&s_budget_lock)
#endif
#define DEFAULT_MAX_JOBS 16u
#define DEFAULT_MAX_ENTRIES 32768u
#define DEFAULT_MAX_PENDING_BYTES (8ull*1024ull*1024ull*1024ull)
#define DEFAULT_MAX_IMAGE_BYTES (256ull*1024ull*1024ull)
#define DEFAULT_MAX_SINGLE_IMAGE_BYTES (256ull*1024ull*1024ull)
static hop_xfer_budget_limits s_limits={DEFAULT_MAX_JOBS,DEFAULT_MAX_ENTRIES,DEFAULT_MAX_PENDING_BYTES,DEFAULT_MAX_IMAGE_BYTES,DEFAULT_MAX_SINGLE_IMAGE_BYTES};
static hop_xfer_budget_snapshot s_used;
static int add64(uint64_t a,uint64_t b,uint64_t*out){if(!out||b>UINT64_MAX-a)return 0;*out=a+b;return 1;}
void hop_xfer_budget_reservation_init(hop_xfer_budget_reservation*r){if(r)memset(r,0,sizeof(*r));}
int hop_xfer_budget_reserve_job(hop_xfer_budget_reservation*r){
 if(!r||r->active)return 0;LOCK();
 if(s_used.jobs>=s_limits.max_jobs||s_used.jobs==UINT32_MAX){s_used.rejected_jobs++;UNLOCK();return 0;}
 s_used.jobs++;if(s_used.jobs>s_used.jobs_high_water)s_used.jobs_high_water=s_used.jobs;
 r->jobs=1;r->active=1;UNLOCK();return 1;
}
int hop_xfer_budget_reserve_entry(hop_xfer_budget_reservation*r,uint64_t bytes){
 uint64_t next;if(!r||!r->active)return 0;LOCK();
 if(s_used.entries>=s_limits.max_entries||s_used.entries==UINT32_MAX){s_used.rejected_entries++;UNLOCK();return 0;}
 if(!add64(s_used.pending_bytes,bytes,&next)||next>s_limits.max_pending_bytes||bytes>UINT64_MAX-r->pending_bytes||r->entries==UINT32_MAX){s_used.rejected_bytes++;UNLOCK();return 0;}
 s_used.entries++;s_used.pending_bytes=next;
 if(s_used.entries>s_used.entries_high_water)s_used.entries_high_water=s_used.entries;
 if(next>s_used.pending_bytes_high_water)s_used.pending_bytes_high_water=next;
 r->entries++;r->pending_bytes+=bytes;UNLOCK();return 1;
}
int hop_xfer_budget_reserve_image(hop_xfer_budget_reservation*r,uint64_t bytes){
 uint64_t pending,image;if(!r||!r->active||!bytes)return 0;LOCK();
 if(bytes>s_limits.max_single_image_bytes||s_used.entries>=s_limits.max_entries||s_used.entries==UINT32_MAX){s_used.rejected_images++;UNLOCK();return 0;}
 if(!add64(s_used.pending_bytes,bytes,&pending)||pending>s_limits.max_pending_bytes||!add64(s_used.image_bytes,bytes,&image)||image>s_limits.max_image_bytes||bytes>UINT64_MAX-r->pending_bytes||bytes>UINT64_MAX-r->image_bytes||r->entries==UINT32_MAX){s_used.rejected_images++;UNLOCK();return 0;}
 s_used.entries++;s_used.pending_bytes=pending;s_used.image_bytes=image;
 if(s_used.entries>s_used.entries_high_water)s_used.entries_high_water=s_used.entries;
 if(pending>s_used.pending_bytes_high_water)s_used.pending_bytes_high_water=pending;
 if(image>s_used.image_bytes_high_water)s_used.image_bytes_high_water=image;
 r->entries++;r->pending_bytes+=bytes;r->image_bytes+=bytes;UNLOCK();return 1;
}
void hop_xfer_budget_release(hop_xfer_budget_reservation*r){
 if(!r||!r->active)return;LOCK();
 s_used.jobs=r->jobs>s_used.jobs?0:s_used.jobs-r->jobs;
 s_used.entries=r->entries>s_used.entries?0:s_used.entries-r->entries;
 s_used.pending_bytes=r->pending_bytes>s_used.pending_bytes?0:s_used.pending_bytes-r->pending_bytes;
 s_used.image_bytes=r->image_bytes>s_used.image_bytes?0:s_used.image_bytes-r->image_bytes;
 memset(r,0,sizeof(*r));UNLOCK();
}
void hop_xfer_budget_get_snapshot(hop_xfer_budget_snapshot*out){if(!out)return;LOCK();*out=s_used;UNLOCK();}
int hop_xfer_budget_set_limits_for_test(const hop_xfer_budget_limits*l){
 if(!l||!l->max_jobs||!l->max_entries||!l->max_pending_bytes||!l->max_image_bytes||!l->max_single_image_bytes||l->max_single_image_bytes>l->max_image_bytes)return 0;
 LOCK();if(s_used.jobs||s_used.entries||s_used.pending_bytes||s_used.image_bytes){UNLOCK();return 0;}s_limits=*l;memset(&s_used,0,sizeof(s_used));UNLOCK();return 1;
}
