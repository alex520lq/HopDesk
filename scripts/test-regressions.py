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
    def run(name,source,extra=(),threads=False,libraries=()):
        path=work/(name+'.c');path.write_text(source,encoding='utf-8')
        exe=work/(name+('.exe' if os.name=='nt' else ''))
        if os.name=='nt' and shutil.which('cl'):
            cmd=['cl','/nologo','/std:c11','/W4','/utf-8','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'src/core'),'/I'+str(ROOT/'src/win'),'/Fe:'+str(exe),str(path)]+[str(ROOT/x) for x in extra]+list(libraries)
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

    # Exercise the real Windows filesystem adapter with the exact staging
    # path shape produced by xfer_receive.inc, not a generic "HopDesk" prefix.
    if os.name == 'nt':
        run('win_rx_fs_staging_cleanup',r"""
 #include "xfer_rx_fs.h"
 #include <assert.h>
 #include <stdio.h>
 #include <windows.h>
 int main(void) {
     wchar_t temp[MAX_PATH], root[MAX_PATH], session[MAX_PATH], batch[MAX_PATH], file[MAX_PATH];
     DWORD n=GetTempPathW(MAX_PATH,temp); assert(n>0 && n<MAX_PATH);
     assert(swprintf(root,MAX_PATH,L"%sHopDesk",temp)>0);
     assert(CreateDirectoryW(root,NULL) || GetLastError()==ERROR_ALREADY_EXISTS);
     assert(swprintf(session,MAX_PATH,L"%s\\rx-fs-session",root)>0);
     assert(CreateDirectoryW(session,NULL));
     assert(swprintf(batch,MAX_PATH,L"%s\\batch",session)>0);
     assert(CreateDirectoryW(batch,NULL));
     assert(swprintf(file,MAX_PATH,L"%s\\payload.bin",batch)>0);
     HANDLE h=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
     assert(h!=INVALID_HANDLE_VALUE); CloseHandle(h);
     assert(hop_xfer_rx_fs_remove_tree(batch,L"\\HopDesk\\")==0);
     assert(GetFileAttributesW(batch)==INVALID_FILE_ATTRIBUTES);
     /* Repeating cleanup for an already absent staging tree is idempotent. */
     assert(hop_xfer_rx_fs_remove_tree(batch,L"\\HopDesk\\")==0);
     /* The previous over-escaped fragment must still be rejected by the guard. */
     assert(hop_xfer_rx_fs_remove_tree(batch,L"\\\\HopDesk\\\\")==-2);
     assert(RemoveDirectoryW(session));
     assert(RemoveDirectoryW(root));
     puts("PASS: Windows staging tree is deleted with the exact production path guard");
     return 0;
 }
        """,['src/win/xfer_rx_fs.c'],libraries=['shell32.lib'])

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


    clip=(ROOT/'src/win/clipboard.c').read_text(encoding='utf-8')+'\n'+(ROOT/'src/win/clipboard_offer.inc').read_text(encoding='utf-8')+'\n'+(ROOT/'src/win/clipboard_paste.inc').read_text(encoding='utf-8')
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
        rx='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c'))
        if platform=='win':
            assert 'clip_complete_remote_paste(batch, (const wchar_t *const *)top_paths, top_count, 1)' in rx
            assert 'clip_complete_remote_paste(batch, NULL, 0, 0)' in rx
            clip=(ROOT/'src/win/clipboard.c').read_text(encoding='utf-8')+'\n'+(ROOT/'src/win/clipboard_offer.inc').read_text(encoding='utf-8')+'\n'+(ROOT/'src/win/clipboard_paste.inc').read_text(encoding='utf-8')
            assert 'GetClipboardSequenceNumber()!=expected' in clip
            paste_message=extract(clip,'clip_on_paste_message')
            assert 'SendInput(' not in paste_message
            assert 'apply_global_expected(CF_HDROP,h,expected_sequence,1,1)' in paste_message
        else:
            assert 'clipboard_complete_remote_paste(batch, (const char *const *)top_paths, top_count, 1)' in rx
            assert 'clipboard_complete_remote_paste(batch, NULL, 0, 0)' in rx
            clip=(ROOT/'src/mac/clipboard.m').read_text(encoding='utf-8')+'\n'+(ROOT/'src/mac/clipboard_offer.inc').read_text(encoding='utf-8')+'\n'+(ROOT/'src/mac/clipboard_paste.inc').read_text(encoding='utf-8')
            assert '[pb changeCount] != expected_count' in clip
            apply_objects=extract(clip,'apply_objects_retry')
            assert 'inject_paste_shortcut()' not in apply_objects
            assert 'statusbar_notify_paste_ready()' in apply_objects

        platform_handlers=(ROOT/f'src/{platform}/xfer_rx_protocol_handlers.c').read_text(encoding='utf-8')
        core_handlers=(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        begin=extract(core_handlers,'hop_xfer_rx_handle_file_begin')
        end=extract(core_handlers,'hop_xfer_rx_handle_file_end')
        assert 'if (rx->batch_state.active && hop_xfer_task_cancel_requested(rx->task.id))' in begin
        assert 'if (!hop_xfer_task_set_kind(&rx->task, HOP_TASK_KIND_FILES)' in begin
        assert 'hop_xfer_rx_file_state_prepare_entry(&rx->file_state, id, size,' in begin
        begin_reserve=begin.index('hop_xfer_rx_entry_reservations_reserve(')
        begin_commit=begin.index('rx->cur_path = (hop_xfer_rx_path_char *)candidate')
        assert begin_reserve < begin_commit
        for side_effect in ['rx->services.reset_file_error(', 'rx->services.remove_top_path(', 'rx->services.status_add_received_file(', 'rx->services.prepare_file_path(']:
            assert begin.index(side_effect) > begin_reserve
        assert 'rx->services.open_file(' in begin
        assert 'hop_xfer_rx_file_state_evaluate_end' in end
        assert 'rx->services.close_file(' in end and 'rx->services.send_file_ack(' in end
        assert 'rx->services.remove_file(' in end and 'rx->services.is_top_level_path(' in end
        assert 'rx_dispatch_file_begin' not in platform_handlers and 'rx_dispatch_file_end' not in platform_handlers
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
        assert 'queue->callbacks.task_cancelled' in writer
        assert 'if (!cancelled) run_command(queue, command);' in writer
    shared_rx_handlers=(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
    file_state_tests=(ROOT/'tests/xfer_rx_file_state_tests.c').read_text(encoding='utf-8')
    assert 'hop_xfer_rx_file_state_prepare_entry' in file_state_tests
    assert 'hop_xfer_rx_file_state_evaluate_end' in file_state_tests
    assert 'HOP_RX_FILE_END_INCOMPLETE' in file_state_tests
    assert 'HOP_FILE_ACK_BADPATH' in file_state_tests
    assert 'if (rx->batch_state.active && hop_xfer_task_cancel_requested(rx->task.id))' in shared_rx_handlers
    assert 'if (!hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_IMAGE) || hop_xfer_task_cancel_requested(rx->task.id))' in shared_rx_handlers
    assert 'if (!hop_xfer_decode_image_begin(p, len, &msg))' in shared_rx_handlers
    assert 'if (!rx->batch_state.active) return; /* late data after termination */' in shared_rx_handlers
    assert 'if (hop_xfer_task_cancel_requested(rx->task.id))' in shared_rx_handlers
    assert 'if (!hop_xfer_decode_image_begin(p, len, &msg))' in shared_rx_handlers
    assert 'if (!rx->batch_state.active) return; /* late data after termination */' in shared_rx_handlers
    assert 'if (hop_xfer_task_cancel_requested(rx->task.id))' in shared_rx_handlers
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
        rx_sources='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c'))
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')
        source=source.replace('#include "xfer_receive.inc"', rx_sources)
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
#include "xfer_rx_services.h"
#include "xfer_rx_context.h"
#include <assert.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
static void hop_xfer_rx_terminate_batch(void *context,uint32_t batch,uint8_t reason,int notify_peer){(void)context;(void)batch;(void)reason;(void)notify_peer;}
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
static xf_job j={10,1,1,1};
static int applied;static uint8_t wire[20];
static hop_xfer_tracking s_receive_status;
static xfer_rx_context s_rx;
static int send_small(int fd,uint8_t type,const uint8_t*p,uint32_t n,uint32_t batch){(void)batch;(void)fd;(void)type;assert(n==20);memcpy(wire,p,n);return 1;}
static void wq_drain(void){}
static void status_file_phase(int d,uint32_t b,int i,int p,uint32_t a){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_phase(&s_receive_status,b,i,p,a);}
static void status_end(int d,uint32_t b,int p,const char*s){assert(d==HOP_XFER_DIR_RECEIVE);hop_xfer_tracking_end(&s_receive_status,b,p,s);}
static void apply_image(const uint8_t*p,int w,int h,int stride){(void)p;(void)w;(void)h;(void)stride;applied++;}
static void apply_paths(const void*p,int n){(void)p;(void)n;}
static void fixture_close_file(void *context){(void)context;wq_drain();}
static void fixture_status_end(void *context,uint32_t batch,int phase,const char *detail){(void)context;status_end(HOP_XFER_DIR_RECEIVE,batch,phase,detail);}
static void fixture_status_file_phase(void *context,uint32_t batch,int index,int phase,uint32_t attempts){(void)context;status_file_phase(HOP_XFER_DIR_RECEIVE,batch,index,phase,attempts);}
static void fixture_status_total(void *context,uint32_t total){(void)context;(void)total;}
static void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success);
static void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success);
static void fixture_finish_clipboard(void *context,uint32_t batch,const void *image_data,uint32_t width,uint32_t height,uint32_t stride,int image_valid,const void *const *paths,int count,int allow_publish){
    (void)context;
    if(allow_publish && image_valid && image_data){apply_image((const uint8_t *)image_data,(int)width,(int)height,(int)stride);return;}
    if(allow_publish && count>0){
        if(batch&HOP_XFER_PASTE_BATCH_FLAG){
#ifdef FIXTURE_WINDOWS
            clip_complete_remote_paste(batch,(const wchar_t *const *)paths,count,1);
#else
            clipboard_complete_remote_paste(batch,(const char *const *)paths,count,1);
#endif
        } else apply_paths(paths,count);
    } else if(batch&HOP_XFER_PASTE_BATCH_FLAG){
#ifdef FIXTURE_WINDOWS
        clip_complete_remote_paste(batch,NULL,0,0);
#else
        clipboard_complete_remote_paste(batch,NULL,0,0);
#endif
    }
}
static int paste_success_calls,paste_failure_calls;
static void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success);
static void clipboard_complete_remote_paste(uint32_t batch,const char *const *paths,int n,int success);
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
    s_rx.services.close_file=fixture_close_file;
    s_rx.services.finish_clipboard=fixture_finish_clipboard;
    s_rx.services.status_end=fixture_status_end;
    s_rx.services.status_file_phase=fixture_status_file_phase;
    s_rx.services.status_set_files_total=fixture_status_total;
    /* Both platform callbacks are linked in each host compiler's fixture. */
    (void)&clip_complete_remote_paste;
    (void)&clipboard_complete_remote_paste;
    assert(send_done(0,10,1,1));
    assert(hop_get_be32(wire+4)==1 && hop_get_be32(wire+8)==1);
    uint8_t *pixel=(uint8_t*)malloc(4);assert(pixel);memset(pixel,0,4);
    assert(hop_xfer_task_init(&s_rx.task,0,0,j.batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,j.batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,1,0);
    s_rx.image_buffer.data=pixel;s_rx.image_buffer.capacity=4;assert(hop_xfer_rx_image_state_begin(&s_rx.image_state,1,1,4,4));s_rx.image_state.received=4;s_rx.image_state.capacity=4;
    hop_xfer_tracking_begin(&s_receive_status,j.batch,HOP_XFER_DIR_RECEIVE,0,0,"Image",HOP_XFER_RECEIVING);
    hop_xfer_tracking_file(&s_receive_status,j.batch,1,"Image",4,0,HOP_XFER_FILE_RECEIVING);
    hop_xfer_rx_finish_batch(&s_rx,j.batch,hop_get_be32(wire+4),hop_get_be32(wire+8));
    assert(applied==1 && !s_receive_status.status.active && s_receive_status.status.phase==HOP_XFER_COMPLETED);
    assert(s_receive_status.status.files_done==1);
    hop_xfer_rx_finish_batch(&s_rx,j.batch,1,1);assert(applied==1);
    assert(hop_xfer_task_init(&s_rx.task,0,0,11));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,11));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,1,0);assert(hop_xfer_rx_image_state_begin(&s_rx.image_state,1,1,4,4));s_rx.image_state.received=3;s_rx.image_state.capacity=4;
    hop_xfer_tracking_begin(&s_receive_status,11,HOP_XFER_DIR_RECEIVE,1,4,"Image",HOP_XFER_RECEIVING);
    hop_xfer_rx_finish_batch(&s_rx,11,1,1);assert(applied==1 && s_receive_status.status.phase==HOP_XFER_FAILED);

    /* Requested file paste must use the validated callback, not generic paths. */
