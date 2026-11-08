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

    run('tracking_config',(ROOT/'tests/regression_tests.c').read_text(encoding='utf-8'),['src/core/config.c','src/core/xfer_tracking.c'])
    run('rx_protocol_adapter',r'''
#include "hop.h"
#include "xfer_rx_protocol_adapter.h"
#include <assert.h>
#include <stdio.h>
typedef struct { int calls; int marker; } test_context;
static void on_file_begin(void *context,const uint8_t *payload,uint32_t length) {
    test_context *state=(test_context *)context;
    assert(state && state->marker==73 && length==2 && payload[0]==4);
    state->calls++;
}
int main(void) {
    hop_xfer_rx_protocol_adapter adapter={0};
    hop_xfer_rx_dispatch_handlers handlers={0};
    test_context context={0,73};
    uint8_t payload[2]={4,5};
    assert(!hop_xfer_rx_protocol_adapter_dispatch(&adapter,HOP_MSG_FILE_BEGIN,payload,2));
    assert(!hop_xfer_rx_protocol_adapter_init(NULL,&handlers,&context));
    assert(!hop_xfer_rx_protocol_adapter_init(&adapter,NULL,&context));
    handlers.file_begin=on_file_begin;
    assert(hop_xfer_rx_protocol_adapter_init(&adapter,&handlers,&context));
    assert(hop_xfer_rx_protocol_adapter_dispatch(&adapter,HOP_MSG_FILE_BEGIN,payload,2));
    assert(context.calls==1);
    assert(!hop_xfer_rx_protocol_adapter_dispatch(&adapter,0xff,payload,2));
    assert(context.calls==1);
    puts("PASS: receiver protocol adapter validates lifecycle and forwards explicit context");
    return 0;
}
''',['src/core/xfer_rx_protocol_adapter.c','src/core/xfer_rx_dispatch.c'])
    assert not list(work.glob('*.tmp.*')), 'failed migration leaked temporary files'
    run('ack_window_checkpoint',r'''
#include "xfer_ack_window.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    int checkpoint=0;
    assert(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,0,0));
    assert(checkpoint==0);
    assert(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,1));
    assert(checkpoint==0);
    assert(hop_xfer_ack_window_advance_checkpoint(&checkpoint,0,1));
    assert(checkpoint==1);
    assert(!hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,0));
    assert(checkpoint==1);
    assert(hop_xfer_ack_window_advance_checkpoint(&checkpoint,1,1));
    assert(checkpoint==2);
    puts("PASS: only contiguous ACK-confirmed entries advance resume checkpoint");
    return 0;
}
''',['src/core/xfer_ack_window.c'])

    run('rx_dispatch',r'''
#include "hop.h"
#include "xfer_rx_dispatch.h"
#include <assert.h>
int main(void) {
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_BEGIN)==HOP_RX_DISPATCH_FILE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_IMAGE_BEGIN)==HOP_RX_DISPATCH_IMAGE_BEGIN);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_CHUNK)==HOP_RX_DISPATCH_FILE_CHUNK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_END)==HOP_RX_DISPATCH_FILE_END);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_FILE_ACK)==HOP_RX_DISPATCH_FILE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_ABORT)==HOP_RX_DISPATCH_ABORT);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE)==HOP_RX_DISPATCH_DONE);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_DONE_ACK)==HOP_RX_DISPATCH_DONE_ACK);
    assert(hop_xfer_rx_dispatch_classify(HOP_MSG_XFER_HELLO)==HOP_RX_DISPATCH_UNKNOWN);
    assert(hop_xfer_rx_dispatch_classify(255)==HOP_RX_DISPATCH_UNKNOWN);
    return 0;
}
''',['src/core/xfer_rx_dispatch.c'])

    run('rx_batch_state',r'''
#include "xfer_rx_batch_state.h"
#include <assert.h>
int main(void) {
    hop_xfer_rx_batch_state s;
    hop_xfer_rx_batch_state_init(&s);
    assert(hop_xfer_rx_batch_state_begin(&s,7));
    assert(hop_xfer_rx_batch_state_matches(&s,7));
    assert(!hop_xfer_rx_batch_state_begin(&s,8));
    assert(!hop_xfer_rx_batch_state_end(&s,8));
    assert(hop_xfer_rx_batch_state_end(&s,7));
    assert(!s.active && !s.batch && s.generation==1);
    assert(hop_xfer_rx_batch_state_begin(&s,7));
    assert(s.generation==2);
    hop_xfer_rx_batch_state_reset(&s);
    assert(!s.active && !s.batch && s.generation==2);
    return 0;
}
''',['src/core/xfer_rx_batch_state.c'])

    # Cards are painted by the page, never sibling HWNDs obscuring its controls.
    ui=(ROOT/'src/win/main_ui.c').read_text(encoding='utf-8')
    group_fn=extract(ui,'group')
    page_fn=extract(ui,'page_proc')
    page_create_fn=extract(ui,'page_create')
    assert 'HopDeskCard' not in ui and 'CreateWindow' not in group_fn
    assert 'WS_CLIPCHILDREN' in page_create_fn and 'paint_cards(h,dc,GetScrollPos(h,SB_VERT))' in page_fn
    ui_preamble=r'''
#include <assert.h>
#include <stddef.h>
#include <wchar.h>
#include <stdio.h>
typedef int HWND;typedef int HDC;typedef int HPEN;typedef int HGDIOBJ;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {HWND page;const wchar_t *title;int x,y,w,h;} ui_card;
static ui_card s_cards[32];static int s_card_count;static int s_font=3,scale=1;
static int dip(int n){return n*scale;}
#define PS_SOLID 0
#define NULL_BRUSH 0
#define TRANSPARENT 1
#define DT_SINGLELINE 1
#define DT_LEFT 0
#define RGB(r,g,b) 0
static int boxes,titles,last_top,last_left,last_right,last_bottom,text_top;
static HPEN CreatePen(int style,int width,int color){(void)style;(void)width;(void)color;return 1;}
static HGDIOBJ SelectObject(HDC dc,HGDIOBJ o){(void)dc;return o;}
static HGDIOBJ GetStockObject(int n){return n;}
static int SetBkMode(HDC dc,int mode){(void)dc;return mode;}
static int SetTextColor(HDC dc,int color){(void)dc;return color;}
static int RoundRect(HDC dc,int l,int t,int r,int b,int x,int y){(void)dc;(void)x;(void)y;boxes++;last_left=l;last_top=t;last_right=r;last_bottom=b;return 1;}
static int DrawTextW(HDC dc,const wchar_t *s,int n,RECT *r,int flags){(void)dc;(void)n;(void)flags;assert(s && *s);titles++;text_top=r->top;return 1;}
static int DeleteObject(HGDIOBJ o){(void)o;return 1;}
'''
    ui_main=r'''
int main(void){
    group(1,L"first",20,18,800,145);group(2,L"other page",10,20,30,40);
    paint_cards(1,0,30);assert(boxes==1 && titles==1);
    assert(last_left==20 && last_top==-12 && last_right==819 && last_bottom==132 && text_top==-2);
    boxes=titles=0;scale=2;paint_cards(2,0,10);assert(boxes==1 && titles==1);
    assert(last_left==20 && last_top==30 && last_right==79 && last_bottom==109 && text_top==50);
    puts("PASS: Windows page card painting and scroll offsets (API doubles)");return 0;
}
'''
    run('win_ui_cards',ui_preamble+group_fn+'\n'+extract(ui,'paint_cards')+ui_main)

    # Mac edge entry must keep tracking drag events before sharing starts.
    capture=(ROOT/'src/mac/capture.c').read_text(encoding='utf-8')
    edge_helper=extract(capture,'is_edge_motion_event')
    assert 'if(is_edge_motion_event(type))' in extract(capture,'capture_event')
    edge_preamble=r'''
#include <assert.h>
#include <stdio.h>
typedef int CGEventType;
enum {kCGEventMouseMoved=1,kCGEventLeftMouseDragged=2,kCGEventRightMouseDragged=3,
      kCGEventOtherMouseDragged=4,kCGEventLeftMouseDown=5};
'''
    edge_main=r'''
int main(void){
    assert(is_edge_motion_event(kCGEventMouseMoved));
    assert(is_edge_motion_event(kCGEventLeftMouseDragged));
    assert(is_edge_motion_event(kCGEventRightMouseDragged));
    assert(is_edge_motion_event(kCGEventOtherMouseDragged));
    assert(!is_edge_motion_event(kCGEventLeftMouseDown));
    puts("PASS: macOS edge entry includes held-button drag events");return 0;
}
'''
    run('mac_edge_motion',edge_preamble+edge_helper+edge_main)

    # Windows server queues ENTER before transferring held keys/buttons, and
    # leaves local input untouched if ENTER could not be queued.
    mouse=(ROOT/'src/win/mouse_capture.c').read_text(encoding='utf-8')
    handoff_preamble=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t BYTE;typedef uint32_t DWORD;typedef struct {int x,y;} POINT;
