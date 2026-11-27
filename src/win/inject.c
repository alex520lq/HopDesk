/* Client input injection. Owned exclusively by the Windows message thread. */
#include "app.h"
#include <string.h>
#include <math.h>
static BYTE held[256],buttons[3],paste_suppressed[256];
static BYTE key_release_pending[256],button_release_pending[3];
static double px,py,over;
static int sharing;
static int emit(INPUT *input){
    static DWORD last_warning;static int warned;
    if(SendInput(1,input,sizeof(*input))==1)return 1;
    DWORD now=GetTickCount();
    if(!warned || (DWORD)(now-last_warning)>5000){
        hop_log(HOP_LOG_WARN,"inject: SendInput failed (%lu); check desktop/elevation permissions",(unsigned long)GetLastError());
        last_warning=now;warned=1;
    }
    return 0;
}
static int mouse(DWORD flags,LONG dx,LONG dy,DWORD data){
    INPUT i;memset(&i,0,sizeof(i));i.type=INPUT_MOUSE;i.mi.dwFlags=flags;i.mi.dx=dx;i.mi.dy=dy;i.mi.mouseData=data;
    return emit(&i);
}
static int key(WORD vk,int down){
    INPUT i;memset(&i,0,sizeof(i));i.type=INPUT_KEYBOARD;i.ki.wVk=vk;
    if(!down)i.ki.dwFlags|=KEYEVENTF_KEYUP;
    if(vk==VK_RCONTROL||vk==VK_RMENU||vk==VK_LWIN||vk==VK_RWIN||(vk>=VK_PRIOR&&vk<=VK_DOWN)||vk==VK_INSERT||vk==VK_DELETE||vk==VK_DIVIDE)
        i.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;
    return emit(&i);
}
static int button(int b,int down){
    static const DWORD d[]={MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_MIDDLEDOWN,MOUSEEVENTF_RIGHTDOWN};
    static const DWORD u[]={MOUSEEVENTF_LEFTUP,MOUSEEVENTF_MIDDLEUP,MOUSEEVENTF_RIGHTUP};
    if(b<0||b>2)return 0;
    if(!mouse(down?d[b]:u[b],0,0,0))return 0;
    buttons[b]=(BYTE)down;
    return 1;
}
void win_inject_reset(void){
    for(int k=0;k<256;++k)if(held[k]||key_release_pending[k]){
        if(key((WORD)k,0)){held[k]=0;key_release_pending[k]=0;}
        else key_release_pending[k]=1;
    }
    for(int b=0;b<3;++b)if(buttons[b]||button_release_pending[b]){
        if(button(b,0))button_release_pending[b]=0;
        else button_release_pending[b]=1;
    }
    sharing=0;over=0;memset(paste_suppressed,0,sizeof(paste_suppressed));
}

/* A failed SendInput key-up must remain pending; forgetting it is itself a
 * stuck-key bug. Retry on the UI timer and before accepting a new key-down. */