#ifdef FIXTURE_WINDOWS
    const wchar_t one_path[]=L"received.txt";
#else
    const char one_path[]="received.txt";
#endif
    s_rx.tops=malloc(sizeof(*s_rx.tops));assert(s_rx.tops);
    s_rx.tops[0]=malloc(sizeof(one_path));assert(s_rx.tops[0]);memcpy(s_rx.tops[0],one_path,sizeof(one_path));s_rx.ntop=1;
    uint32_t paste_batch=HOP_XFER_PASTE_BATCH_FLAG|12u;
    assert(hop_xfer_task_init(&s_rx.task,0,0,paste_batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,paste_batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,0,0);
    hop_xfer_tracking_begin(&s_receive_status,paste_batch,HOP_XFER_DIR_RECEIVE,1,0,"File",HOP_XFER_RECEIVING);
    hop_xfer_rx_finish_batch(&s_rx,paste_batch,1,1);
    assert(paste_success_calls==1 && paste_failure_calls==0);

    paste_batch=HOP_XFER_PASTE_BATCH_FLAG|13u;
    assert(hop_xfer_task_init(&s_rx.task,0,0,paste_batch));
    assert(hop_xfer_task_reserve_job(&s_rx.task));
    assert(hop_xfer_task_set_state(&s_rx.task,HOP_XFER_TASK_ACTIVE));
    assert(hop_xfer_rx_batch_state_begin(&s_rx.batch_state,paste_batch));hop_xfer_rx_file_state_begin(&s_rx.file_state,1);hop_xfer_rx_file_state_set_kind(&s_rx.file_state,0,0);s_rx.tops=malloc(sizeof(*s_rx.tops));assert(s_rx.tops);s_rx.tops[0]=malloc(sizeof(one_path));assert(s_rx.tops[0]);memcpy(s_rx.tops[0],one_path,sizeof(one_path));s_rx.ntop=1;
    hop_xfer_tracking_begin(&s_receive_status,paste_batch,HOP_XFER_DIR_RECEIVE,1,0,"File",HOP_XFER_RECEIVING);
    hop_xfer_rx_finish_batch(&s_rx,paste_batch,0,1);
    assert(paste_success_calls==1 && paste_failure_calls==1);
    puts("PASS: production IMAGE DONE and validated file-paste completion paths");return 0;
}
'''
        if platform=='win': common='#define FIXTURE_WINDOWS 1\n'+common
        sender_source=(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')
        core_finish_source=(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        run('image_'+platform,common+extract(sender_source,'send_done')+'\n'+extract(core_finish_source,'hop_xfer_rx_reset_batch_state')+'\n'+extract(core_finish_source,'rx_release_terminal_resources')+'\n'+extract(core_finish_source,'rx_abort_clear_state')+'\n'+extract(core_finish_source,'hop_xfer_rx_finish_batch')+main,['src/core/protocol.c','src/core/xfer_budget.c','src/core/xfer_task.c','src/core/xfer_tracking.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_file_state.c','src/core/xfer_validate.c','src/core/crc32.c','src/core/xfer_rx_image_buffer.c','src/core/xfer_rx_entry_reservations.c','src/core/xfer_rx_protocol_adapter.c','src/core/xfer_rx_dispatch.c'],threads=True)
    # Run the actual per-platform state wrappers, not only a reimplemented model.
    for platform in ['win','mac']:
        source=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8').replace('#include "xfer_receive.inc"', '\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c')))
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

    for platform in ['win','mac']:
        transport=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        worker=(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')
        submission=(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        assert '#include "xfer_sender_worker.inc"' in transport
        assert '#include "xfer_sender_protocol.inc"' in transport
        assert '#include "xfer_submit.inc"' in transport
        assert 'static void' in worker and 'send_thread' in worker
        assert 'hop_xfer_sender_queue_take_cancelled' in worker and 'hop_xfer_sender_queue_take_head' in worker
        assert 'queue_unlink' not in worker
        protocol=(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')
        assert 'sender_engine_send_entry' in protocol and 'send_done_result' in protocol and 'run_job' in protocol
        assert 'ack_wait' in protocol and 'rate_feedback' in protocol
        assert 'xfer_submit_paths_impl' in submission and 'xfer_submit_image_impl' in submission

        scanner=(ROOT/f'src/{platform}/xfer_scanner.c').read_text(encoding='utf-8')
        scanner_header=(ROOT/f'src/{platform}/xfer_scanner.h').read_text(encoding='utf-8')
        assert 'hop_xfer_scan_paths' in scanner and 'walk_into' in scanner and 'elist_push' in scanner
        assert 'services->reserve_entry' in scanner and 'services->is_cancelled' in scanner
        assert 'services->watchdog_begin' in scanner and 'services->watchdog_end' in scanner
        assert 'hop_xfer_task_' not in scanner and 'hop_watchdog_' not in scanner and 'hop_log(' not in scanner
        assert 'hop_xfer_scan_error' in scanner_header and 'HOP_XFER_SCAN_OUT_OF_MEMORY' in scanner_header
        assert 'hop_xfer_scan_paths(&l, paths, n, &scan_services)' in submission
        assert 'hop_xfer_task task;' in submission and 'hop_xfer_task_dispose(&task)' in submission
        assert 'HOP_XFER_SCAN_MAX_ENTRIES' in scanner_header
        if platform=='win':
            assert 'FILE_ATTRIBUTE_REPARSE_POINT' in scanner and 'FindFirstFileW' in scanner
        else:
            assert 'S_ISLNK' in scanner and 'lstat(' in scanner and 'S_ISREG' in scanner and 'read_error' in scanner and 'errno = 0' in scanner
        assert 'walk_into' not in transport and 'elist_push' not in transport
        assert 'hop_xfer_scan_paths(&l, paths, n, &scan_services)' in submission

    # Exact production case bodies + lifecycle helpers; only OS I/O is doubled.
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c'))
        assert 's_ack' not in receiver
        receiver+='\n'+(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        protocol_handlers=(ROOT/f'src/{platform}/xfer_rx_protocol_handlers.c').read_text(encoding='utf-8')
        context_header=(ROOT/'src/core/xfer_rx_context.h').read_text(encoding='utf-8')
        assert 'typedef struct xfer_rx_context' in context_header
        state_contract='#include "xfer_rx_context.h"\n#include "xfer_rx_message_handlers.h"\nstatic xfer_rx_context s_rx;\nstatic void rx_release_terminal_resources(xfer_rx_context *rx, int close_writer, int discard_staging);\nstatic void rx_abort_clear_state(void *context);'
        dispatch_names=[]
        dispatch_handlers='\n'.join(extract(receiver,name) for name in ['rx_reserve_entry_budget','rx_handler_terminate','rx_handler_begin_batch','rx_handler_finish_batch','rx_ack_deliver_file','rx_ack_deliver_done','rx_now_ns','hop_xfer_rx_bind_session'])+'\n'+'\n'.join(extract(receiver,name) for name in ['hop_xfer_rx_handle_file_begin','hop_xfer_rx_handle_image_begin','hop_xfer_rx_handle_file_chunk','hop_xfer_rx_handle_file_end','hop_xfer_rx_dispatch_file_ack_message','hop_xfer_rx_handle_abort','hop_xfer_rx_handle_done','hop_xfer_rx_dispatch_done_ack_message'])
        dispatch_handlers+='\nstatic const hop_xfer_rx_dispatch_handlers s_rx_dispatch_handlers = { hop_xfer_rx_handle_file_begin, hop_xfer_rx_handle_image_begin, hop_xfer_rx_handle_file_chunk, hop_xfer_rx_handle_file_end, hop_xfer_rx_dispatch_file_ack_message, hop_xfer_rx_handle_abort, hop_xfer_rx_handle_done, hop_xfer_rx_dispatch_done_ack_message };\n'
        fixture=(ROOT/'tests/receive_abort_fixture.c.in').read_text(encoding='utf-8')
        values={
            'RX_STATE':state_contract,
            'WIRE_SEND':extract(sender,'send_abort_msg' if platform=='win' else 'send_abort'),
            'STATUS_FUNCTIONS':'\n'.join(extract(sender,n) for n in ['status_for','status_begin','status_end']),
            'CLEAR_STATE':extract(receiver,'hop_xfer_rx_reset_batch_state'),
            'TERMINATE':'\n'.join(extract(receiver,name) for name in ['rx_abort_send','rx_abort_cancel_clipboard','rx_release_terminal_resources','rx_abort_release_resources','rx_abort_mark_dead','rx_abort_finish_task','rx_abort_clear_state','rx_abort_status_end'])+'\n'+extract(receiver,'hop_xfer_rx_terminate_batch'),
            'BEGIN_BATCH':extract(receiver,'hop_xfer_rx_begin_batch'),
            'FINISH_BATCH':extract(receiver,'hop_xfer_rx_finish_batch'),
            'RX_SERVICE_CALLBACKS':'\n'.join([
                'static void fixture_send_abort(void *context,uint32_t batch,uint8_t reason){(void)context;\n#ifdef FIXTURE_WINDOWS\nsend_abort_msg(s_x.client,batch,reason,HOP_ABORT_TO_SENDER);\n#else\nsend_abort(batch,reason);\n#endif\n}',
                'static int fixture_add_top_path(void *context,const void *path){(void)context;(void)path;return 1;}',
                'static void fixture_remove_top_path(void *context,const void *path){(void)context;(void)path;}',
                'static void fixture_finish_clipboard(void *context,uint32_t batch,const void *image,uint32_t w,uint32_t h,uint32_t stride,int image_valid,const void *const *paths,int count,int allow){(void)context;if(allow&&image_valid&&image){apply_image((const uint8_t*)image,(int)w,(int)h,(int)stride);return;}if(allow&&count>0){if(batch&HOP_XFER_PASTE_BATCH_FLAG){',
                '#ifdef FIXTURE_WINDOWS',
                'clip_complete_remote_paste(batch,(const wchar_t *const *)paths,count,1);',
                '#else',
                'clipboard_complete_remote_paste(batch,(const char *const *)paths,count,1);',
                '#endif',
                '}else apply_paths(paths,count);}else if(batch&HOP_XFER_PASTE_BATCH_FLAG){',
                '#ifdef FIXTURE_WINDOWS',
                'clip_complete_remote_paste(batch,NULL,0,0);',
                '#else',
                'clipboard_complete_remote_paste(batch,NULL,0,0);',
                '#endif',
                '}}',
                'static void fixture_cancel_clipboard(void *context,uint32_t batch){(void)context;if(batch&HOP_XFER_PASTE_BATCH_FLAG){',
                '#ifdef FIXTURE_WINDOWS',
                'clip_complete_remote_paste(batch,NULL,0,0);',
                '#else',
                'clipboard_complete_remote_paste(batch,NULL,0,0);',
                '#endif',
                '}}',
                'static uint64_t fixture_now_ns(void *context){(void)context;return rate_now_ns();}',
                'static int fixture_status_begin(void *context,uint32_t batch,uint32_t files,uint64_t bytes,const char *name,int phase){(void)context;return status_begin(batch,HOP_XFER_DIR_RECEIVE,files,bytes,name,phase);}',
                'static void fixture_status_end(void *context,uint32_t batch,int phase,const char *detail){(void)context;status_end(HOP_XFER_DIR_RECEIVE,batch,phase,detail);}',
                'static void fixture_status_add_file(void *context,uint32_t batch,uint32_t id,const char *path,uint64_t size,int directory){(void)context;hop_xfer_tracking_file(&s_receive_status,batch,id,path,size,directory,HOP_XFER_FILE_RECEIVING);}',
                'static void fixture_status_image(void *context,uint32_t batch,uint64_t size){(void)context;status_image(batch,HOP_XFER_DIR_RECEIVE,size);}',
                'static void fixture_status_stage(void *context,uint32_t batch,int stage,uint64_t elapsed){(void)context;status_stage_time(HOP_XFER_DIR_RECEIVE,batch,stage,elapsed);}',
                'static void fixture_status_chunk(void *context,uint32_t batch,uint32_t id,uint64_t offset,uint32_t length,uint64_t crc,uint64_t receive){(void)context;status_chunk_timed(HOP_XFER_DIR_RECEIVE,batch,id,offset,length,0,crc,0,receive);}',
                'static void fixture_status_file_phase(void *context,uint32_t batch,int index,int phase,uint32_t attempts){(void)context;status_file_phase(HOP_XFER_DIR_RECEIVE,batch,index,phase,attempts);}',
                'static void fixture_status_total(void *context,uint32_t total){(void)context;s_receive_status.status.files_total=total;}',
                'static void fixture_status_reset(void *context){(void)context;memset(&s_receive_status,0,sizeof(s_receive_status));}',
                'static void fixture_clear_cancel_request(void *context){(void)context;}',
                'static int fixture_create_staging_directory(void *context,const char *session_hex,uint32_t batch){xfer_rx_context *rx=(xfer_rx_context*)context;(void)session_hex;(void)batch;if(!rx||fail_directory)return 0;\n#ifdef FIXTURE_WINDOWS\nwcscpy(rx->dir,L"tmp\\\\HopDesk\\\\fixture");\n#else\nstrcpy(rx->dir,"/tmp/HopDesk/fixture");\n#endif\nreturn 1;}',
            ]),
            'RX_WRITER_CALLBACKS':'\n'.join([
                'static void fixture_reset_file_error(void *context){(void)context;}',
                'static int fixture_open_file(void *context,const void *native_path){(void)context;(void)native_path;return 1;}',
                'static int fixture_write_file_chunk(void *context,hop_xfer_task *task,uint64_t offset,const uint8_t *data,uint32_t length){(void)context;if(fail_write)return 0;return wq_push_data(task,offset,data,length);}',
                'static void fixture_close_file(void *context){(void)context;wq_drain();rx_task task;memset(&task,0,sizeof(task));task.kind=RX_CLOSE;(void)wq_push(&task);wq_drain();}',
                'static int fixture_file_error(void *context){(void)context;return fail_writer_io;}'
            ]),
            'CANCEL_LOCAL':extract(receiver,'rx_cancel_local'),
            'BIND_SESSION':extract(receiver,'rx_bind_session'),
            'POLL_CANCEL':extract(receiver,'rx_poll_cancel'),
            'BUSY':extract(sender,'xfer_busy'),
            'DISPATCH_HANDLERS':dispatch_handlers,
        }
        for key,value in values.items():fixture=fixture.replace('@'+key+'@',value)
        assert '@' not in fixture
        assert 'if (!rx->batch_state.active || rx->batch_state.batch != batch) return;' in extract(receiver,'hop_xfer_rx_handle_file_begin')
        for handler_name in ['hop_xfer_rx_handle_file_begin','hop_xfer_rx_handle_file_end']:
            handler=extract(receiver,handler_name)
            assert 'xfer_rx_context *rx = (xfer_rx_context *)context;' in handler
            assert not any(token in handler for token in ['s_rx.', 's_x.', 's_ack.', 's_wq']), f'{platform} {handler_name} still reads receiver/platform globals'
        assert 'rx_handler_begin_batch(rx, batch)' in extract(receiver,'hop_xfer_rx_handle_image_begin')
        for core_handler in ['hop_xfer_rx_handle_file_begin','hop_xfer_rx_handle_image_begin','hop_xfer_rx_handle_file_chunk','hop_xfer_rx_handle_file_end','hop_xfer_rx_dispatch_file_ack_message','hop_xfer_rx_handle_abort','hop_xfer_rx_handle_done','hop_xfer_rx_dispatch_done_ack_message']:
            handler=extract(receiver,core_handler)
            assert 'xfer_rx_context *rx = (xfer_rx_context *)context;' in handler
            assert not any(token in handler for token in ['s_rx.', 's_x.', 's_ack.', 's_wq'])
        if platform=='win':fixture='#define FIXTURE_WINDOWS 1\n'+fixture
        run('abort_'+platform,fixture,['src/core/protocol.c','src/core/crc32.c','src/core/xfer_sender_queue.c','src/core/xfer_validate.c','src/core/xfer_budget.c','src/core/xfer_task.c','src/core/xfer_tracking.c','src/core/xfer_rx_receipt.c','src/core/xfer_rx_lifecycle.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_dispatch.c','src/core/xfer_rx_ack_handler.c','src/core/xfer_rx_entry_reservations.c','src/core/xfer_rx_file_state.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_image_buffer.c'],threads=True)

    inject=(ROOT/'src/mac/inject.c').read_text(encoding='utf-8')
    fixture=(ROOT/'tests/inject_release_fixture.c.in').read_text(encoding='utf-8')
    names=['recompute_flags','mod_index','post','inject_release_all','inject_reset','inject_key','inject_button']
    fixture=fixture.replace('@FUNCTIONS@','\n'.join(extract(inject,n) for n in names))
    run('inject_release',fixture,['src/core/keymap.c'])

    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c'))
        receiver+='\n'+(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        fixture=(ROOT/'tests/session_transport_fixture.c.in').read_text(encoding='utf-8')
        context_protocol=(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')
        assert 's_sender_context.user = &s_sender_platform' in context_protocol
        for callback in ['sender_context_is_stopping','sender_context_is_batch_cancelled','sender_context_peer_aborted','sender_context_wait_ns']:
            assert 's_x.' not in extract(context_protocol,callback)
        for callback in ['sender_context_session_current','sender_context_write_frame','sender_context_snapshot_session']:
            assert 's_x.' not in extract(sender,callback)
        functions='\n'.join([extract(sender,'sender_context_session_current'),extract(sender,'sender_context_write_frame'),extract(sender,'sender_context_snapshot_session'),extract(sender,'sender_status_stage_time'),extract(sender,'sender_status_chunk_timed'),extract(sender,'send_small_with_context'),extract(sender,'job_bind_session'),extract(sender,'sender_commit_is_stopping'),extract(sender,'sender_commit_wait_ms'),extract(sender,'send_small'),extract(sender,'send_chunk')]+[extract(receiver,name) for name in ['rx_abort_send','rx_abort_cancel_clipboard','rx_release_terminal_resources','rx_abort_release_resources','rx_abort_mark_dead','rx_abort_finish_task','rx_abort_clear_state','rx_abort_status_end','hop_xfer_rx_reset_batch_state','hop_xfer_rx_terminate_batch','hop_xfer_rx_bind_session','rx_bind_session','rx_expire_stale_session','on_data_frame_checked','on_data_frame']])
        fixture=fixture.replace('@FUNCTIONS@',functions)
        run('session_transport_'+platform,fixture,['src/core/protocol.c','src/core/session.c','src/core/clock.c','src/core/watchdog.c','src/core/update_state.c','src/core/xfer_rate.c','src/core/xfer_rx_receipt.c','src/core/xfer_rx_lifecycle.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_validate.c','src/core/crc32.c','src/core/xfer_task.c','src/core/xfer_budget.c','src/core/xfer_rx_entry_reservations.c','src/core/xfer_rx_image_buffer.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_file_state.c'],threads=True)

    control=(ROOT/'src/mac/net_client.c').read_text(encoding='utf-8')+'\n'+(ROOT/'src/mac/net_client_protocol.inc').read_text(encoding='utf-8')
    for platform,source_name,module_name in [('win','net_server.c','net_server_protocol.inc'),('mac','net_client.c','net_client_protocol.inc')]:
        transport=(ROOT/f'src/{platform}/{source_name}').read_text(encoding='utf-8')
        protocol=(ROOT/f'src/{platform}/{module_name}').read_text(encoding='utf-8')
        assert '#include "'+module_name+'"' in transport
        assert '#include "'+('net_server_transport.inc' if platform=='win' else 'net_client_transport.inc')+'"' in transport
        transport_module=(ROOT/'src'/platform/('net_server_transport.inc' if platform=='win' else 'net_client_transport.inc')).read_text(encoding='utf-8')
        assert 'reader_thread' in transport_module or 'control_sender' in transport_module
        assert 'net_loop_run' in transport_module or 'server_thread' in transport_module
        assert 'HOP_MSG_HELLO' in protocol and 'HOP_MSG_PING' in protocol

    dispatch=extract(control,'dispatch_message')
    dispatch=dispatch.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});','mac_capture_peer_geometry(pw,ph);')
    dispatch=dispatch.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});','mac_capture_return();')
    assert 'dispatch_async' not in dispatch
    fixture=(ROOT/'tests/dual_role_control_fixture.c.in').read_text(encoding='utf-8').replace('@DISPATCH@',dispatch)
    run('dual_role_control',fixture,['src/core/protocol.c','src/core/session.c','src/core/clock.c','src/core/xfer_validate.c','src/core/crc32.c'],threads=True)

    # An unconfirmed ACK must force a fresh data channel before the same
    # batch/file ID is reused; the unresolved file remains the resume point.
    for platform in ['mac','win']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        receiver='\n'.join((ROOT/f'src/{platform}/{name}').read_text(encoding='utf-8') for name in ('xfer_receive.inc','xfer_rx_protocol_handlers.c'))
        receiver+='\n'+(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        protocol_handlers=(ROOT/f'src/{platform}/xfer_rx_protocol_handlers.c').read_text(encoding='utf-8')
        entry=extract(sender,'sender_engine_send_entry')
        assert 'hop_xfer_sender_send_file_entry' in entry
        assert 'call->tx + CHUNK_HDR' in entry
        assert 'malloc(blen)' not in entry
        job=extract(sender,'run_job')
        assert 'hop_xfer_sender_run_files' in job
        assert 'window_item' not in job
        assert 'hop_xfer_sender_run_image' in job
        engine=(ROOT/'src/core/xfer_sender_engine.c').read_text(encoding='utf-8')
        assert 'int acknowledged;' in engine
        assert 'HOP_XFER_SENDER_SEND_PENDING' in engine
        assert 'if (!item->acknowledged)' in engine
        assert 'hop_xfer_ack_window_advance_checkpoint(next_index,' in engine
        assert 'fill_result = HOP_XFER_SENDER_JOB_NET_DOWN;' in engine
        timeout=engine.index('if (ack == HOP_XFER_SENDER_ACK_TIMEOUT)')
        unregister=engine.index('sender_unregister(ops, batch);',timeout)
        kick=engine.index('ops->request_reconnect(ops->user)',timeout)
        net_down=engine.index('return HOP_XFER_SENDER_JOB_NET_DOWN;',kick)
        assert timeout < unregister < kick < net_down
        reconnect=extract(sender,'sender_engine_request_reconnect')
        assert 'call->job->sender_context' in reconnect
        assert 'context->request_reconnect' in reconnect
        assert 'xfer_kick();' not in reconnect
        kick=extract(sender,'xfer_kick')
        assert 'sender_context_request_reconnect(&s_sender_platform)' in kick
        assert 'sender_engine_wait_ack' in sender
        assert 'static hop_xfer_ack_coordinator *s_ack' not in sender
        assert 'hop_xfer_ack_coordinator_' not in sender
        assert 'hop_xfer_sender_ack_adapter_create' in sender
        assert 'hop_xfer_sender_ack_adapter_register' in sender
        ack_wait_body=extract(sender,'ack_wait')
        assert 'context->ack_wait_take(context->ack_user' in ack_wait_body
        peer_abort_body=extract(sender,'sender_engine_peer_aborted')
        assert ': peer_aborted(batch)' not in peer_abort_body
        ack_deliver_body=extract(sender,'ack_deliver')
        assert 's_sender_context.ack_deliver(s_sender_context.ack_user' in ack_deliver_body
        platform_xfer=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        assert 's_ack' not in platform_xfer
        assert 'sender_context_ack_create()' in platform_xfer
        assert 'sender_context_ack_destroy()' in platform_xfer
        assert 'sender_context_wake_ack()' in platform_xfer
        assert 'sender_engine_send_entry' in sender
        assert 'sender_engine_send_image_begin' in sender
        assert 'sender_engine_send_image_chunk' in sender
        assert 'hop_xfer_sender_run_image' in engine
        assert 'hop_crc32(0, pixels + offset, length)' in engine
        assert 'hop_xfer_sender_send_file_entry' in engine
        assert 'ops->ack_register' in engine and 'ops->ack_mark_sent' in engine
        assert 'ops->read_file' in engine and 'ops->send_chunk' in engine
        assert 'hop_xfer_file_crc_step' in engine

        begin=extract(receiver,'hop_xfer_rx_handle_file_begin')
        end=extract(receiver,'hop_xfer_rx_handle_file_end')
        # FILE_BEGIN must keep the candidate path local until the task entry
        # reservation succeeds; only then may it replace live context state
        # or trigger clipboard/status/writer/filesystem side effects.
        admission=begin.index('hop_xfer_rx_entry_reservations_reserve(')
        commit=begin.index('rx->cur_path = (hop_xfer_rx_path_char *)candidate')
        assert admission < commit, 'FILE_BEGIN publishes candidate path before budget admission'
        for side_effect in ['rx->services.reset_file_error(', 'rx->services.remove_top_path(', 'rx->services.status_add_received_file(', 'rx->services.prepare_file_path(']:
            assert admission < begin.index(side_effect), 'FILE_BEGIN side effect precedes budget admission: '+side_effect
        file_ack_handler=extract(receiver,'hop_xfer_rx_dispatch_file_ack_message')
        done_ack_handler=extract(receiver,'hop_xfer_rx_dispatch_done_ack_message')
        services=(ROOT/'src/core/xfer_rx_services.h').read_text(encoding='utf-8')
        core_message_handlers=(ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        assert 'if (!rx->services.write_file_chunk || !rx->services.write_file_chunk(' in core_message_handlers
        context_header=(ROOT/'src/core/xfer_rx_context.h').read_text(encoding='utf-8')
        assert 'typedef struct xfer_rx_context' in context_header
        assert 'hop_xfer_rx_services services;' in context_header
        assert 'void *writer;' in context_header
        receive_impl=(ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8')
        assert 's_rx.services.context = &s_rx;' in receive_impl
        assert 's_rx.writer = s_wq;' in receive_impl
        assert 's_rx.services.build_path = rx_context_build_path;' in receive_impl
        assert 's_rx.services.prepare_file_path = rx_context_prepare_file_path;' in receive_impl
        assert 's_rx.services.is_top_level_path = rx_context_is_top_level_path;' in receive_impl
        assert 's_rx.services.remove_file = rx_context_remove_file;' in receive_impl
        assert 'task != &rx->task' in receive_impl
        assert 'rx->writer' in receive_impl
        assert 'typedef struct xfer_rx_context' not in receiver
        assert all(name in services for name in ['shutdown_data_channel','mark_sender_aborted','send_abort','send_file_ack','send_done_ack','deliver_ack','clear_cancel_request','create_staging_directory','discard_staging_directory','send_abort','reset_file_error','open_file','write_file_chunk','close_file','file_error','build_path','prepare_file_path','is_top_level_path','remove_file'])
        on_data_msg=extract(receiver,'on_data_msg')
        assert 'rx_context_configure();' in on_data_msg and '&hop_xfer_rx_platform_dispatch_handlers, &s_rx' in on_data_msg
        assert 'hop_xfer_rx_handle_file_ack(p, len, rx_ack_deliver_file, rx)' in file_ack_handler
        assert 'hop_xfer_rx_handle_done_ack(p, len, rx_ack_deliver_done, rx)' in done_ack_handler
        assert 'hop_xfer_rx_batch_abort_decide(' in core_message_handlers
        assert 'hop_xfer_rx_abort_execute(' in core_message_handlers
        abort_policy=(ROOT/'src/core/xfer_rx_lifecycle.c').read_text(encoding='utf-8')
        assert 'HOP_RX_BATCH_ABORT_REPLY_ONLY' in abort_policy
        assert 'HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE' in abort_policy
        assert 'hop_xfer_rx_batch_begin_decide(' in core_message_handlers
        assert 'hop_xfer_rx_batch_finish_decide(' in core_message_handlers
        for writer_call in ['reset_file_error','open_file','write_file_chunk','close_file','file_error']:
            assert 'rx->services.'+writer_call in protocol_handlers or 'rx->services.'+writer_call in core_message_handlers
        assert not any(token in protocol_handlers for token in ['wq_push(', 'wq_push_data(', 'wq_error_get(', 'rx_close_file(', 'hop_writer_queue_error_reset('])
        assert not re.search(r'(?<!services\.)\b(?:rx_add_top|rx_remove_top|status_add_received_file|status_image|status_stage_time|status_chunk_timed|status_file_phase|rate_now_ns)\s*\(', protocol_handlers)
        for service in ['add_top_path','remove_top_path','finish_clipboard','cancel_clipboard','now_ns','status_begin','status_end','status_add_received_file','status_image','status_stage_time','status_chunk_received','status_file_phase','status_set_files_total','status_reset']:
            assert 'rx->services.'+service in protocol_handlers or service in receiver
        done_handler=extract(receiver,'hop_xfer_rx_handle_done')
        assert 'hop_xfer_rx_done_receipt_find(rx->done_receipt,' in done_handler
        assert 'hop_xfer_rx_done_receipt_find(&rx->done_receipt,' not in done_handler
        shared_ack=(ROOT/'src/core/xfer_rx_ack_handler.c').read_text(encoding='utf-8')
        assert 'ack.id < 1u' in shared_ack and 'ack.id > HOP_XFER_FILE_LIMIT' in shared_ack
        assert 'status > HOP_DONE_ACK_FAILED' in shared_ack
        lifecycle=extract(receiver,'hop_xfer_rx_begin_batch')
        assert 'hop_xfer_rx_reset_batch_state(rx)' in lifecycle
        assert 'hop_xfer_rx_reset_batch_state' in core_message_handlers
        assert 'hop_xfer_rx_terminate_batch' in core_message_handlers
        assert 'rx_clear_state' not in receive_impl
        assert 'xfer_rx_batch_lifecycle.inc' not in receive_impl
        assert 's_rx.services.clear_cancel_request = rx_context_clear_cancel_request;' in receive_impl
        assert 's_rx.services.create_staging_directory = rx_context_create_staging_directory;' in receive_impl
        assert 's_rx.services.discard_staging_directory = rx_context_discard_staging_directory;' in receive_impl
        assert 'hop_xfer_rx_bind_session(&s_rx, session);' in receive_impl
        assert 'hop_xfer_rx_bind_session' in core_message_handlers
        if platform=='mac':
            assert 'hop_xfer_rx_fs_mkdir_p(rx->dir)' in receive_impl
        else:
            assert 'hop_xfer_rx_fs_create_directory_if_missing(rx->dir)' in receive_impl
        assert 'rx->services.remove_top_path' in begin
        assert 'rx->services.add_top_path' in begin and 'rx->services.add_top_path' in end
        if platform=='mac':
            assert 'hop_xfer_rx_fs_mkdir_p(copy)' in receive_impl
            fs=(ROOT/'src/mac/xfer_rx_fs.c').read_text(encoding='utf-8')
            assert 'hop_xfer_rx_fs_path_for' in fs and 'hop_xfer_rx_fs_remove_tree' in fs and 'hop_xfer_rx_fs_remove_file' in fs
        else:
            assert 'hop_xfer_rx_fs_create_directory_if_missing(path)' in receive_impl
            assert 'hop_xfer_rx_fs_ensure_dirs(copy)' in receive_impl
            fs=(ROOT/'src/win/xfer_rx_fs.c').read_text(encoding='utf-8')
            assert 'GetFileAttributesW(path)' in fs
            assert 'operation.fAnyOperationsAborted' in fs
            assert 'operation_result != 0 || operation.fAnyOperationsAborted' in fs
            assert 'hop_xfer_rx_fs_path_for' in fs and 'hop_xfer_rx_fs_remove_tree' in fs
            assert 'L"\\\\HopDesk\\\\"' in receive_impl, 'Windows staging cleanup must match the single-separator production path'
            assert 'retaining path for retry' in receive_impl
            assert 'if (!rx->staging_cleanup_pending) rx->dir[0] = 0;' in (ROOT/'src/core/xfer_rx_message_handlers.c').read_text(encoding='utf-8')
        close=end.index('rx->services.close_file(')
        cleanup=end.index('rx->services.remove_file(')
        assert close < cleanup, 'failed receive cleanup must wait for writer close'

        if platform=='mac':
            queue=extract(sender,'send_thread')
            assert 'peer_ready_snapshot' in queue and 'waited_for_queue' in queue
            assert 'hop_xfer_sender_queue_take_head(' in queue
            assert 'peer_ready_snapshot, !waited_for_queue' in queue
        else:
            queue=extract(sender,'send_thread')
            assert 'hop_xfer_sender_queue_take_head(' in queue
            assert 'c != INVALID_SOCKET, 1' in queue

    # DONE/DONE_ACK retry policy is now shared Core; platform files only adapt
    # current connection/session, wire send, ACK coordinator and wait callbacks.
    commit_core=(ROOT/'src/core/xfer_sender_engine.c').read_text(encoding='utf-8')
    assert 'hop_xfer_sender_run_commit' in commit_core
    assert 'HOP_XFER_SENDER_COMMIT_ACK_TIMEOUT' in commit_core
    assert 'HOP_XFER_SENDER_COMMIT_ACK_SEND_DEAD' in commit_core
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        commit=extract(sender,'send_done_result')
        assert 'hop_xfer_sender_run_commit' in commit
        assert 'ops.user = (void *)job' in commit
        assert 'sender_commit_current_connection' in sender
        assert 'sender_commit_wait_ack' in sender
        commit_stop=extract(sender,'sender_commit_is_stopping')
        commit_wait=extract(sender,'sender_commit_wait_ms')
        assert 'job->sender_context.is_stopping' in commit_stop
        assert 'job->sender_context.wait_ns' in commit_wait
        assert 'sender_context_is_stopping(NULL)' not in commit_stop
        thread=extract(sender,'send_thread')
        worker=(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')
        flow_core=(ROOT/'src/core/xfer_sender_worker_flow.c').read_text(encoding='utf-8')
        assert 'sender_worker_finish_job(' in thread
        assert 'hop_xfer_sender_worker_finish_job' in worker
        assert 'hop_xfer_sender_plan_retry(' not in worker
        assert 'hop_xfer_sender_get_terminal_decision(' not in worker
        assert 'hop_xfer_sender_plan_retry(' in flow_core
        assert 'hop_xfer_sender_get_terminal_decision(' in flow_core
        assert 'sender_status_start_job(&j->sender_context' in thread
        assert 'sender_status_end(&j->sender_context' not in thread
        assert 'sender_status_end(' in worker
        assert ' status_start_job(' not in thread and ' status_end(' not in thread

        # Shutdown is an ownership transfer, not a best-effort queue cleanup.
        lifecycle=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        submit=(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        shutdown=extract(lifecycle,'xfer_shutdown')
        shutdown_drain=extract(worker,'sender_shutdown_drain_jobs')
        job_free=extract(worker,'job_free')
        requeue=extract(worker,'sender_worker_requeue_front')
        submit_enter=extract(submit,'sender_submit_enter')
        assert 'XFER_JOB_OWNER_WORKER' in worker and 'XFER_JOB_OWNER_SUBMITTER' in submit
        assert 'XFER_JOB_OWNER_SHUTDOWN' in job_free and 'XFER_JOB_OWNER_RELEASED' in job_free
        assert 'hop_xfer_task_is_terminal' in job_free and 'sender_task_dispose' in job_free
        assert 'shutdown_jobs' in lifecycle and 's_x.shutting_down' in lifecycle
        assert 'XFER_JOB_OWNER_QUEUE' in submit and 'XFER_JOB_OWNER_QUEUE' in requeue
        assert 'XFER_JOB_OWNER_WORKER' in requeue and 'sender_worker_is_stopping' in requeue
        assert 'sender_submit_enter' in submit and 'sender_submit_leave' in submit
        assert ('AcquireSRWLockShared' in submit_enter if platform=='win' else 'pthread_rwlock_rdlock' in submit_enter)
        assert ('AcquireSRWLockExclusive' in shutdown if platform=='win' else 'pthread_rwlock_wrlock' in shutdown)
        assert shutdown.index('sender_threads_join') < shutdown.index('sender_shutdown_drain_jobs')
        assert shutdown.index('sender_shutdown_drain_jobs') < shutdown.index('rx_reset')
        assert shutdown.index('rx_reset') < shutdown.index('wq_stop') < shutdown.index('xfer_shutdown_task_audit')
        assert shutdown.index('xfer_shutdown_task_audit') < shutdown.index('hop_xfer_task_history_store_close')
        assert 'Transfer interrupted during shutdown' in shutdown_drain
        assert 'HOP_XFER_TASK_INTERRUPTED' in shutdown_drain
        assert 'hop_xfer_sender_queue_detach_all' in shutdown_drain
        assert 'while (s_x.shutdown_jobs)' in shutdown_drain
        assert 'hop_xfer_task_reconcile' in extract(lifecycle,'xfer_shutdown_task_audit')
        assert 'HOP_XFER_SENDER_JOB_SHUTDOWN' in flow_core
        assert 'ops->is_stopping' in flow_core
        if platform=='mac':
            assert 'hop_xfer_sender_queue_detach_all' not in thread
        else:
            assert 'sender_context_is_stopping(&s_sender_platform)' in thread

        receive=(ROOT/f'src/{platform}/xfer_receive.inc').read_text(encoding='utf-8')
        assert 'static int wq_stop(void)' in receive and 'return 0;' in extract(receive,'wq_stop')
        assert 'int xfer_shutdown(void)' in lifecycle
        if platform=='win':
            app=(ROOT/'src/win/win_main.c').read_text(encoding='utf-8')
            startup=extract(app,'wWinMain')
            assert startup.index('xfer_init(') < startup.index('net_server_start(')
            assert startup.index('clip_shutdown(') < startup.index('net_server_stop(') < startup.index('xfer_shutdown()')
            assert 'while (!xfer_shutdown()) Sleep(100)' in startup
        else:
            app=(ROOT/'src/mac/main.c').read_text(encoding='utf-8')
            startup=extract(app,'main')
            assert startup.index('xfer_init(') < startup.index('net_loop_run(')
            assert startup.index('clipboard_shutdown(') < startup.index('net_loop_shutdown(') < startup.index('xfer_shutdown()')
            assert 'while (!xfer_shutdown()) usleep(100000)' in startup
        protocol=(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')
        assert 'send_chunk(&call->job->sender_context' in protocol
        assert 'send_small_with_context(&call->job->sender_context' in protocol
        assert 'sender_status_file_phase(call ? &call->job->sender_context' in protocol
        assert 'sender_status_stage_time(call ? &call->job->sender_context' in protocol
        ack_wait=extract(protocol,'ack_wait')
        assert 's_x.' not in ack_wait
        assert 'context->session_current' in ack_wait
        assert 'sender_job_cancel_requested(context' in ack_wait
        assert 'context->peer_aborted' in ack_wait
        commit_wait_ack=extract(protocol,'sender_commit_wait_ack')
        assert 'job->commit_socket' in commit_wait_ack
        assert 'job->commit_epoch' in commit_wait_ack
        file_wait_ack=extract(protocol,'sender_engine_wait_ack')
        assert 'call->job->sender_context' in file_wait_ack
        commit_send=extract(protocol,'sender_commit_send_done')
        assert 'send_done_with_context(&job->sender_context' in commit_send
        abort_send=extract(protocol,'send_abort_msg_with_context')
        assert 'send_small_with_context(context' in abort_send
        assert 'send_abort_msg_with_context(&call->job->sender_context' in worker
        assert 'send_done_result(' in worker
        assert 'hop_xfer_sender_get_terminal_decision' not in thread and 'completion_path' in thread

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

    # Cancellation and DONE completion share one manager lock. Exactly one
    # contender must claim the terminal transition; the loser cannot win later.
    if os.name != 'nt':
        race_fixture=r"""
#include "xfer_task.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
uint64_t hop_wallclock_ms(void){return 1700000000000ull;}
static hop_xfer_task task;
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static int ready,go,completion_won,cancel_won;
static void await_gate(void){pthread_mutex_lock(&gate_lock);ready++;if(ready==3){go=1;pthread_cond_broadcast(&gate_cond);}while(!go)pthread_cond_wait(&gate_cond,&gate_lock);pthread_mutex_unlock(&gate_lock);}
static void *complete(void *unused){(void)unused;await_gate();completion_won=hop_xfer_task_begin_completion(&task);return NULL;}
static void *cancel(void *unused){(void)unused;await_gate();cancel_won=hop_xfer_task_request_cancel(task.id);return NULL;}
int main(void){
 for(unsigned i=0;i<500;i++){
  task=(hop_xfer_task){0};completion_won=cancel_won=0;
  assert(hop_xfer_task_init_classified(&task,0,0,i+1,HOP_XFER_DIR_RECEIVE,HOP_TASK_KIND_FILES));
  assert(hop_xfer_task_reserve_job(&task));
  assert(hop_xfer_task_set_state(&task,HOP_XFER_TASK_ACTIVE));
  pthread_mutex_lock(&gate_lock);ready=0;go=0;pthread_mutex_unlock(&gate_lock);
  pthread_t a,b;assert(pthread_create(&a,NULL,complete,NULL)==0);
  assert(pthread_create(&b,NULL,cancel,NULL)==0);
  await_gate();pthread_join(a,NULL);pthread_join(b,NULL);
  assert(completion_won+cancel_won==1);
  assert(!hop_xfer_task_request_cancel(task.id) || cancel_won);
  assert(hop_xfer_task_finish(&task,cancel_won?HOP_XFER_TASK_CANCELLED:HOP_XFER_TASK_COMPLETED));
  hop_xfer_task_dispose(&task);
 }
 hop_xfer_task_audit audit;assert(hop_xfer_task_reconcile(&audit)&&!audit.mismatches&&!audit.jobs);
 puts("PASS: 500 concurrent receive cancellation vs DONE completion races");return 0;
}
"""
        run('task_cancel_completion_race',race_fixture,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)


    run('task_history',(ROOT/'tests/task_history_tests.c').read_text(encoding='utf-8'),['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    history_ui=(ROOT/'tests/task_history_ui_fixture.c.in').read_text(encoding='utf-8')
    history_ui=history_ui.replace('@UI_FUNCTIONS@',extract(ui,'task_state_name')+'\n'+extract(ui,'refresh_task_history'))
    run('task_history_ui_win',history_ui,['src/core/xfer_budget.c','src/core/xfer_task.c'],threads=True)

    for platform,source_name in [('win','clipboard.c'),('mac','clipboard.m')]:
        clipboard_source=(ROOT/f'src/{platform}/{source_name}').read_text(encoding='utf-8')
        paste_module=(ROOT/f'src/{platform}/clipboard_paste.inc').read_text(encoding='utf-8')
        offer_module=(ROOT/f'src/{platform}/clipboard_offer.inc').read_text(encoding='utf-8')
        assert '#include "clipboard_paste.inc"' in clipboard_source
        assert '#include "clipboard_offer.inc"' in clipboard_source
        assert 'clip_send_offer' in offer_module and 'clip_publish_local' in offer_module
        assert 'clip_send_paste_failed' in paste_module
        assert 'clip_complete_remote_paste' in paste_module or 'clipboard_complete_remote_paste' in paste_module

    # A paste request cannot be replayed as a generic transfer: that would
    # discard destination/version context and may overwrite the peer clipboard.
    retry_fixture=(ROOT/'tests/retry_guard_fixture.c.in').read_text(encoding='utf-8')
    for platform in ['win','mac']:
        sender=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_protocol.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')+'\n'+(ROOT/f'src/{platform}/xfer_submit.inc').read_text(encoding='utf-8')
        fixture=retry_fixture.replace('@RETRY@',extract(sender,'xfer_retry_file'))
        if platform=='win':fixture='#define FIXTURE_WINDOWS 1\n'+fixture
        run('retry_guard_'+platform,fixture,[])

    # The shared Core queue policy is production-linked by both platform workers.
    queue_core=(ROOT/'src/core/xfer_sender_queue.c').read_text(encoding='utf-8')
    queue_header=(ROOT/'src/core/xfer_sender_queue.h').read_text(encoding='utf-8')
    queue_tests=(ROOT/'tests/xfer_sender_queue_tests.c').read_text(encoding='utf-8')
    assert 'hop_xfer_sender_queue_push_back' in queue_core
    assert 'hop_xfer_sender_queue_push_front' in queue_core
    assert 'hop_xfer_sender_queue_detach_all' in queue_core
    assert 'hop_xfer_sender_queue_contains_batch' in queue_core
    assert 'hop_xfer_sender_queue_take_cancelled' in queue_core
    assert 'hop_xfer_sender_queue_take_head' in queue_core
    assert 'now_ms >= give_up_at' in queue_core
    assert 'retry_at_ms(ops->user, node) <= now_ms' in queue_core
    assert 'HOP_XFER_SENDER_QUEUE_READY' in queue_header
    assert 'HOP_XFER_SENDER_QUEUE_EXPIRED' in queue_header
    assert 'test_cancelled_middle_and_tail' in queue_tests
    assert 'test_head_readiness_and_fifo' in queue_tests
    assert 'test_front_insert_lookup_and_detach' in queue_tests
    assert 'test_expiry_while_offline' in queue_tests
    for platform in ['win','mac']:
        worker=(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')
        assert 'hop_xfer_sender_queue_take_cancelled' in worker
        assert 'hop_xfer_sender_queue_take_head' in worker
        assert 'queue_unlink' not in worker
        transport=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        thread_adapter=(ROOT/f'src/{platform}/xfer_sender_threads.c').read_text(encoding='utf-8')
        if platform=='win':
            assert 'CreateThread(' not in transport
            assert 'hop_win_sender_threads_start' in transport
            assert 'CreateThread(' in thread_adapter and 'WaitForSingleObject(' in thread_adapter
        else:
            assert 'pthread_create(' not in transport
            assert 'pthread_join(' not in transport
            assert 'hop_mac_sender_threads_start' in transport
            assert 'pthread_create(' in thread_adapter and 'pthread_join(' in thread_adapter

    retry_core=(ROOT/'src/core/xfer_sender_retry.c').read_text(encoding='utf-8')
    retry_header=(ROOT/'src/core/xfer_sender_retry.h').read_text(encoding='utf-8')
    retry_tests=(ROOT/'tests/xfer_sender_retry_tests.c').read_text(encoding='utf-8')
    assert 'hop_xfer_sender_plan_retry' in retry_core
    assert 'retry_add_saturating' in retry_core
    assert 'HOP_XFER_SENDER_RETRY_SCHEDULED' in retry_header
    assert 'HOP_XFER_SENDER_RETRY_EXHAUSTED' in retry_header
    assert 'test_initial_and_repeated_retry' in retry_tests
    assert 'test_attempt_and_deadline_exhaustion' in retry_tests
    assert 'test_saturating_deadlines_and_invalid_input' in retry_tests
    worker_flow_core=(ROOT/'src/core/xfer_sender_worker_flow.c').read_text(encoding='utf-8')
    assert 'hop_xfer_sender_plan_retry' in worker_flow_core
    for platform in ['win','mac']:
        worker=(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')
        assert 'hop_xfer_sender_worker_finish_job' in worker
        assert 'hop_xfer_sender_plan_retry' not in worker

    lifecycle_core=(ROOT/'src/core/xfer_sender_lifecycle.c').read_text(encoding='utf-8')
    lifecycle_header=(ROOT/'src/core/xfer_sender_lifecycle.h').read_text(encoding='utf-8')
    lifecycle_tests=(ROOT/'tests/xfer_sender_lifecycle_tests.c').read_text(encoding='utf-8')
    assert 'hop_xfer_sender_get_terminal_decision' in lifecycle_core
    assert 'HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN' in lifecycle_core
    assert 'HOP_XFER_SENDER_JOB_COMMIT_REJECTED' in lifecycle_core
    assert 'hop_xfer_sender_terminal_decision' in lifecycle_header
    assert 'expect(HOP_XFER_SENDER_JOB_COMMIT_REJECTED' in lifecycle_tests
    assert 'expect(HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN' in lifecycle_tests
    assert 'HOP_XFER_TASK_INTERRUPTED' in lifecycle_tests
    assert 'Cancelled by receiver' in lifecycle_tests
    assert 'hop_xfer_sender_get_terminal_decision' in worker_flow_core
    for platform in ['win','mac']:
        worker=(ROOT/f'src/{platform}/xfer_sender_worker.inc').read_text(encoding='utf-8')
        assert 'hop_xfer_sender_worker_finish_job' in worker
        assert 'hop_xfer_sender_get_terminal_decision' not in worker

    win_main=(ROOT/'src/win/win_main.c').read_text(encoding='utf-8')
    assert win_main.index('clip_shutdown(g_app.hwnd)') < win_main.index('xfer_shutdown()')

    # macOS control workers can call xfer_kick during disconnect handling;
    # they must be joined before xfer_shutdown destroys transfer synchronization.
    mac_main=(ROOT/'src/mac/main.c').read_text(encoding='utf-8')
    mac_transport=(ROOT/'src/mac/net_client_transport.inc').read_text(encoding='utf-8')
    assert mac_main.index('clipboard_shutdown()') < mac_main.index('net_loop_shutdown()') < mac_main.index('xfer_shutdown()')
    assert 'pthread_detach' not in mac_transport
    assert 'pthread_join(s_net_thread' in mac_transport
    assert 'pthread_join(s_control_sender_thread' in mac_transport

    # hop_xfer_tracking embeds 8192 file records with 1024-byte paths
    # (~8 MiB). Keep these fixtures off Windows' default 1 MiB thread stack.
    core_tests=(ROOT/'tests/core_tests.c').read_text(encoding='utf-8')
    for name in ['test_xfer_tracking_stage_timing','test_xfer_tracking_speed_and_eta']:
        body=extract(core_tests,name)
        assert not re.search(r'(?m)^\s*(?:const\s+)?hop_xfer_tracking\s+\w+\s*;',body), (
            name+' must not place the multi-megabyte tracking object on the stack'
        )

    print('All targeted regressions passed.',flush=True)