typedef struct {int server_edge,edge_margin_px,switch_cooldown_ms,screen_w,screen_h;} cfg_t;
typedef struct {cfg_t cfg;int peer_w,peer_h,state;void *net;} app_t;
static app_t g_app;
enum {HOP_EDGE_LEFT=0,HOP_EDGE_RIGHT=1,HOP_EDGE_TOP=2,HOP_EDGE_BOTTOM=3,
      HOP_LINK_LOCAL=1,HOP_LINK_SHARING=2,HOP_BTN_LEFT=0,
      HOP_MSG_MOUSE_BUTTON=11,HOP_MSG_MOUSE_ENTER=14,HOP_MSG_KEY_DOWN=20};
enum {SM_CYVIRTUALSCREEN=1,SM_CXVIRTUALSCREEN=2,SM_CYSCREEN=3,SM_CXSCREEN=4};
static int s_sharing,s_have_last,s_physical_buttons[3],s_buttons[3],event_count,fail_enter,local_releases;
static POINT s_anchor,s_last;static DWORD s_cooldown_until;static uint8_t events[16];
static int net_server_send(void *net,uint8_t type,const void *p,uint32_t len){
    (void)net;(void)p;(void)len;events[event_count++]=type;
    return !(fail_enter&&type==HOP_MSG_MOUSE_ENTER);
}
static void hop_put_be16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static int edge_boundary(int *edge_x,int *is_left){*edge_x=0;*is_left=1;return 1;}
static int GetSystemMetrics(int what){return (what==SM_CYVIRTUALSCREEN||what==SM_CYSCREEN)?1080:1920;}
static DWORD GetTickCount(void){return 100;}
static void kbd_begin_sharing(void){uint8_t p[2]={0};net_server_send(g_app.net,HOP_MSG_KEY_DOWN,p,2);}
static void inject_local_button(int b,int down){(void)b;if(!down)local_releases++;}
static int SetCursorPos(int x,int y){(void)x;(void)y;return 1;}
static void ui_refresh(app_t *app){(void)app;}
#define hop_log(...) ((void)0)
'''
    handoff_main=r'''
