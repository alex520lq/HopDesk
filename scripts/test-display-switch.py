#!/usr/bin/env python3
"""Exercise production reply/session gates with Windows API doubles, not hardware."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
source = (ROOT / 'src/win/shared_display.c').read_text(encoding='utf-8')
start = re.search(r'^void shared_display_message\(', source, re.M).start()
a = source.index('{', start)
depth, b = 1, a + 1
while depth:
    depth += (source[b] == '{') - (source[b] == '}')
    b += 1
body = source[start:b]
fixture = r'''
#include "hop.h"
#include "display_switch.h"
#include <assert.h>
#include <stdio.h>
#define HOPDESK_UI_DISPLAY_TO_MAC 0
#define HOPDESK_UI_DISPLAY_TO_WINDOWS 1
static struct {void *net;struct {int disp_shared;} cfg;} g_app;
static unsigned long epoch=7,s_epoch=7;
static uint32_t s_expected=10;
static int s_reply_ok,s_reply_lock,s_reply_event,busy,signals,actions,completions,sent;
static void AcquireSRWLockExclusive(int *p){(void)p;}
static void ReleaseSRWLockExclusive(int *p){(void)p;}
static void SetEvent(int p){(void)p;++signals;}
static unsigned long net_server_epoch(void *p){(void)p;return epoch;}
static int shared_display_busy(void){return busy;}
static void notify_complete(void *n,unsigned long e,int target,int ok){(void)n;(void)e;(void)target;assert(!ok);++completions;}
static void win_ui_display_action(int action){actions=action+1;}
static int net_server_send(void *n,uint8_t type,const void *p,uint32_t size){
    (void)n;assert(type==HOP_MSG_DISP_SWITCH);hop_display_msg m;
    assert(hop_display_decode(&m,p,size));assert(m.op==HOP_DISPLAY_LOCAL);++sent;return 1;
}
@PRODUCTION@
static void deliver(hop_display_msg m){uint8_t wire[HOP_DISPLAY_WIRE_SIZE];assert(hop_display_encode(wire,&m));shared_display_message(wire,sizeof(wire));}
int main(void){
    hop_display_msg m={HOP_DISPLAY_RESULT,1,9,0};deliver(m);assert(!signals);
    m.id=10;epoch=8;deliver(m);assert(!signals);
    epoch=7;deliver(m);assert(signals==1&&s_reply_ok);
    s_expected=0;deliver(m);assert(signals==1);
    m=(hop_display_msg){HOP_DISPLAY_REQUEST,0,0,HOP_DISPLAY_MAC};
    busy=1;deliver(m);assert(completions==1&&!actions);
    busy=0;g_app.cfg.disp_shared=0;deliver(m);assert(sent==1&&!actions);
    g_app.cfg.disp_shared=1;deliver(m);assert(actions==1);
    m.value=HOP_DISPLAY_WINDOWS;deliver(m);assert(actions==2);
    puts("PASS: shared display rejects stale replies/epochs, duplicate completion, busy requests and preserves ordinary Mac targets (API doubles)");
}
'''.replace('@PRODUCTION@', body)
with tempfile.TemporaryDirectory(prefix='hopdesk-display-') as d:
    p=pathlib.Path(d)
    (p/'test.c').write_text(fixture, encoding='utf-8')
    sources=[str(p/'test.c'),str(ROOT/'src/core/display_switch.c'),str(ROOT/'src/core/protocol.c')]
    exe=p/('test.exe' if os.name=='nt' else 'test')
    if os.name=='nt' and shutil.which('cl.exe'):
        cmd=['cl.exe','/nologo','/std:c11','/W4','/utf-8','/D_CRT_SECURE_NO_WARNINGS',
             '/I'+str(ROOT/'src/core'),'/Fe:'+str(exe)]+sources
    else:
        compiler=os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or 'clang'
        cmd=[compiler,'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'src/core')]+sources+['-o',str(exe)]
    subprocess.run(cmd,check=True,cwd=p)
    subprocess.run([str(exe)],check=True,timeout=5)
