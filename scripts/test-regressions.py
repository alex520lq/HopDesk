#!/usr/bin/env python3
"""Exercise shared production state and extracted platform glue, with API doubles.
Hosted CI still builds the full native applications. These focused tests are not
physical-desktop clipboard or two-machine end-to-end tests.
"""
import os, pathlib, re, shutil, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parent.parent

def extract(source,name):
    m=re.search(r'^[^\n;{}]*\b'+name+r'\([^;]*?\)\s*\{',source,re.M)
    if not m: raise AssertionError('Missing production function: '+name)
    start=m.start();i=source.index('{',start);depth=1;j=i+1
    while depth:
        if source[j]=='{':depth+=1
        elif source[j]=='}':depth-=1
        j+=1
    return source[start:j]

with tempfile.TemporaryDirectory(prefix='hopdesk-regression-') as directory:
    work=pathlib.Path(directory)
    def run(name,source,extra=(),threads=False):
        path=work/(name+'.c');path.write_text(source,encoding='utf-8')
        exe=work/(name+('.exe' if os.name=='nt' else ''))
        if os.name=='nt' and shutil.which('cl'):
            cmd=['cl','/nologo','/std:c11','/W4','/utf-8','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'src/core'),'/Fe:'+str(exe),str(path)]+[str(ROOT/x) for x in extra]
        else:
            cc=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
            cmd=[cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-I',str(ROOT/'src/core'),str(path)]+[str(ROOT/x) for x in extra]+['-o',str(exe)]
            if os.environ.get('HOP_TEST_SANITIZERS','1') != '0':
                cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            if threads:cmd+=['-pthread']
        subprocess.run(cmd,cwd=work,check=True)
        subprocess.run([str(exe)],cwd=work,check=True,timeout=30)

    run('tracking_config',(ROOT/'tests/regression_tests.c').read_text(encoding='utf-8'),['src/core/config.c'])
    assert not list(work.glob('*.tmp.*')), 'failed migration leaked temporary files'

    clip=(ROOT/'src/win/clipboard.c').read_text(encoding='utf-8')
    clip_preamble=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef void *HGLOBAL;typedef unsigned UINT;typedef int HWND;typedef unsigned DWORD;
#define IDT_CLIP_RETRY 1
#define CLIP_SUPPRESS_MS 400
#define hop_log(...) ((void)0)
static struct {HWND hwnd;} g_app={1};
static int s_suppress;static DWORD s_suppress_until;
static HGLOBAL s_pending_global;static UINT s_pending_format;static DWORD s_pending_sequence,s_remote_sequence;
static int can_open,fail_set,freed;static HGLOBAL clipboard;static DWORD sequence=1;
static int open_retry(HWND h){(void)h;return can_open;}
static int EmptyClipboard(void){clipboard=NULL;sequence++;return 1;}
static HGLOBAL SetClipboardData(UINT f,HGLOBAL h){(void)f;if(fail_set)return NULL;clipboard=h;sequence++;return h;}
static void CloseClipboard(void){}
static DWORD GetTickCount(void){return 1;}
static DWORD GetClipboardSequenceNumber(void){return sequence;}
static void GlobalFree(HGLOBAL h){(void)h;freed++;}
static void SetTimer(HWND h,int i,int t,void*p){(void)h;(void)i;(void)t;(void)p;}
static void KillTimer(HWND h,int i){(void)h;(void)i;}
'''
    clip_main=r'''
#define VALUE(n) ((void*)(uintptr_t)(n))
int main(void){
    can_open=0;apply_global(1,VALUE(101));
    can_open=1;assert(apply_global(1,VALUE(202)));
    clip_retry_pending();assert(clipboard==VALUE(202) && !s_pending_global && freed==1);
    can_open=0;apply_global(1,VALUE(303));
    clipboard=VALUE(404);sequence++;can_open=1;
    clip_retry_pending();assert(clipboard==VALUE(404) && !s_pending_global);
    fail_set=1;apply_global(1,VALUE(505));assert(s_pending_global==VALUE(505));
    fail_set=0;clip_retry_pending();assert(clipboard==VALUE(505) && !s_pending_global);
    puts("PASS: production Windows clipboard retry preserves newer remote/local content");return 0;
}
'''
    run('clipboard',clip_preamble+'\n'.join(extract(clip,n) for n in ['discard_pending','schedule_clip_retry','apply_global','clip_retry_pending'])+clip_main)
    for platform in ['win','mac']:
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8').replace('#include "xfer_receive.inc"', (ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8'))
        common=r'''
#include "hop.h"
#include "xfer_tracking.h"
#include <assert.h>
#include <wchar.h>
#define hop_log(...) ((void)0)
#define HOP_MSG_XFER_DONE 54
#define RX_CLOSE 1
#define AcquireSRWLockExclusive(p) ((void)(p))
#define ReleaseSRWLockExclusive(p) ((void)(p))
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
typedef int SOCKET;
typedef struct {uint32_t batch;int is_image,ok,n;} xf_job;
typedef xf_job xfer_job;
typedef struct {int kind;} rx_task;
static int s_status_lock,applied;static uint8_t wire[20];
static hop_xfer_tracking s_receive_status;
static struct {int active,cur_is_image,ntop;uint32_t batch;char *dir;uint8_t *img;size_t img_len,img_expected;int img_w,img_h,img_stride;void **tops;} s_rx;
static int send_small(int fd,uint8_t type,const uint8_t*p,uint32_t n,uint32_t batch){(void)batch;(void)fd;(void)type;assert(n==20);memcpy(wire,p,n);return 1;}
static void wq_push(rx_task*t){(void)t;}
static void wq_drain(void){}
static void rx_reset(void){s_rx.active=0;}
static void status_file_phase(int d,uint32_t b,int i,int p,uint32_t a){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_phase(&s_receive_status,b,i,p,a);}
static void status_end(int d,uint32_t b,int p,const char*s){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_end(&s_receive_status,b,p,s);}
static void apply_image(const uint8_t*p,int w,int h,int stride){(void)p;(void)w;(void)h;(void)stride;applied++;}
static void apply_paths(const void*p,int n){(void)p;(void)n;}
#define clip_apply_remote_image apply_image
#define clipboard_apply_image apply_image
#define clip_apply_remote_paths apply_paths
#define clipboard_apply_paths apply_paths
'''
        main=r'''
int main(void){
    xf_job j={0};j.batch=10;j.is_image=1;j.ok=1;assert(send_done(0,&j));
    assert(hop_get_be32(wire+4)==1 && hop_get_be32(wire+8)==1);
    uint8_t pixel[4]={0};s_rx.active=1;s_rx.batch=j.batch;s_rx.cur_is_image=1;
    s_rx.img=pixel;s_rx.img_len=4;s_rx.img_expected=4;
    hop_xfer_tracking_begin(&s_receive_status,j.batch,HOP_XFER_DIR_RECEIVE,0,0,"Image",HOP_XFER_RECEIVING);
    hop_xfer_tracking_file(&s_receive_status,j.batch,1,"Image",4,0,HOP_XFER_FILE_RECEIVING);
    rx_finish_batch(j.batch,hop_get_be32(wire+4),hop_get_be32(wire+8));
    assert(applied==1 && !s_receive_status.status.active && s_receive_status.status.phase==HOP_XFER_COMPLETED);
    assert(s_receive_status.status.files_done==1);
    rx_finish_batch(j.batch,1,1);assert(applied==1);
    s_rx.active=1;s_rx.batch=11;s_rx.img_len=3;
    hop_xfer_tracking_begin(&s_receive_status,11,HOP_XFER_DIR_RECEIVE,1,4,"Image",HOP_XFER_RECEIVING);
    rx_finish_batch(11,1,1);assert(applied==1 && s_receive_status.status.phase==HOP_XFER_FAILED);
    puts("PASS: production IMAGE DONE and clipboard completion path");return 0;
}
'''
        run('image_'+platform,common+extract(source,'send_done')+'\n'+extract(source,'rx_finish_batch')+main,['src/core/protocol.c'])
    # Run the actual per-platform state wrappers, not only a reimplemented model.
    for platform in ['win','mac']:
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8').replace('#include "xfer_receive.inc"', (ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8'))
        preamble=r'''
#include "hop.h"
#include "xfer_tracking.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#define AcquireSRWLockExclusive(p) ((void)(p))
#define ReleaseSRWLockExclusive(p) ((void)(p))
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static int s_status_lock, saves;
static uint64_t s_status_revision;
static hop_xfer_tracking s_send_status,s_receive_status;
static void *s_retry_paths[HOP_XFER_FILE_LIMIT];
static hop_xfer_history_entry s_history[HOP_XFER_HISTORY_LIMIT];static int s_history_count;
static void history_save(void){saves++;}
'''
        main=r'''
int main(void){
    status_begin(1,HOP_XFER_DIR_SEND,1,100,"out",HOP_XFER_SENDING);
    status_begin(1,HOP_XFER_DIR_RECEIVE,0,0,"in",HOP_XFER_RECEIVING);
    status_add_received_file(1,1,"in",4,0);
    status_chunk(HOP_XFER_DIR_RECEIVE,1,1,0,4);
    assert(s_receive_status.status.bytes_done==4 && s_receive_status.status.bytes_total==4);
    assert(s_send_status.status.active && s_send_status.status.bytes_done==0);
    status_end(HOP_XFER_DIR_SEND,1,HOP_XFER_FAILED,"retries exhausted");
    assert(s_receive_status.status.active && s_history[0].direction==HOP_XFER_DIR_SEND);
    status_end(HOP_XFER_DIR_SEND,1,HOP_XFER_FAILED,"duplicate");assert(saves==1);
    status_file_phase(HOP_XFER_DIR_RECEIVE,1,0,HOP_XFER_FILE_COMPLETED,1);
    status_end(HOP_XFER_DIR_RECEIVE,1,HOP_XFER_COMPLETED,"done");assert(saves==2);
    assert(s_history[0].direction==HOP_XFER_DIR_RECEIVE && s_history[0].files_done==1);
    puts("PASS: production per-direction state wrappers and exactly-once history");return 0;
}
'''
        names=['status_for','status_begin','status_chunk','status_file_phase','status_end','status_add_received_file']
        run('wrappers_'+platform,preamble+'\n'.join(extract(source,n) for n in names)+main)

    if os.name!='nt':
        source=(ROOT/'src/mac/xfer.c').read_text(encoding='utf-8')
        preamble=r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <time.h>
#include "xfer_status.h"
static pthread_mutex_t s_status_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_history_io_lock=PTHREAD_MUTEX_INITIALIZER;
static hop_xfer_history_entry s_history[HOP_XFER_HISTORY_LIMIT];static int s_history_count;
typedef struct {char magic[8];uint32_t version,record_size,count;} hop_history_disk_header;
static char s_history_path[PATH_MAX]="history.bin";
static int history_prepare_path(void){return 1;}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int opened,resume_writer,second_started,second_done;
static _Thread_local int slow_writer;
static FILE* test_open(const char*p,const char*m){
    FILE*f=fopen(p,m);
    if(slow_writer){pthread_mutex_lock(&gate);opened=1;pthread_cond_broadcast(&cond);
        while(!resume_writer)pthread_cond_wait(&cond,&gate);
        pthread_mutex_unlock(&gate);}
    return f;
}
'''
        code=extract(source,'history_save').replace('fopen(temp,"wb")','test_open(temp,"wb")')
        main=r'''
static void* first(void*p){(void)p;slow_writer=1;history_save();return NULL;}
static void* second(void*p){(void)p;pthread_mutex_lock(&gate);second_started=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
    history_save();pthread_mutex_lock(&gate);second_done=1;pthread_mutex_unlock(&gate);return NULL;}
int main(void){
    s_history_count=1;s_history[0].batch=1;
    pthread_t a,b;assert(pthread_create(&a,NULL,first,NULL)==0);
    pthread_mutex_lock(&gate);while(!opened)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
    pthread_mutex_lock(&s_status_lock);s_history_count=2;s_history[1].batch=2;pthread_mutex_unlock(&s_status_lock);
    assert(pthread_create(&b,NULL,second,NULL)==0);
    pthread_mutex_lock(&gate);while(!second_started)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
    struct timespec pause={0,100000000L};nanosleep(&pause,NULL);
    pthread_mutex_lock(&gate);assert(!second_done);resume_writer=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
    pthread_join(a,NULL);pthread_join(b,NULL);
    hop_history_disk_header h;FILE*f=fopen(s_history_path,"rb");assert(f);assert(fread(&h,sizeof(h),1,f)==1);fclose(f);
    assert(h.count==2);assert(access("history.bin.tmp",F_OK)!=0);
    puts("PASS: concurrent production history saves serialize snapshot through rename");return 0;
}
'''
        run('history',preamble+code+main,threads=True)

    # Exact production case bodies + lifecycle helpers; only OS I/O is doubled.
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        receiver=(ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8')
        state_start=receiver.index('typedef struct {',receiver.index('/* ---------------- 接收')) if platform=='win' else receiver.index('static struct {',receiver.index('/* ---------------- 接收'))
        state_end=receiver.index('static xfer_rx s_rx;',state_start)+len('static xfer_rx s_rx;') if platform=='win' else receiver.index('} s_rx;',state_start)+len('} s_rx;')
        handler=extract(receiver,'on_data_msg')
        def cases(first,after):
            return handler[handler.index('        case '+first+':'):handler.index('        case '+after+':')]
        selected=cases('HOP_MSG_IMAGE_BEGIN','HOP_MSG_FILE_ACK')+cases('HOP_MSG_XFER_ABORT','HOP_MSG_XFER_DONE')
        selected+=handler[handler.index('        case HOP_MSG_XFER_DONE:'):handler.index('        default:')]
        fixture=(ROOT/'tests/receive_abort_fixture.c.in').read_text(encoding='utf-8')
        values={
            'RX_STATE':receiver[state_start:state_end],
            'WIRE_SEND':extract(sender,'send_abort_msg' if platform=='win' else 'send_abort'),
            'STATUS_FUNCTIONS':'\n'.join(extract(sender,n) for n in ['status_for','status_begin','status_end']),
            'CLOSE_FILE':extract(receiver,'rx_close_file'),
            'CLEAR_STATE':extract(receiver,'rx_clear_state'),
            'RESET_STATE':extract(receiver,'rx_reset'),
            'TERMINATE':extract(receiver,'rx_terminate'),
            'BEGIN_BATCH':extract(receiver,'rx_begin_batch'),
            'FINISH_BATCH':extract(receiver,'rx_finish_batch'),
            'CANCEL_LOCAL':extract(receiver,'rx_cancel_local'),
            'BUSY':extract(sender,'xfer_busy'),
            'RECEIVE_CASES':selected,
        }
        for key,value in values.items():fixture=fixture.replace('@'+key+'@',value)
        assert '@' not in fixture
        assert 'if (!s_rx.active || s_rx.batch != batch) return;' in cases('HOP_MSG_FILE_BEGIN','HOP_MSG_IMAGE_BEGIN')
        assert 'if (!s_rx.active || s_rx.batch != batch) return;' in cases('HOP_MSG_IMAGE_BEGIN','HOP_MSG_FILE_CHUNK')
        if platform=='win':fixture='#define FIXTURE_WINDOWS 1\n'+fixture
        run('abort_'+platform,fixture,['src/core/protocol.c','src/core/crc32.c','src/core/xfer_validate.c'])

    inject=(ROOT/'src/mac/inject.c').read_text(encoding='utf-8')
    fixture=(ROOT/'tests/inject_release_fixture.c.in').read_text(encoding='utf-8')
    names=['recompute_flags','mod_index','post','inject_release_all','inject_reset','inject_key','inject_button']
    fixture=fixture.replace('@FUNCTIONS@','\n'.join(extract(inject,n) for n in names))
    run('inject_release',fixture,['src/core/keymap.c'])

    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        receiver=(ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8')
        fixture=(ROOT/'tests/session_transport_fixture.c.in').read_text(encoding='utf-8')
        functions='\n'.join([extract(sender,'job_bind_session'),extract(sender,'send_small'),extract(sender,'send_chunk'),extract(receiver,'rx_bind_session'),extract(receiver,'on_data_frame_checked'),extract(receiver,'on_data_frame')])
        fixture=fixture.replace('@FUNCTIONS@',functions)
        run('session_transport_'+platform,fixture,['src/core/protocol.c','src/core/session.c','src/core/clock.c','src/core/watchdog.c'],threads=True)

    print('All targeted regressions passed.',flush=True)
