/* Server input capture. All state is main-runloop-owned; net_send only queues. */
#include "mac_app.h"
#include <ApplicationServices/ApplicationServices.h>
#include <string.h>
#include <math.h>
static CFMachPortRef tap;
static int connected,sharing;
static double peer_w,peer_h;
static uint64_t edge_since,cooldown;
static CGPoint anchor;
static int have_last;
static unsigned char held[128],buttons[3];
static void send_key(unsigned k,int down){uint8_t p[2];hop_put_be16(p,(uint16_t)k);net_send(down?HOP_MSG_KEY_DOWN:HOP_MSG_KEY_UP,p,2);}
static void send_button(int b,int down){uint8_t p[2]={(uint8_t)b,(uint8_t)down};net_send(HOP_MSG_MOUSE_BUTTON,p,2);buttons[b]=(unsigned char)down;}
void mac_capture_peer_geometry(uint32_t w,uint32_t h){peer_w=w;peer_h=h;}
void mac_capture_return(void){
    if(!sharing)return;
    for(unsigned k=0;k<128;++k)if(held[k]){send_key(k,0);held[k]=0;}
    for(int b=0;b<3;++b)if(buttons[b])send_button(b,0);
    sharing=0;edge_since=0;cooldown=hop_monotonic_ms()+g_mac.cfg.switch_cooldown_ms;
    CGWarpMouseCursorPosition(anchor);statusbar_update(connected,0);
}
void mac_capture_connected(int on){connected=on;
    if(!on)mac_capture_return();}