int main(void){
    POINT trigger={0,540};
    memset(&g_app,0,sizeof(g_app));g_app.cfg.server_edge=HOP_EDGE_LEFT;
    g_app.cfg.edge_margin_px=40;g_app.cfg.switch_cooldown_ms=300;
    g_app.cfg.screen_w=1920;g_app.cfg.screen_h=1080;g_app.peer_w=1440;g_app.peer_h=900;
    g_app.state=HOP_LINK_LOCAL;s_physical_buttons[HOP_BTN_LEFT]=1;
    enter_sharing(trigger);
    assert(event_count==3 && events[0]==HOP_MSG_MOUSE_ENTER &&
           events[1]==HOP_MSG_KEY_DOWN && events[2]==HOP_MSG_MOUSE_BUTTON);
    assert(s_sharing && g_app.state==HOP_LINK_SHARING && local_releases==1);

    memset(&g_app,0,sizeof(g_app));g_app.cfg.server_edge=HOP_EDGE_LEFT;
    g_app.cfg.edge_margin_px=40;g_app.cfg.screen_w=1920;g_app.cfg.screen_h=1080;
    g_app.peer_w=1440;g_app.peer_h=900;g_app.state=HOP_LINK_LOCAL;
    memset(s_physical_buttons,0,sizeof(s_physical_buttons));s_physical_buttons[0]=1;
    s_sharing=0;s_have_last=0;event_count=0;local_releases=0;fail_enter=1;
    enter_sharing(trigger);
    assert(event_count==1 && events[0]==HOP_MSG_MOUSE_ENTER);
    assert(!s_sharing && g_app.state==HOP_LINK_LOCAL && local_releases==0);
    puts("PASS: Windows ENTER ordering and failed-handoff rollback");return 0;
}
'''
    run('win_handoff_order',handoff_preamble+extract(mouse,'send_enter')+'\n'+
        extract(mouse,'send_button')+'\n'+extract(mouse,'enter_sharing')+handoff_main)

    # Both platform clients clear remote key/button state on server return.
    win_inject=(ROOT/'src/win/inject.c').read_text(encoding='utf-8')
    win_leave_preamble=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t BYTE;typedef uint16_t WORD;typedef uint32_t DWORD;typedef int32_t LONG;
typedef struct {int role,client_edge,edge_margin_px;} cfg_t;
typedef struct {cfg_t cfg;int peer_w,peer_h,state;void *net;} app_t;
static app_t g_app;static BYTE held[256],buttons[3],paste_suppressed[256];static double px,py,over;static int sharing,clip_request_result,injected_key_events;
static int released_keys,released_buttons,refreshes;
enum {HOP_ROLE_CLIENT=1,HOP_EDGE_TOP=2,HOP_LINK_LOCAL=1,HOP_LINK_SHARING=2,
 HOP_KEY_NONE=0xffff,HOP_MSG_EDGE_LEAVE=13,HOP_MSG_MOUSE_ENTER=14,HOP_MSG_MOUSE_MOVE=10,
 HOP_MSG_MOUSE_BUTTON=11,HOP_MSG_MOUSE_WHEEL=12,HOP_MSG_KEY_DOWN=20,HOP_MSG_KEY_UP=21,
 SM_CXSCREEN=1,SM_CYSCREEN=2,MOUSEEVENTF_HWHEEL=4,MOUSEEVENTF_WHEEL=8,
 VK_LCONTROL=0xA2,VK_RCONTROL=0xA3,VK_LWIN=0x5B,VK_RWIN=0x5C};
static void key(WORD vk,int down){held[vk]=(BYTE)down;injected_key_events++;if(!down)released_keys++;}
static void button(int b,int down){buttons[b]=(BYTE)down;if(!down)released_buttons++;}
static void mouse(DWORD flags,LONG dx,LONG dy,DWORD data){(void)flags;(void)dx;(void)dy;(void)data;}
static void enter(int along){(void)along;sharing=1;}
static double edge_margin(double w,double h){(void)w;(void)h;return 40;}
static int GetSystemMetrics(int what){return what==SM_CYSCREEN?1080:1920;}
static int SetCursorPos(int x,int y){(void)x;(void)y;return 1;}
static uint16_t hop_get_be16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static WORD hop_mac_to_vk(uint16_t keycode){return keycode==0x09?'V':(keycode==0x3B?VK_LCONTROL:65);}
static int clip_request_remote_paste(void){return clip_request_result;}
static int net_server_send(void *net,uint8_t type,const void *p,uint32_t len){(void)net;(void)type;(void)p;(void)len;return 1;}
static void ui_refresh(app_t *app){(void)app;refreshes++;}
'''
    win_leave_main=r'''
int main(void){
    g_app.cfg.role=HOP_ROLE_CLIENT;g_app.state=HOP_LINK_SHARING;
    sharing=1;held[65]=1;buttons[0]=1;
    win_inject_message(HOP_MSG_EDGE_LEAVE,NULL,0);
    assert(!sharing && !held[65] && !buttons[0]);
    assert(g_app.state==HOP_LINK_LOCAL && released_keys==1 && released_buttons==1 && refreshes==1);
    /* A remote file Offer consumes the first Ctrl+V and releases the modifier;
       the physical key-up frames must not leak to the foreground application. */
    sharing=1;held[VK_LCONTROL]=1;clip_request_result=1;injected_key_events=0;
    uint8_t paste_key[2]={0,0x09};
    win_inject_message(HOP_MSG_KEY_DOWN,paste_key,sizeof(paste_key));
    assert(!held[VK_LCONTROL] && paste_suppressed['V'] && paste_suppressed[VK_LCONTROL]);
    win_inject_message(HOP_MSG_KEY_UP,paste_key,sizeof(paste_key));
    uint8_t ctrl_key[2]={0,0x3B};
    win_inject_message(HOP_MSG_KEY_UP,ctrl_key,sizeof(ctrl_key));
    assert(!paste_suppressed['V'] && !paste_suppressed[VK_LCONTROL]);
    assert(injected_key_events==1);
    puts("PASS: Windows client releases stuck input and consumes lazy-paste key sequence");return 0;
}
'''
    run('win_client_server_return',win_leave_preamble+
        extract(win_inject,'win_inject_reset')+'\n'+extract(win_inject,'win_inject_message')+win_leave_main)

    # Server return must follow all remote release frames.
    mac_return=extract(capture,'mac_capture_return')
    assert mac_return.index('send_key(k, 0)') < mac_return.index('net_send(HOP_MSG_EDGE_LEAVE, NULL, 0)')
    assert mac_return.index('send_button(b, 0)') < mac_return.index('net_send(HOP_MSG_EDGE_LEAVE, NULL, 0)')
    win_return=extract(mouse,'enter_local')
    assert win_return.index('kbd_end_sharing()') < win_return.index('net_server_send(g_app.net, HOP_MSG_EDGE_LEAVE, NULL, 0)')
    assert win_return.index('send_button(i, 0)') < win_return.index('net_server_send(g_app.net, HOP_MSG_EDGE_LEAVE, NULL, 0)')


    clip=(ROOT/'src/win/clipboard.c').read_text(encoding='utf-8')
    clip_preamble=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef void *HGLOBAL;typedef unsigned UINT;typedef int HWND;typedef unsigned DWORD;
