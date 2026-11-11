#include "xfer_sender_job_lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {hop_xfer_sender_job_owner owner;int deferred,freed,mark,status;} fake;
static uint64_t temp_bytes;static int terminal,fail_release,cancelled;
uint64_t hop_xfer_task_image_temp_bytes(const hop_xfer_task*t){return t&&t->initialized?temp_bytes:0;}
int hop_xfer_task_release_image_temp(hop_xfer_task*t,uint64_t n){if(!t||!t->initialized||fail_release||n>temp_bytes)return 0;temp_bytes-=n;return 1;}
int hop_xfer_task_is_terminal(const hop_xfer_task*t){return t&&t->initialized&&terminal;}
int hop_xfer_task_finish(hop_xfer_task*t,hop_xfer_task_state s){(void)s;if(!t||!t->initialized)return 0;terminal=1;return 1;}
void hop_xfer_task_dispose(hop_xfer_task*t){if(t&&t->initialized&&terminal)memset(t,0,sizeof(*t));}
static int is_cancel(void*u,uint32_t b,uint64_t id){(void)u;(void)b;(void)id;return cancelled;}
static int no_cancel(void*u,uint32_t b){(void)u;(void)b;return 0;}
static void finish(void*u,hop_xfer_task*t,int s){(void)u;(void)hop_xfer_task_finish(t,(hop_xfer_task_state)s);}
static void dispose(void*u,hop_xfer_task*t){(void)u;hop_xfer_task_dispose(t);}
static void status(void*u,uint32_t b,int p,const char*d){(void)b;(void)p;(void)d;((fake*)u)->status++;}
static void logmsg(void*u,int l,const char*m){(void)u;(void)l;(void)m;}
static int claim(void*u,void*j,hop_xfer_sender_job_owner e,hop_xfer_sender_job_owner*a){
 fake*f=j;(void)u;*a=f->owner;if(f->owner!=e)return 0;f->owner=HOP_XFER_SENDER_JOB_OWNER_RELEASING;return 1;}
static void defer_job(void*u,void*j,hop_xfer_sender_job_owner e){fake*f=j;(void)u;(void)e;if(f->owner==HOP_XFER_SENDER_JOB_OWNER_RELEASING){f->owner=HOP_XFER_SENDER_JOB_OWNER_SHUTDOWN;f->deferred++;}}
static int mark(void*u,void*j){fake*f=j;(void)u;if(f->owner!=HOP_XFER_SENDER_JOB_OWNER_RELEASING)return 0;f->owner=HOP_XFER_SENDER_JOB_OWNER_RELEASED;return 1;}
static void free_job(void*u,void*j){(void)u;((fake*)j)->freed++;}
static const hop_xfer_sender_job_release_ops ops={NULL,claim,defer_job,mark,free_job};
static void setup(hop_xfer_sender_job_release_view*v,fake*f,hop_xfer_sender_context*c,
 hop_xfer_task*t,uint8_t**px,int*rel,hop_xfer_task_state*ps,int*pv,int*se,int*sp,char*d){
 memset(v,0,sizeof(*v));memset(c,0,sizeof(*c));memset(t,0,sizeof(*t));memset(f,0,sizeof(*f));
 f->owner=HOP_XFER_SENDER_JOB_OWNER_WORKER;t->id=99;t->initialized=1;
 c->user=f;c->is_cancelled=is_cancel;c->is_batch_cancelled=no_cancel;c->task_finish=finish;
 c->task_dispose=dispose;c->status_end=status;c->log_message=logmsg;
 *px=malloc(8);assert(*px);*rel=0;*ps=HOP_XFER_TASK_FAILED;*pv=0;*se=1;*sp=7;strcpy(d,"ended");
 v->job=f;v->context=c;v->task=t;v->batch=3;v->is_image=1;v->pixels=px;v->image_payload_released=rel;
 v->pending_finish_state=ps;v->pending_finish_valid=pv;v->pending_status_end=se;v->pending_status_phase=sp;v->pending_status_detail=d;
}
int main(void){
 hop_xfer_sender_job_release_view v;hop_xfer_sender_context c;hop_xfer_task t;fake f;
 uint8_t*px;int rel,pv,se,sp;hop_xfer_task_state ps;char d[96];
 temp_bytes=42;terminal=fail_release=cancelled=0;
 setup(&v,&f,&c,&t,&px,&rel,&ps,&pv,&se,&sp,d);
 assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_WORKER)==HOP_XFER_SENDER_JOB_RELEASED);
 assert(!px&&rel&&!temp_bytes&&!t.id&&f.status==1&&f.freed==1&&f.owner==HOP_XFER_SENDER_JOB_OWNER_RELEASED);
 temp_bytes=42;terminal=fail_release=cancelled=0;
 setup(&v,&f,&c,&t,&px,&rel,&ps,&pv,&se,&sp,d);free(px);px=NULL;
 assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_WORKER)==HOP_XFER_SENDER_JOB_RELEASE_DEFERRED);
 assert(t.initialized&&temp_bytes==42&&f.deferred==1&&f.freed==0);
 rel=1;assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_SHUTDOWN)==HOP_XFER_SENDER_JOB_RELEASED);
 assert(!temp_bytes&&!t.id&&f.freed==1);
 temp_bytes=42;terminal=fail_release=cancelled=0;
 setup(&v,&f,&c,&t,&px,&rel,&ps,&pv,&se,&sp,d);fail_release=1;
 assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_WORKER)==HOP_XFER_SENDER_JOB_RELEASE_DEFERRED);
 assert(!px&&!terminal&&t.initialized&&f.freed==0);
 fail_release=0;assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_SHUTDOWN)==HOP_XFER_SENDER_JOB_RELEASED);
 assert(!temp_bytes&&!t.id&&f.freed==1);
 temp_bytes=0;terminal=fail_release=cancelled=0;
 setup(&v,&f,&c,&t,&px,&rel,&ps,&pv,&se,&sp,d);
 assert(hop_xfer_sender_job_release(&v,&ops,HOP_XFER_SENDER_JOB_OWNER_QUEUE)==HOP_XFER_SENDER_JOB_RELEASE_NOT_OWNER);
 assert(px&&t.initialized&&!f.freed);free(px);
 puts("PASS: Sender Job Lifecycle owner, resource, Task and deferred Shutdown ordering");return 0;
}