void win_inject_retry_pending(void){
    for(int k=0;k<256;++k)if(key_release_pending[k]){
        if(key((WORD)k,0)){held[k]=0;key_release_pending[k]=0;}
    }
    for(int b=0;b<3;++b)if(button_release_pending[b]){
        if(button(b,0))button_release_pending[b]=0;
    }
}
static double edge_margin(double w,double h){
    int edge=g_app.cfg.client_edge,vertical=edge>=HOP_EDGE_TOP;
    double extent=vertical?h:w;
    double scale=vertical?(g_app.peer_h>0?h/g_app.peer_h:1.0):(g_app.peer_w>0?w/g_app.peer_w:1.0);
    double m=g_app.cfg.edge_margin_px*scale;
    if(m<1)m=1;
    if(m>extent/2)m=extent/2;
    return m;
}
static void enter(int along){
    double w=GetSystemMetrics(SM_CXSCREEN),h=GetSystemMetrics(SM_CYSCREEN),m=edge_margin(w,h);
    int edge=g_app.cfg.client_edge;
    if(edge<2){px=edge==HOP_EDGE_LEFT?m:w-1-m;py=along;}
    else{px=along;py=edge==HOP_EDGE_TOP?m:h-1-m;}
    if(px<0)px=0;
    if(px>w-1)px=w-1;
    if(py<0)py=0;
    if(py>h-1)py=h-1;
    sharing=1;over=0;SetCursorPos((int)px,(int)py);g_app.state=HOP_LINK_SHARING;ui_refresh(&g_app);
}
void win_inject_message(uint8_t type,const uint8_t *p,uint32_t len){
    switch(type){
    case HOP_MSG_EDGE_LEAVE:
        if(len==0 && !(g_app.cfg.input_mode & HOP_INPUT_MOUSE)){
            win_inject_reset();
            g_app.state=HOP_LINK_LOCAL;
            ui_refresh(&g_app);
        }
        break;
    case HOP_MSG_MOUSE_ENTER:
        if(!(g_app.cfg.input_mode & HOP_INPUT_MOUSE) && len>=2)enter(hop_get_be16(p));
        break;
    case HOP_MSG_MOUSE_MOVE:if(!(g_app.cfg.input_mode & HOP_INPUT_MOUSE) && len>=4){
        if(!sharing)return;
        double w=GetSystemMetrics(SM_CXSCREEN),h=GetSystemMetrics(SM_CYSCREEN);
        double mscale = g_app.cfg.mouse_scale_pct >= 25 ? (double)g_app.cfg.mouse_scale_pct / 100.0 : 1.0;
        px+=(int16_t)hop_get_be16(p)*(g_app.peer_w>0?w/g_app.peer_w:1)*mscale;
        py+=(int16_t)hop_get_be16(p+2)*(g_app.peer_h>0?h/g_app.peer_h:1)*mscale;
        int edge=g_app.cfg.client_edge;double beyond=edge==0?-px:edge==1?px-w+1:edge==2?-py:py-h+1;
        over=beyond>0?over+beyond:0;
        double margin=edge_margin(w,h);
        if(over>=margin){win_inject_reset();g_app.state=HOP_LINK_LOCAL;net_server_send(g_app.net,HOP_MSG_EDGE_LEAVE,(uint8_t[]){(uint8_t)edge},1);ui_refresh(&g_app);return;}
        if(px<0)px=0;
        if(px>w-1)px=w-1;
        if(py<0)py=0;
        if(py>h-1)py=h-1;
        SetCursorPos((int)px,(int)py);
    }break;
    case HOP_MSG_MOUSE_BUTTON:if(!(g_app.cfg.input_mode & HOP_INPUT_MOUSE) && len>=2 && sharing){
        int b=p[0],down=!!p[1];
        if(!button(b,down)&&!down&&b<3)button_release_pending[b]=1;
    }break;
    case HOP_MSG_MOUSE_WHEEL:if(!(g_app.cfg.input_mode & HOP_INPUT_MOUSE) && len>=4 && sharing){
        double wscale = g_app.cfg.wheel_scale_pct >= 25 ? (double)g_app.cfg.wheel_scale_pct / 100.0 : 1.0;
        double dir = g_app.cfg.wheel_invert ? -1.0 : 1.0;
        int dx=(int)llround((double)(int16_t)hop_get_be16(p) * wscale * dir);
        int dy=(int)llround((double)(int16_t)hop_get_be16(p+2) * wscale * dir);
        if(dx)mouse(MOUSEEVENTF_HWHEEL,0,0,(DWORD)dx);
        if(dy)mouse(MOUSEEVENTF_WHEEL,0,0,(DWORD)dy);
    }break;
    case HOP_MSG_KEY_DOWN:case HOP_MSG_KEY_UP:if(!(g_app.cfg.input_mode & HOP_INPUT_KEYBOARD) && len>=2){
        WORD vk=hop_mac_to_vk(hop_get_be16(p));int down=type==HOP_MSG_KEY_DOWN;
        if(vk!=HOP_KEY_NONE && vk<256) {
            if(!down && paste_suppressed[vk]) { paste_suppressed[vk]=0; break; }
            if(down && key_release_pending[vk]){
                if(!key(vk,0))break;
                held[vk]=0;key_release_pending[vk]=0;
            }
            if(down && vk=='V' &&
               (held[VK_LCONTROL]||held[VK_RCONTROL]||held[VK_LWIN]||held[VK_RWIN]) &&
               clip_request_remote_paste()) {
                paste_suppressed[vk]=1;
                const WORD mods[]={VK_LCONTROL,VK_RCONTROL,VK_LWIN,VK_RWIN};
                for(unsigned i=0;i<sizeof(mods)/sizeof(mods[0]);++i) {
                    WORD mod=mods[i];
                    if(held[mod]) {
                        if(key(mod,0)){held[mod]=0;key_release_pending[mod]=0;}
                        else key_release_pending[mod]=1;
                        paste_suppressed[mod]=1;
                    }
                }
                break;
            }
            if(sharing||!(g_app.peer_input_mode & HOP_INPUT_MOUSE)||(!down&&held[vk])){
                if(key(vk,down)){held[vk]=(BYTE)down;key_release_pending[vk]=0;}
                else if(!down)key_release_pending[vk]=1;
            }
        }
    }break;
    default:break;
    }
}