#define IDT_CLIP_RETRY 1
#define CF_HDROP 15
#define hop_log(...) ((void)0)
static struct {HWND hwnd;} g_app={1};
static HGLOBAL s_pending_global;
static UINT s_pending_format;
static DWORD s_pending_sequence,s_remote_sequence,s_last_sequence;
static int s_pending_clear_remote;
static int can_open,fail_set,freed,clear_remote_calls,paste_notices;
static void ui_notify_paste_ready(void){paste_notices++;}
static HGLOBAL clipboard;static DWORD sequence=1;
static int apply_global_expected(UINT,HGLOBAL,DWORD,int,int);
static void clip_clear_remote_offer_local(void){clear_remote_calls++;}
static int open_retry(HWND h){(void)h;return can_open;}
static int EmptyClipboard(void){clipboard=NULL;sequence++;return 1;}
static HGLOBAL SetClipboardData(UINT f,HGLOBAL h){(void)f;if(fail_set)return NULL;clipboard=h;sequence++;return h;}
static void CloseClipboard(void){}
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
    assert(s_remote_sequence==sequence && s_last_sequence==sequence);
    clip_retry_pending();assert(s_pending_global==VALUE(505));
    assert(s_remote_sequence==sequence && s_last_sequence==sequence);
    fail_set=0;clip_retry_pending();assert(clipboard==VALUE(505) && !s_pending_global);
    /* Completed cross-device paste must not notify until a busy clipboard is
     * successfully written; a subsequent user clipboard change cancels it. */
    can_open=0;DWORD expected=sequence;
    clip_on_paste_message(VALUE(606),expected);
    assert(s_pending_global==VALUE(606) && paste_notices==0);
    can_open=1;clip_retry_pending();
    assert(clipboard==VALUE(606) && !s_pending_global && paste_notices==1);
    expected=sequence;clip_on_paste_message(VALUE(707),expected);
    assert(clipboard==VALUE(707) && paste_notices==2);
    can_open=0;expected=sequence;clip_on_paste_message(VALUE(808),expected);
    assert(s_pending_global==VALUE(808));clipboard=VALUE(909);sequence++;
    can_open=1;clip_retry_pending();
    assert(clipboard==VALUE(909) && !s_pending_global && paste_notices==2);
    puts("PASS: production Windows retry waits for clipboard, notifies only on successful paste apply and preserves newer content");return 0;
}
'''
    run('clipboard',clip_preamble+'\n'.join(extract(clip,n) for n in ['discard_pending','schedule_clip_retry','apply_global_expected','apply_global','clip_retry_pending','clip_on_paste_message'])+clip_main)
    # A cross-device paste batch must never fall through to the ordinary clipboard writer.
    for platform in ['win','mac']:
        rx='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc'))
        if platform=='win':
            assert 'clip_complete_remote_paste(batch,(const wchar_t *const *)s_rx.tops,s_rx.ntop,1)' in rx
            assert 'clip_complete_remote_paste(batch,NULL,0,0)' in rx
            clip=(ROOT/'src/win/clipboard.c').read_text(encoding='utf-8')
            assert 'GetClipboardSequenceNumber()!=expected' in clip
            paste_message=extract(clip,'clip_on_paste_message')
            assert 'SendInput(' not in paste_message
            assert 'apply_global_expected(CF_HDROP,h,expected_sequence,1,1)' in paste_message
        else:
            assert 'clipboard_complete_remote_paste(batch,(const char *const *)s_rx.tops,s_rx.ntop,1)' in rx
            assert 'clipboard_complete_remote_paste(batch,NULL,0,0)' in rx
            clip=(ROOT/'src/mac/clipboard.m').read_text(encoding='utf-8')
            assert '[pb changeCount] != expected_count' in clip
            apply_objects=extract(clip,'apply_objects_retry')
            assert 'inject_paste_shortcut()' not in apply_objects
            assert 'statusbar_notify_paste_ready()' in apply_objects

        # RX storage/accounting lives in the production writer implementation,
        # not in Receiver protocol code. Windows has completed this extraction;
        # macOS is intentionally checked against its current adapter until parity lands.
        if platform == 'win':
            writer=(ROOT/'src/win/xfer_writer_queue.c').read_text(encoding='utf-8')
            assert 'hop_xfer_buffer_pool_acquire_locked(' in writer and '&queue->pool' in writer
            assert 'hop_xfer_buffer_pool_release_locked(&queue->pool' in writer
            assert 'hop_xfer_rx_queue_can_push(&queue->accounting' in writer
            assert 'hop_xfer_rx_queue_reserve(&queue->accounting' in writer
            assert 'hop_xfer_rx_queue_commit_reserved(&queue->accounting)' in writer
            assert 'hop_xfer_rx_queue_drained(&queue->accounting)' in writer
            assert 'hop_xfer_buffer_pool_acquire_locked' not in rx
            assert 'static int wq_worker_take' not in rx
            assert 'hop_writer_queue_enqueue_write' in rx
        else:
            writer=(ROOT/'src/mac/xfer_writer_queue.c').read_text(encoding='utf-8')
            assert 'hop_xfer_buffer_pool_acquire_locked(' in writer and '&queue->pool' in writer
            assert 'hop_xfer_buffer_pool_release_locked(&queue->pool' in writer
            assert 'hop_xfer_rx_queue_can_push(&queue->accounting' in writer
            assert 'hop_xfer_rx_queue_reserve(&queue->accounting' in writer
            assert 'hop_xfer_rx_queue_commit_reserved(&queue->accounting)' in writer
            assert 'hop_xfer_rx_queue_drained(&queue->accounting)' in writer
            assert 'hop_xfer_buffer_pool_acquire_locked' not in rx
            assert 'static int wq_worker_take' not in rx
            assert 'hop_writer_queue_enqueue_write' in rx
        assert 'rx_buffer_slot' not in rx and 'wq_pool_acquire_locked' not in rx
    clip=(ROOT/'src/mac/clipboard.m').read_text(encoding='utf-8')
    assert 'send_image_data_async(imageType, cc, previous_change)' in clip
    assert 'budget_deferred = 1' in clip
    assert 's_last_change = previous_change' in clip
    assert 'image budget released; same clipboard version will be retried' in clip
    # Intrinsically oversized decoded images are permanent failures, while
    # contention for a feasible allocation remains retryable.
    image_send=extract(clip,'send_image')
    assert image_send.index('image_conversion_fits_budget(') < image_send.index('budget_deferred = 1')
    assert 'statusbar_notify_image_limit()' in image_send
    image_limits_preamble=r'''
#include "xfer_budget.h"
#include <assert.h>
#include <stdio.h>
'''
    image_limits_main=r'''
int main(void){
    hop_xfer_budget_limits limits={0};limits.max_image_bytes=100;limits.max_image_temp_bytes=80;
    assert(image_conversion_fits_budget(20,60,&limits));
    assert(!image_conversion_fits_budget(20,61,&limits));
    limits.max_image_temp_bytes=200;
    assert(image_conversion_fits_budget(20,80,&limits));
    assert(!image_conversion_fits_budget(20,81,&limits));
    assert(!image_conversion_fits_budget(UINT64_MAX,1,&limits));
    assert(!image_conversion_fits_budget(1,1,NULL));
    puts("PASS: intrinsic image budget rejects permanent oversize, retains feasible retries");return 0;
}
'''
    run('image_budget_permanent',image_limits_preamble+extract(clip,'image_conversion_fits_budget')+image_limits_main)

    for platform in ['win','mac']:
        rx_sources='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc'))
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8').replace('#include "xfer_receive.inc"', rx_sources)
        common=r'''