static int at_edge(CGPoint p){
    int e=g_mac.cfg.server_edge;return e==0?p.x<=0:e==1?p.x>=g_mac.screen_w-1:e==2?p.y<=0:p.y>=g_mac.screen_h-1;
}
static void enter(CGPoint p){
    int e=g_mac.cfg.server_edge;
    double extent=e<2?g_mac.screen_w:g_mac.screen_h;
    double m=g_mac.cfg.edge_margin_px;
    if(m<1)m=1;
    if(m>extent/2)m=extent/2;
    anchor=p;
    if(e<2)anchor.x=e==0?m:g_mac.screen_w-1-m;else anchor.y=e==2?m:g_mac.screen_h-1-m;
    double along=e<2?p.y:p.x,local=e<2?g_mac.screen_h:g_mac.screen_w,remote=e<2?peer_h:peer_w;
    if(remote>0&&local>0)along=along*remote/local;
    if(along<0)along=0;
    if(along>65535)along=65535;
    uint8_t data[2];hop_put_be16(data,(uint16_t)along);
    if(!net_send(HOP_MSG_MOUSE_ENTER,data,2))return;
    sharing=1;edge_since=0;memset(held,0,sizeof(held));memset(buttons,0,sizeof(buttons));
    CGWarpMouseCursorPosition(anchor);statusbar_update(1,1);
    /* Carry modifiers already held across an edge transition. */
    const unsigned mods[]={0x36,0x37,0x38,0x3A,0x3B,0x3C,0x3D,0x3E};
    for(unsigned i=0;i<sizeof(mods)/sizeof(mods[0]);++i)if(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,(CGKeyCode)mods[i])){held[mods[i]]=1;send_key(mods[i],1);}
}
static CGEventRef capture_event(CGEventTapProxy proxy,CGEventType type,CGEventRef event,void *ctx){
    (void)proxy;(void)ctx;
    if(type==kCGEventTapDisabledByTimeout||type==kCGEventTapDisabledByUserInput){mac_capture_return();
    if(tap)CGEventTapEnable(tap,true);return event;}
    if(!connected)return event;
    CGPoint p=CGEventGetLocation(event);
    if(!sharing){
        if(type==kCGEventMouseMoved){
            if(at_edge(p)&&hop_monotonic_ms()>=cooldown){if(!edge_since)edge_since=hop_monotonic_ms();}
            else edge_since=0;
            have_last=1;
        }
        return event;
    }
    switch(type){
    case kCGEventMouseMoved:case kCGEventLeftMouseDragged:case kCGEventRightMouseDragged:case kCGEventOtherMouseDragged:{
        int dx=(int)CGEventGetIntegerValueField(event,kCGMouseEventDeltaX),dy=(int)CGEventGetIntegerValueField(event,kCGMouseEventDeltaY);
        if(dx>32767)dx=32767;
    if(dx< -32768)dx= -32768;
    if(dy>32767)dy=32767;
    if(dy< -32768)dy= -32768;
        if(dx||dy){uint8_t data[4];hop_put_be16(data,(uint16_t)(int16_t)dx);hop_put_be16(data+2,(uint16_t)(int16_t)dy);net_send(HOP_MSG_MOUSE_MOVE,data,4);}
        CGWarpMouseCursorPosition(anchor);return NULL;
    }
    case kCGEventLeftMouseDown:send_button(0,1);return NULL;
    case kCGEventLeftMouseUp:send_button(0,0);return NULL;
    case kCGEventRightMouseDown:send_button(2,1);return NULL;
    case kCGEventRightMouseUp:send_button(2,0);return NULL;
    case kCGEventOtherMouseDown:send_button(1,1);return NULL;
    case kCGEventOtherMouseUp:send_button(1,0);return NULL;
    case kCGEventScrollWheel:{
        int dy=(int)CGEventGetIntegerValueField(event,kCGScrollWheelEventDeltaAxis1)*120;
        int dx=(int)CGEventGetIntegerValueField(event,kCGScrollWheelEventDeltaAxis2)*120;
        if(dx>32767)dx=32767;
    if(dx< -32768)dx= -32768;
    if(dy>32767)dy=32767;
    if(dy< -32768)dy= -32768;
        uint8_t data[4];hop_put_be16(data,(uint16_t)(int16_t)dx);hop_put_be16(data+2,(uint16_t)(int16_t)dy);net_send(HOP_MSG_MOUSE_WHEEL,data,4);return NULL;
    }
    case kCGEventKeyDown:case kCGEventKeyUp:case kCGEventFlagsChanged:{
        unsigned k=(unsigned)CGEventGetIntegerValueField(event,kCGKeyboardEventKeycode);
    if(k>=128)return NULL;
        int down=type==kCGEventFlagsChanged?CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,(CGKeyCode)k):type==kCGEventKeyDown;
        if(k==0x35&&(CGEventGetFlags(event)&kCGEventFlagMaskControl)&&(CGEventGetFlags(event)&kCGEventFlagMaskAlternate)){mac_capture_return();return NULL;}
        held[k]=(unsigned char)down;send_key(k,down);return NULL;
    }
    default:return event;
    }
}
static void edge_tick(CFRunLoopTimerRef timer,void *ctx){
    (void)timer;(void)ctx;
    if(!connected||sharing||!edge_since||!have_last)return;
    CGEventRef e=CGEventCreate(NULL);
    if(!e)return;CGPoint p=CGEventGetLocation(e);CFRelease(e);
    if(!at_edge(p)){edge_since=0;return;}
    if(hop_monotonic_ms()-edge_since>=(uint64_t)g_mac.cfg.switch_delay_ms)enter(p);
}
void mac_capture_init(void){
    if(g_mac.cfg.role!=HOP_ROLE_SERVER)return;
    CGEventMask mask=CGEventMaskBit(kCGEventMouseMoved)|CGEventMaskBit(kCGEventLeftMouseDragged)|CGEventMaskBit(kCGEventRightMouseDragged)|CGEventMaskBit(kCGEventOtherMouseDragged)|CGEventMaskBit(kCGEventLeftMouseDown)|CGEventMaskBit(kCGEventLeftMouseUp)|CGEventMaskBit(kCGEventRightMouseDown)|CGEventMaskBit(kCGEventRightMouseUp)|CGEventMaskBit(kCGEventOtherMouseDown)|CGEventMaskBit(kCGEventOtherMouseUp)|CGEventMaskBit(kCGEventScrollWheel)|CGEventMaskBit(kCGEventKeyDown)|CGEventMaskBit(kCGEventKeyUp)|CGEventMaskBit(kCGEventFlagsChanged);
    tap=CGEventTapCreate(kCGHIDEventTap,kCGHeadInsertEventTap,kCGEventTapOptionDefault,mask,capture_event,NULL);
    if(!tap){hop_log(HOP_LOG_ERROR,"capture: grant Accessibility and Input Monitoring permissions, then restart HopDesk");return;}
    CFRunLoopSourceRef source=CFMachPortCreateRunLoopSource(NULL,tap,0);CFRunLoopAddSource(CFRunLoopGetMain(),source,kCFRunLoopCommonModes);CFRelease(source);
    CFRunLoopTimerRef timer=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+.02,.02,0,0,edge_tick,NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(),timer,kCFRunLoopCommonModes);CFRelease(timer);CGEventTapEnable(tap,true);
}