#include "hop.h"
#include "xfer_rx_batch_rules.h"
#include "xfer_rx_batch_state.h"
#include "xfer_rx_image_state.h"
#include "xfer_rx_image_buffer.h"
#include "xfer_rx_file_state.h"
#include "xfer_tracking.h"
#include "xfer_budget.h"
#include "xfer_task.h"
#include <assert.h>
#include <wchar.h>
#include <string.h>
#define hop_log(...) ((void)0)
#define HOP_MSG_XFER_DONE 54
#define RX_CLOSE 1
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
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
static struct {hop_xfer_rx_batch_state batch_state;int ntop;char *dir;hop_xfer_rx_image_buffer image_buffer;hop_xfer_rx_image_state image_state;hop_xfer_rx_file_state file_state;void **tops;hop_xfer_task task;} s_rx;
static int send_small(int fd,uint8_t type,const uint8_t*p,uint32_t n,uint32_t batch){(void)batch;(void)fd;(void)type;assert(n==20);memcpy(wire,p,n);return 1;}
static void wq_push(rx_task*t){(void)t;}
static void wq_drain(void){}
static void rx_reset(void){hop_xfer_rx_batch_state_reset(&s_rx.batch_state);hop_xfer_task_dispose(&s_rx.task);}
static void rx_terminate(uint32_t batch,uint8_t reason,int notify){(void)batch;(void)reason;(void)notify;assert(!"unexpected cancellation in successful fixture");}
static void status_file_phase(int d,uint32_t b,int i,int p,uint32_t a){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_phase(&s_receive_status,b,i,p,a);}
static void status_end(int d,uint32_t b,int p,const char*s){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_end(&s_receive_status,b,p,s);}
static void apply_image(const uint8_t*p,int w,int h,int stride){(void)p;(void)w;(void)h;(void)stride;applied++;}
static void apply_paths(const void*p,int n){(void)p;(void)n;}
static uint64_t rate_now_ns(void){static uint64_t now=100;return ++now;}
static void status_stage_time(int direction,uint32_t batch,int stage,uint64_t elapsed_ns){
    (void)batch;(void)stage;(void)elapsed_ns;assert(direction==HOP_XFER_DIR_RECEIVE);
}
static int paste_success_calls,paste_failure_calls;
static void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success){
    (void)batch;(void)paths;(void)n;
    if(success)paste_success_calls++;else paste_failure_calls++;
}
static void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success){
    (void)batch;(void)paths;(void)n;
    if(success)paste_success_calls++;else paste_failure_calls++;
}
#define clip_apply_remote_image apply_image
#define clipboard_apply_image apply_image
#define clip_apply_remote_paths apply_paths
#define clipboard_apply_paths apply_paths
'''
        main=r'''
int main(void){
    /* Both platform callbacks are linked in each host compiler's fixture. */
    (void)&clip_complete_remote_paste;
    (void)&clipboard_complete_remote_paste;
    xf_job j={0};j.batch=10;j.is_image=1;j.ok=1;assert(send_done(0,&j));
    assert(hop_get_be32(wire+4)==1 && hop_get_be32(wire+8)==1);
    uint8_t pixel[4]={0};
    assert(hop_xfer_task_init(&s_rx.task,0,0,j.batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,j.batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,1,0);
    s_rx.image_buffer.data=pixel;s_rx.image_buffer.capacity=4;assert(hop_xfer_rx_image_state_begin(&s_rx.image_state,1,1,4,4));s_rx.image_state.received=4;s_rx.image_state.capacity=4;
    hop_xfer_tracking_begin(&s_receive_status,j.batch,HOP_XFER_DIR_RECEIVE,0,0,"Image",HOP_XFER_RECEIVING);
    hop_xfer_tracking_file(&s_receive_status,j.batch,1,"Image",4,0,HOP_XFER_FILE_RECEIVING);
    rx_finish_batch(j.batch,hop_get_be32(wire+4),hop_get_be32(wire+8));
    assert(applied==1 && !s_receive_status.status.active && s_receive_status.status.phase==HOP_XFER_COMPLETED);
    assert(s_receive_status.status.files_done==1);
    rx_finish_batch(j.batch,1,1);assert(applied==1);
    assert(hop_xfer_task_init(&s_rx.task,0,0,11));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,11));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,1,0);assert(hop_xfer_rx_image_state_begin(&s_rx.image_state,1,1,4,4));s_rx.image_state.received=3;s_rx.image_state.capacity=4;
    hop_xfer_tracking_begin(&s_receive_status,11,HOP_XFER_DIR_RECEIVE,1,4,"Image",HOP_XFER_RECEIVING);
    rx_finish_batch(11,1,1);assert(applied==1 && s_receive_status.status.phase==HOP_XFER_FAILED);

    /* Requested file paste must use the validated callback, not generic paths. */
    const char *one_path="received.txt";void *tops[1]={(void *)one_path};
    uint32_t paste_batch=HOP_XFER_PASTE_BATCH_FLAG|12u;
    assert(hop_xfer_task_init(&s_rx.task,0,0,paste_batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,paste_batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,0,0);s_rx.ntop=1;s_rx.tops=tops;
    hop_xfer_tracking_begin(&s_receive_status,paste_batch,HOP_XFER_DIR_RECEIVE,1,0,"File",HOP_XFER_RECEIVING);
    rx_finish_batch(paste_batch,1,1);
    assert(paste_success_calls==1 && paste_failure_calls==0);

    paste_batch=HOP_XFER_PASTE_BATCH_FLAG|13u;
    assert(hop_xfer_task_init(&s_rx.task,0,0,paste_batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,paste_batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,0,0);s_rx.ntop=1;s_rx.tops=tops;
    hop_xfer_tracking_begin(&s_receive_status,paste_batch,HOP_XFER_DIR_RECEIVE,1,0,"File",HOP_XFER_RECEIVING);
    rx_finish_batch(paste_batch,0,1);
    assert(paste_success_calls==1 && paste_failure_calls==1);
    puts("PASS: production IMAGE DONE and validated file-paste completion paths");return 0;
}
'''
        run('image_'+platform,common+extract(source,'send_done')+'\n'+extract(source,'rx_finish_batch')+main,['src/core/protocol.c','src/core/xfer_budget.c','src/core/xfer_task.c','src/core/xfer_tracking.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_file_state.c','src/core/xfer_validate.c','src/core/crc32.c','src/core/xfer_rx_image_buffer.c','src/core/xfer_rx_protocol_adapter.c','src/core/xfer_rx_dispatch.c'],threads=True)
    # Run the actual per-platform state wrappers, not only a reimplemented model.
    for platform in ['win','mac']:
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8').replace('#include "xfer_receive.inc"', '\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc')))
        preamble=r'''
#include "hop.h"
#include "xfer_tracking.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#define AcquireSRWLockExclusive(p) ((void)(p))
#define ReleaseSRWLockExclusive(p) ((void)(p))
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static int s_status_lock, saves;
static uint64_t s_status_revision;
static uint64_t s_test_monotonic_ms=1000;
uint64_t hop_monotonic_ms(void){return ++s_test_monotonic_ms;}
uint64_t hop_wallclock_ms(void){return 1700000000000ull+s_test_monotonic_ms;}
static hop_xfer_tracking s_send_status,s_receive_status;
static void *s_retry_paths[HOP_XFER_FILE_LIMIT];
static hop_xfer_history_entry s_history[HOP_XFER_HISTORY_LIMIT];static int s_history_count;
static void history_save(void){saves++;}
void hop_log(hop_log_level lvl,const char *fmt,...){(void)lvl;(void)fmt;}
'''
        main=r'''
int main(void){
    status_begin(1,HOP_XFER_DIR_SEND,1,100,"out",HOP_XFER_SENDING);
    status_begin(1,HOP_XFER_DIR_RECEIVE,0,0,"in",HOP_XFER_RECEIVING);
    status_add_received_file(1,1,"in",4,0);
    status_chunk_timed(HOP_XFER_DIR_RECEIVE,1,1,0,4,0,2,0,3);
    assert(s_receive_status.stage_ns[HOP_XFER_TIME_CRC]==2);
    assert(s_receive_status.stage_ns[HOP_XFER_TIME_RECEIVE]==3);
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
        names=['status_for','status_begin','status_chunk_timed','status_file_phase','status_end','status_add_received_file']
        run('wrappers_'+platform,preamble+'\n'.join(extract(source,n) for n in names)+main,['src/core/xfer_tracking.c'])

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
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc'))
        state_start=receiver.index('typedef struct xfer_rx_context {',receiver.index('/* ---------------- 接收'))
        state_end=receiver.index('static xfer_rx_context s_rx;',state_start)+len('static xfer_rx_context s_rx;')
        dispatch_names=['image_begin','file_chunk','file_ack','abort_message','done','done_ack']
        dispatch_handlers='\n'.join(extract(receiver,'rx_dispatch_'+name) for name in dispatch_names)
        dispatch_handlers+='\nstatic const hop_xfer_rx_dispatch_handlers s_rx_dispatch_handlers = { NULL, rx_dispatch_image_begin, rx_dispatch_file_chunk, NULL, rx_dispatch_file_ack, rx_dispatch_abort_message, rx_dispatch_done, rx_dispatch_done_ack };\n'
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
            'BIND_SESSION':extract(receiver,'rx_bind_session'),
            'POLL_CANCEL':extract(receiver,'rx_poll_cancel'),
            'BUSY':extract(sender,'xfer_busy'),
            'DISPATCH_HANDLERS':dispatch_handlers,
        }
        for key,value in values.items():fixture=fixture.replace('@'+key+'@',value)
        assert '@' not in fixture
        assert 'if (!rx->batch_state.active || rx->batch_state.batch != batch) return;' in extract(receiver,'rx_dispatch_file_begin')
        for handler_name in ['file_begin','image_begin','file_chunk','file_end','file_ack','abort_message','done','done_ack']:
            handler=extract(receiver,'rx_dispatch_'+handler_name)
            assert 'xfer_rx_context *rx = (xfer_rx_context *)context;' in handler
            assert 's_rx.' not in handler and 's_x.' not in handler and 's_ack.' not in handler, f'{platform} {handler_name} still reads platform globals'
        assert 'rx_begin_batch(batch)' in extract(receiver,'rx_dispatch_image_begin')
        if platform=='win':fixture='#define FIXTURE_WINDOWS 1\n'+fixture
        run('abort_'+platform,fixture,['src/core/protocol.c','src/core/crc32.c','src/core/xfer_validate.c','src/core/xfer_budget.c','src/core/xfer_task.c','src/core/xfer_tracking.c','src/core/xfer_rx_receipt.c','src/core/xfer_rx_lifecycle.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_dispatch.c','src/core/xfer_rx_entry_reservations.c','src/core/xfer_rx_file_state.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_image_buffer.c'],threads=True)

    inject=(ROOT/'src/mac/inject.c').read_text(encoding='utf-8')
    fixture=(ROOT/'tests/inject_release_fixture.c.in').read_text(encoding='utf-8')
    names=['recompute_flags','mod_index','post','inject_release_all','inject_reset','inject_key','inject_button']
    fixture=fixture.replace('@FUNCTIONS@','\n'.join(extract(inject,n) for n in names))
    run('inject_release',fixture,['src/core/keymap.c'])

    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc'))
        fixture=(ROOT/'tests/session_transport_fixture.c.in').read_text(encoding='utf-8')
        functions='\n'.join([extract(sender,'job_bind_session'),extract(sender,'send_small'),extract(sender,'send_chunk'),extract(receiver,'rx_bind_session'),extract(receiver,'rx_expire_stale_session'),extract(receiver,'on_data_frame_checked'),extract(receiver,'on_data_frame')])
        fixture=fixture.replace('@FUNCTIONS@',functions)
        run('session_transport_'+platform,fixture,['src/core/protocol.c','src/core/session.c','src/core/clock.c','src/core/watchdog.c','src/core/update_state.c','src/core/xfer_rate.c','src/core/xfer_rx_receipt.c','src/core/xfer_rx_lifecycle.c','src/core/xfer_rx_batch_state.c'],threads=True)

    control=(ROOT/'src/mac/net_client.c').read_text(encoding='utf-8')
    dispatch=extract(control,'dispatch_message')
    dispatch=dispatch.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});','mac_capture_peer_geometry(pw,ph);')
    dispatch=dispatch.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});','mac_capture_return();')
    assert 'dispatch_async' not in dispatch
    fixture=(ROOT/'tests/dual_role_control_fixture.c.in').read_text(encoding='utf-8').replace('@DISPATCH@',dispatch)
    run('dual_role_control',fixture,['src/core/protocol.c','src/core/session.c','src/core/clock.c','src/core/xfer_validate.c','src/core/crc32.c'],threads=True)

    # An unconfirmed ACK must force a fresh data channel before the same
    # batch/file ID is reused; the unresolved file remains the resume point.
    for platform in ['mac','win']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_batch_lifecycle.inc','xfer_rx_protocol_handlers.inc'))
        entry=extract(sender,'send_file_entry')
        assert 'tx + CHUNK_HDR' in entry
        assert 'malloc(blen)' not in entry and 'free(bp)' not in entry
        job=extract(sender,'run_job')
        assert 'int acknowledged;' in job
        assert 'if (st >= 0) item->acknowledged = 1;' in job
        assert 'if (st != XFER_ACK_PENDING)' in job
        assert 'fill_result = JOB_NET_DOWN;' in job
        assert 'if (!item->acknowledged)' in job
        assert 'hop_xfer_ack_window_advance_checkpoint(&j->next_i,item->index,1)' in job
        timeout=job.index('if (st == XFER_ACK_TIMEOUT)')
        unregister=job.index('ack_unregister_batch(j->batch)',timeout)
        kick=job.index('xfer_kick();',timeout)
        net_down=job.index('return JOB_NET_DOWN;',kick)
        assert timeout < unregister < kick < net_down
        kick_fn=extract(sender,'xfer_kick')
        assert 'peer_ready = 0' in kick_fn or 'peer_ready=0' in kick_fn

        begin=extract(receiver,'rx_dispatch_file_begin')
        end=extract(receiver,'rx_dispatch_file_end')
        file_ack_handler=extract(receiver,'rx_dispatch_file_ack')
        done_ack_handler=extract(receiver,'rx_dispatch_done_ack')
        assert 'hop_xfer_rx_handle_file_ack(p, len, rx_ack_deliver_file, NULL)' in file_ack_handler
        assert 'hop_xfer_rx_handle_done_ack(p, len, rx_ack_deliver_done, NULL)' in done_ack_handler
        shared_ack=(ROOT/'src/core/xfer_rx_ack_handler.c').read_text(encoding='utf-8')
        assert 'ack.id < 1u' in shared_ack and 'ack.id > HOP_XFER_FILE_LIMIT' in shared_ack
        assert 'status > HOP_DONE_ACK_FAILED' in shared_ack
        lifecycle=extract(receiver,'rx_begin_batch')
        assert 'hop_xfer_rx_fs_mkdir_p(s_rx.dir)' in lifecycle if platform=='mac' else 'hop_xfer_rx_fs_ensure_dirs(s_rx.dir)' in lifecycle
        assert 'rx_remove_top(s_rx.cur_path)' in begin
        assert '!rx_add_top(path)' in begin or '!rx_add_top(s_rx.cur_path)' in end
        if platform=='mac':
            assert 'hop_xfer_rx_fs_mkdir_p(path) != 0' in begin
            fs=(ROOT/'src/mac/xfer_rx_fs.c').read_text(encoding='utf-8')
            assert 'hop_xfer_rx_fs_path_for' in fs and 'hop_xfer_rx_fs_remove_tree' in fs and 'hop_xfer_rx_fs_remove_file' in fs
            assert 'if (!parent_ok)' in begin
        else:
            assert 'hop_xfer_rx_fs_create_directory_if_missing(path)' in begin
            assert 'hop_xfer_rx_fs_ensure_dirs(path)' in begin
            assert 'if (!parent_ok)' in begin
            fs=(ROOT/'src/win/xfer_rx_fs.c').read_text(encoding='utf-8')
            assert 'GetFileAttributesW(path)' in fs
            assert 'hop_xfer_rx_fs_path_for' in fs and 'hop_xfer_rx_fs_remove_tree' in fs
        close=end.index('rx_close_file()')
        cleanup=end.index('hop_xfer_rx_fs_remove_file(')
        assert close < cleanup, 'failed receive cleanup must wait for writer close'

        if platform=='mac':
            queue=extract(sender,'send_thread')
            assert 'peer_ready_snapshot' in queue and 'waited_for_queue' in queue
            assert '!waited_for_queue && peer_ready_snapshot' in queue
        else:
            queue=extract(sender,'send_thread')
            assert 'c != INVALID_SOCKET && cur->retry_at <= now' in queue

    # Exercise the exact completion-ACK loop from both platform senders with
    # deterministic clocks, dropped ACK, reconnect, session replacement, and abort.
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        source=(ROOT/'tests/done_ack_wait_fixture.c.in').read_text(encoding='utf-8')
        source=source.replace('@FUNCTION@',extract(sender,'send_done_result'))
        if platform=='win':source='#define FIXTURE_WINDOWS 1\n'+source
        run('done_ack_'+platform,source,['src/core/protocol.c','src/core/xfer_tracking.c'])
        thread=extract(sender,'send_thread')
        assert 'send_done_result(' in thread and 'HOP_XFER_TASK_INTERRUPTED' in thread
        assert 'Completion delivery uncertain' in thread and 'completion_path' in thread

    # Test exact Windows task-list selection and action logic with UI doubles.
    # Rows may move/disappear between refreshes; only the selected Task ID may
    # receive cancellation, including after a reordered snapshot.
    ui=(ROOT/'src/win/main_ui.c').read_text(encoding='utf-8')
    common=r"""
#include "xfer_task.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#define UI_TASK_CAP HOP_TASK_MANAGER_MAX_LIVE
#define LB_ERR (-1)
#define LB_ERRSPACE (-2)
#define LB_GETCURSEL 1
#define LB_RESETCONTENT 2
#define LB_ADDSTRING 3
#define LB_SETCURSEL 4
#define MB_OK 0
#define MB_YESNO 1
#define MB_ICONINFORMATION 2
#define MB_ICONWARNING 4
#define IDYES 6
typedef intptr_t LPARAM;typedef uintptr_t WPARAM;typedef intptr_t LRESULT;
typedef struct {int count,selected;} list;typedef list* HWND;
static list lists[2]={{0,-1},{0,-1}};
static HWND s_task_lists[2]={&lists[0],&lists[1]},s_main;
static hop_xfer_task_id s_task_ids[2][UI_TASK_CAP];static int s_task_counts[2];
static hop_xfer_task_snapshot *s_tasks_snapshot;
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
static int connected=1,answer=IDYES,calls;
static LRESULT SendMessageW(HWND h,int op,WPARAM w,LPARAM l){(void)l;
    if(op==LB_GETCURSEL)return h->selected;
    if(op==LB_RESETCONTENT){h->count=0;h->selected=-1;return 0;}
    if(op==LB_ADDSTRING)return h->count++;
    if(op==LB_SETCURSEL){h->selected=(int)w;return 0;}
    assert(0);return LB_ERR;
}
static int MessageBoxW(HWND h,const wchar_t *text,const wchar_t *title,int flags){(void)h;(void)text;(void)title;(void)flags;return answer;}
static int xfer_connected(void){return connected;}
static int xfer_cancel_task(hop_xfer_task_id id){calls++;return hop_xfer_task_request_cancel(id);}
static void refresh_transfers(void){}
"""
    main=r"""
int main(void){
 hop_xfer_task a={0},b={0},c={0},rx={0};
 assert(hop_xfer_task_init_classified(&a,0,0,1,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
 assert(hop_xfer_task_init_classified(&b,0,0,2,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
 assert(hop_xfer_task_init_classified(&rx,0,0,2,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
 refresh_task_lists();assert(s_task_counts[0]==2&&s_task_counts[1]==1);
 for(int i=0;i<s_task_counts[0];i++)if(s_task_ids[0][i]==a.id)lists[0].selected=i;
 assert(selected_task(0)==a.id);
 assert(hop_xfer_task_init_classified(&c,0,0,3,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
 refresh_task_lists();assert(selected_task(0)==a.id);
 connected=0;answer=0;cancel_selected_task(0);assert(!calls && !hop_xfer_task_cancel_requested(a.id));
 answer=IDYES;cancel_selected_task(0);assert(calls==1 && hop_xfer_task_cancel_requested(a.id));
 assert(!hop_xfer_task_cancel_requested(b.id) && !hop_xfer_task_cancel_requested(c.id) && !hop_xfer_task_cancel_requested(rx.id));
 hop_xfer_task_dispose(&a);refresh_task_lists();assert(!selected_task(0));
 cancel_selected_task(0);assert(calls==1);
 hop_xfer_task_dispose(&b);hop_xfer_task_dispose(&c);hop_xfer_task_dispose(&rx);free(s_tasks_snapshot);
 puts("PASS: task list preserves Task ID through reordering and cancels only selection");return 0;
}
"""
    code=common+'\n'.join(extract(ui,n) for n in ['selected_task','task_state_name','refresh_task_lists','cancel_selected_task'])+main
    run('task_list_selection_win',code,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    # Manager-owned disconnect timer must be receive-only, monotonic, and reset
    # by reconnect (including clock rollback); no reservation is independently
    # released until the task owner terminates the task.
    timer_fixture=r"""
#include "xfer_task.h"
#include <assert.h>
#include <stdio.h>
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
int main(void) {
 hop_xfer_task rx={0},tx={0};hop_xfer_task_audit audit;
 assert(hop_xfer_task_init_classified(&rx,0,0,17,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
 assert(hop_xfer_task_init_classified(&tx,0,0,17,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
 assert(hop_xfer_task_reserve_job(&rx)&&hop_xfer_task_reserve_job(&tx));
 assert(hop_xfer_task_set_state(&rx,HOP_XFER_TASK_ACTIVE)&&hop_xfer_task_set_state(&tx,HOP_XFER_TASK_ACTIVE));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,0,100));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,99,100));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,1,101,100));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,150,100));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,140,100));
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,239,100));
 assert(hop_xfer_task_rx_disconnect_expired(&rx,0,240,100));
 assert(!hop_xfer_task_request_cancel(rx.id)); /* expiry claimed first */
 assert(!hop_xfer_task_rx_disconnect_expired(&tx,0,1000,1));
 assert(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.jobs==2);
 assert(hop_xfer_task_finish(&rx,HOP_XFER_TASK_INTERRUPTED));
 assert(hop_xfer_task_reconcile(&audit) && !audit.mismatches && audit.jobs==1);
 assert(!hop_xfer_task_rx_disconnect_expired(&rx,0,500,1));
 assert(hop_xfer_task_finish(&tx,HOP_XFER_TASK_COMPLETED));
 hop_xfer_task_dispose(&rx);hop_xfer_task_dispose(&tx);
 assert(hop_xfer_task_reconcile(&audit) && !audit.mismatches && !audit.jobs);
 puts("PASS: manager-owned receiver timeout, reconnect reset, clock rollback and reservations");return 0;
}
"""
    run('task_disconnect_timer',timer_fixture,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    run('task_history',(ROOT/'tests/task_history_tests.c').read_text(encoding='utf-8'),['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    history_ui=(ROOT/'tests/task_history_ui_fixture.c.in').read_text(encoding='utf-8')
    history_ui=history_ui.replace('@UI_FUNCTIONS@',extract(ui,'task_state_name')+'\n'+extract(ui,'refresh_task_history'))
    run('task_history_ui_win',history_ui,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    # A paste request cannot be replayed as a generic transfer: that would
    # discard destination/version context and may overwrite the peer clipboard.
    retry_fixture=(ROOT/'tests/retry_guard_fixture.c.in').read_text(encoding='utf-8')
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        fixture=retry_fixture.replace('@RETRY@',extract(sender,'xfer_retry_file'))
        if platform=='win':fixture='#define FIXTURE_WINDOWS 1\n'+fixture
        run('retry_guard_'+platform,fixture,[])

    # Exercise the production queue removal helper with manager-owned IDs.
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        common=r"""
#include "xfer_task.h"
#include <assert.h>
#include <stdio.h>
typedef struct job { struct job *next; hop_xfer_task task; } job;
typedef job xfer_job; typedef job xf_job;
static struct { job *q_head,*q_tail; } s_x;
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
"""
        main=r"""
int main(void){
    job a={0}, b={0}, c={0};
    assert(hop_xfer_task_init_classified(&a.task,0,0,1,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    assert(hop_xfer_task_init_classified(&b.task,0,0,2,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    assert(hop_xfer_task_init_classified(&c.task,0,0,3,HOP_XFER_DIR_SEND,HOP_TASK_KIND_FILES));
    assert(hop_xfer_task_reserve_job(&a.task)); assert(hop_xfer_task_reserve_job(&b.task)); assert(hop_xfer_task_reserve_job(&c.task));
    a.next=&b; b.next=&c; s_x.q_head=&a; s_x.q_tail=&c;
    assert(hop_xfer_task_request_cancel(b.task.id));
    assert(queue_take_cancelled()==&b && !b.next);
    assert(s_x.q_head==&a && a.next==&c && s_x.q_tail==&c);
    assert(!hop_xfer_task_cancel_requested(a.task.id) && !hop_xfer_task_cancel_requested(c.task.id));
    assert(hop_xfer_task_finish(&b.task,HOP_XFER_TASK_CANCELLED)); hop_xfer_task_dispose(&b.task);
    assert(hop_xfer_task_request_cancel(c.task.id));
    assert(queue_take_cancelled()==&c && s_x.q_tail==&a && !a.next);
    assert(hop_xfer_task_finish(&c.task,HOP_XFER_TASK_CANCELLED)); hop_xfer_task_dispose(&c.task);
    assert(hop_xfer_task_request_cancel(a.task.id));
    assert(queue_take_cancelled()==&a && !s_x.q_head && !s_x.q_tail);
    assert(hop_xfer_task_finish(&a.task,HOP_XFER_TASK_CANCELLED)); hop_xfer_task_dispose(&a.task);
    hop_xfer_budget_snapshot budget;hop_xfer_budget_get_snapshot(&budget);assert(!budget.jobs);
    puts("PASS: production queue cancels only selected task, preserving links and reservations");return 0;
}
"""
        code=common+extract(sender,'queue_unlink')+'\n'+extract(sender,'queue_take_cancelled')+main
        run('task_cancel_queue_'+platform,code,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    # hop_xfer_tracking embeds 8192 file records with 1024-byte paths
    # (~8 MiB). Keep these fixtures off Windows' default 1 MiB thread stack.
    core_tests=(ROOT/'tests/core_tests.c').read_text(encoding='utf-8')
    for name in ['test_xfer_tracking_stage_timing','test_xfer_tracking_speed_and_eta']:
        body=extract(core_tests,name)
        assert not re.search(r'(?m)^\s*(?:const\s+)?hop_xfer_tracking\s+\w+\s*;',body), (
            name+' must not place the multi-megabyte tracking object on the stack'
        )

    print('All targeted regressions passed.',flush=True)
