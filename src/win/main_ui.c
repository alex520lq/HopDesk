/* =====================================================================
 * HopDesk - Windows native main window
 * A single persistent Win32 window; tray menu only opens it or exits.
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "update.h"
#include "main_ui.h"
#include "../core/xfer_status.h"
#include "../core/hop.h"
#include "../core/xfer_task_history.h"
#include "../platform/audio_link.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { UI_PAGE_OVERVIEW, UI_PAGE_DEVICE, UI_PAGE_TRANSFER, UI_PAGE_SETTINGS, UI_PAGE_COUNT };
#define UI_TIMER 31
#define UI_NAV_BASE 5000
#define UI_ID_SAVE 5100
#define UI_ID_RESET 5101
#define UI_ID_RECONNECT 5102
#define UI_ID_XFER_CANCEL 5103
#define UI_ID_XFER_RETRY 5104
#define UI_ID_LOG_REFRESH 5105
#define UI_ID_LOG_COPY 5106
#define UI_ID_UPDATE 5107
#define UI_ID_OPEN_TRANSFERS 5108
#define UI_ID_SWITCH_MAC 5109
#define UI_ID_SWITCH_WIN 5110
#define UI_ID_WAKE 5111
#define UI_ID_SLEEP 5112
#define UI_ID_TOGGLE 5113
#define UI_ID_FILES 5114
#define UI_ID_SEND_TASKS 5115
#define UI_ID_RECEIVE_TASKS 5116
#define UI_ID_SEND_CANCEL 5117
#define UI_ID_RECEIVE_CANCEL 5118
#define UI_ID_AUDIO_SEND 5119
#define UI_ID_AUDIO_PLAY 5120
#define UI_TASK_CAP HOP_TASK_MANAGER_MAX_LIVE

enum {
    CFG_NAME, CFG_HOST, CFG_PORT, CFG_DATA_PORT, CFG_TOKEN,
    CFG_SERVER_EDGE, CFG_CLIENT_EDGE, CFG_SWITCH_DELAY, CFG_COOLDOWN,
    CFG_MARGIN, CFG_DISP_WIN, CFG_DISP_MAC, CFG_WAKE, CFG_CHUNK,
    CFG_MAC_INPUT, CFG_MAC_SLEEP, CFG_ROLE, CFG_COUNT
};

static HWND s_main;
static HWND s_pages[UI_PAGE_COUNT];
static HWND s_nav[UI_PAGE_COUNT];
static HWND s_title, s_header_status, s_footer;
static HWND s_overview_status, s_overview_peer, s_overview_control, s_overview_data, s_overview_sharing;
static HWND s_connection_role, s_connection_status, s_connection_peer, s_connection_ports;
static HWND s_cfg[CFG_COUNT], s_cfg_note, s_host_label, s_host_hint;
static HWND s_audio_send, s_audio_play, s_audio_status;
static HWND s_cfg_save_button, s_cfg_reset_button;
static HWND s_xfer_title, s_xfer_detail, s_xfer_counts;
static HWND s_xfer_progress, s_xfer_files, s_xfer_retry_button;
static HWND s_task_lists[2], s_cancel_task_buttons[2];
static HWND s_task_history, s_history_direction, s_history_state, s_history_order;
static hop_xfer_task_id s_task_ids[2][UI_TASK_CAP];
static int s_task_counts[2];
static hop_xfer_task_snapshot *s_tasks_snapshot;
static int s_tasks_snapshot_capacity;
static HWND s_log_edit,s_log_note;
static int s_log_note_error;
static HWND s_update_version, s_update_source;
static HFONT s_font, s_font_title, s_font_small;
static int s_page;
static hop_config s_draft;
static int s_pending_restart,s_save_busy,s_log_busy;
static int s_settings_dirty,s_populating_settings,s_cfg_note_error;
static UINT s_dpi=96;
static HBRUSH s_canvas,s_sidebar;
static wchar_t s_addresses[512];
#define UI_ROLE_ID 5150
#define UI_SAVE_RESULT (WM_APP+61)
#define UI_LOG_RESULT (WM_APP+62)
typedef struct {HWND hwnd;int x,y,w,h,font;} ui_node;
static ui_node s_nodes[160];static int s_node_count;
/* Cards are page decoration, not sibling windows: child windows must remain
 * visible and clickable above their backgrounds, regardless of Z order. */
typedef struct {HWND page;const wchar_t *title;int x,y,w,h;} ui_card;
static ui_card s_cards[32];static int s_card_count;
static int dip(int v){return MulDiv(v,(int)s_dpi,96);}
static void layout_main(HWND h);
static void layout_nodes(void);
static hop_xfer_file_status *s_file_snapshot;
static int s_file_snapshot_capacity;
static int resize_file_snapshot(int capacity) {
    if (capacity < 0 || capacity > HOP_XFER_FILE_LIMIT) return 0;
    if (capacity == s_file_snapshot_capacity) return 1;
    if (capacity == 0) {
        free(s_file_snapshot);
        s_file_snapshot = NULL;
        s_file_snapshot_capacity = 0;
        return 1;
    }
    hop_xfer_file_status *next = (hop_xfer_file_status *)realloc(
        s_file_snapshot, (size_t)capacity * sizeof(*s_file_snapshot));
    if (!next) return 0;
    s_file_snapshot = next;
    s_file_snapshot_capacity = capacity;
    return 1;
}
static int s_collapsed[HOP_XFER_FILE_LIMIT];
static int s_visible_indices[HOP_XFER_FILE_LIMIT];
static int s_visible_count;
static uint32_t s_file_batch;
static int s_file_direction,s_file_batch_active;

static void refresh_transfers(void);
static void refresh_logs(void);

static const wchar_t *const kPageNames[UI_PAGE_COUNT] = {
    L"概览", L"设备", L"传输", L"设置"
};

static HWND child(HWND parent, const wchar_t *klass, const wchar_t *text,
                  DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                             dip(x), dip(y), dip(w), dip(h), parent, (HMENU)(INT_PTR)id,
                             g_app.inst, NULL);
    if(c && s_node_count<160)s_nodes[s_node_count++]=(ui_node){c,x,y,w,h,0};
    if (c && s_font) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}
static HWND label(HWND p, const wchar_t *text, int x, int y, int w, int h) {
    return child(p, L"STATIC", text, SS_LEFT, x, y, w, h, 0);
}
static HWND button(HWND p, const wchar_t *text, int x, int y, int w, int h, int id) {
    return child(p, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id);
}
static HWND edit(HWND p, const wchar_t *text, int x, int y, int w, int h, int id, int password) {
    return child(p, L"EDIT", text, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP |
                 (password ? ES_PASSWORD : 0), x, y, w, h, id);
}
static void group(HWND p, const wchar_t *text, int x, int y, int w, int h) {
    if (s_card_count < (int)(sizeof(s_cards) / sizeof(s_cards[0])))
        s_cards[s_card_count++] = (ui_card){p,text,x,y,w,h};
}
static HWND page_create(HWND parent, int index) {
    wchar_t cls[32];
    (void)index;
    wcscpy(cls, L"HopDeskMainPage");
    return CreateWindowExW(WS_EX_CONTROLPARENT, cls, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL, dip(228), dip(88), dip(860), dip(620),
        parent, NULL, g_app.inst, NULL);
}
static void paint_cards(HWND page,HDC dc,int scroll) {
    HPEN pen=CreatePen(PS_SOLID,1,RGB(226,231,238));
    HBRUSH fill=CreateSolidBrush(RGB(255,255,255));
    HGDIOBJ old_pen=SelectObject(dc,pen),old_brush=SelectObject(dc,fill);
    HGDIOBJ old_font=SelectObject(dc,s_font);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(91,103,120));
    for(int i=0;i<s_card_count;++i){
        ui_card *c=&s_cards[i];if(c->page!=page)continue;
        int left=dip(c->x),top=dip(c->y)-scroll,right=dip(c->x+c->w),bottom=dip(c->y+c->h)-scroll;
        RoundRect(dc,left,top,right-1,bottom-1,dip(12),dip(12));
        RECT text={left+dip(18),top+dip(12),right-dip(16),top+dip(34)};
        DrawTextW(dc,c->title,-1,&text,DT_SINGLELINE|DT_LEFT|DT_VCENTER);
    }
    SelectObject(dc,old_font);SelectObject(dc,old_brush);SelectObject(dc,old_pen);
    DeleteObject(fill);DeleteObject(pen);
}

enum { UI_BUTTON_SECONDARY, UI_BUTTON_PRIMARY, UI_BUTTON_DANGER };
static int button_variant(int id) {
    if (id==UI_ID_SAVE || id==UI_ID_RECONNECT ||
        id==UI_ID_OPEN_TRANSFERS || id==UI_ID_UPDATE) return UI_BUTTON_PRIMARY;
    if (id==UI_ID_SEND_CANCEL || id==UI_ID_RECEIVE_CANCEL) return UI_BUTTON_DANGER;
    return UI_BUTTON_SECONDARY;
}
static void paint_action_button(DRAWITEMSTRUCT *item) {
    int variant=button_variant((int)item->CtlID);
    int disabled=(item->itemState&ODS_DISABLED)!=0;
    int pressed=(item->itemState&ODS_SELECTED)!=0;
    COLORREF background,border,foreground;
    if (disabled) {
        background=RGB(244,246,249);border=RGB(232,235,240);foreground=RGB(160,168,178);
    } else if (variant==UI_BUTTON_PRIMARY) {
        background=pressed?RGB(27,91,177):RGB(39,112,216);border=background;foreground=RGB(255,255,255);
    } else if (variant==UI_BUTTON_DANGER) {
        background=pressed?RGB(255,226,224):RGB(255,242,241);
        border=RGB(246,215,212);foreground=RGB(177,58,52);
    } else {
        background=pressed?RGB(229,235,243):RGB(242,245,249);
        border=RGB(222,228,236);foreground=RGB(55,67,84);
    }
    HBRUSH brush=CreateSolidBrush(background);HPEN pen=CreatePen(PS_SOLID,1,border);
    HGDIOBJ old_brush=SelectObject(item->hDC,brush),old_pen=SelectObject(item->hDC,pen);
    RECT r=item->rcItem;
    RoundRect(item->hDC,r.left+1,r.top+1,r.right-1,r.bottom-1,dip(9),dip(9));
    SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,foreground);
    HFONT old_font=(HFONT)SelectObject(item->hDC,s_font);
    wchar_t text[160]={0};GetWindowTextW(item->hwndItem,text,(int)(sizeof(text)/sizeof(text[0])));
    InflateRect(&r,-dip(8),-dip(2));
    DrawTextW(item->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(item->itemState&ODS_FOCUS){InflateRect(&r,-dip(3),-dip(3));DrawFocusRect(item->hDC,&r);}
    SelectObject(item->hDC,old_font);SelectObject(item->hDC,old_brush);SelectObject(item->hDC,old_pen);
    DeleteObject(brush);DeleteObject(pen);
}
static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if(m==WM_DRAWITEM){
        DRAWITEMSTRUCT *item=(DRAWITEMSTRUCT*)l;
        if(item && item->CtlType==ODT_BUTTON){paint_action_button(item);return TRUE;}
    }
    if(m==WM_COMMAND){SendMessageW(GetParent(h),m,w,l);return 0;}
    if(m==WM_CTLCOLORSTATIC){
        HWND control=(HWND)l;SetBkMode((HDC)w,TRANSPARENT);
        if(control==s_header_status || control==s_overview_status)
            SetTextColor((HDC)w,g_app.state==HOP_LINK_SHARING?RGB(36,111,190):
                g_app.state==HOP_LINK_LOCAL?RGB(27,132,82):RGB(112,122,137));
        else if(control==s_overview_control)
            SetTextColor((HDC)w,g_app.state==HOP_LINK_DOWN?RGB(112,122,137):RGB(27,132,82));
        else if(control==s_overview_data)
            SetTextColor((HDC)w,xfer_connected()?RGB(27,132,82):RGB(112,122,137));
        else if(control==s_overview_sharing)
            SetTextColor((HDC)w,g_app.state==HOP_LINK_SHARING?RGB(36,111,190):
                g_app.state==HOP_LINK_LOCAL?RGB(27,132,82):RGB(112,122,137));
        else if(control==s_cfg_note)
            SetTextColor((HDC)w,s_cfg_note_error?RGB(180,55,50):RGB(91,103,120));
        else if(control==s_log_note)
            SetTextColor((HDC)w,s_log_note_error?RGB(180,55,50):RGB(91,103,120));
        else SetTextColor((HDC)w,RGB(48,58,73));
        return (LRESULT)s_canvas;
    }
    if(m==WM_SIZE){RECT r;GetClientRect(h,&r);int content_height=h==s_pages[UI_PAGE_TRANSFER]?960:h==s_pages[UI_PAGE_SETTINGS]?1580:624;SCROLLINFO si={sizeof(si),SIF_RANGE|SIF_PAGE,0,dip(content_height),r.bottom,0,0};SetScrollInfo(h,SB_VERT,&si,TRUE);layout_nodes();return 0;}
    if(m==WM_VSCROLL || m==WM_MOUSEWHEEL){
        SCROLLINFO si={sizeof(si),SIF_ALL,0,0,0,0,0};GetScrollInfo(h,SB_VERT,&si);int pos=si.nPos;
        if(m==WM_MOUSEWHEEL)pos-=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*dip(48);
        else switch(LOWORD(w)){case SB_LINEUP:pos-=dip(24);break;case SB_LINEDOWN:pos+=dip(24);break;case SB_PAGEUP:pos-=(int)si.nPage;break;case SB_PAGEDOWN:pos+=(int)si.nPage;break;case SB_THUMBTRACK:pos=si.nTrackPos;break;}
        si.fMask=SIF_POS;si.nPos=pos;SetScrollInfo(h,SB_VERT,&si,TRUE);layout_nodes();InvalidateRect(h,NULL,TRUE);return 0;
    }
    if(m==WM_ERASEBKGND){RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,s_canvas);return 1;}
    if(m==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);
        paint_cards(h,dc,GetScrollPos(h,SB_VERT));
        EndPaint(h,&ps);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
static void set_text(HWND h, const wchar_t *s) {
    if (h) SetWindowTextW(h, s ? s : L"");
}
static void refresh_audio_status(void) {
    hop_audio_status status = hop_audio_get_status();
    if (!status.requested && g_app.cfg.audio_mode) {
        status.requested = g_app.cfg.audio_mode;
        status.unavailable = g_app.cfg.audio_mode;
    }
    wchar_t send[80] = L"", play[80] = L"", text[192] = L"音频已关闭";
    if (status.requested & HOP_AUDIO_SEND) {
        const wchar_t *state = (status.active & HOP_AUDIO_SEND) ? L"已连接" :
            (status.unavailable & HOP_AUDIO_SEND) ? L"不可用" : L"等待对端播放";
        swprintf_s(send, ARRAYSIZE(send), L"发送：%ls", state);
    }
    if (status.requested & HOP_AUDIO_PLAY) {
        const wchar_t *state = (status.active & HOP_AUDIO_PLAY) ? L"已连接" :
            (status.unavailable & HOP_AUDIO_PLAY) ? L"不可用" : L"等待对端发送";
        swprintf_s(play, ARRAYSIZE(play), L"播放：%ls", state);
    }
    if (send[0] && play[0]) swprintf_s(text, ARRAYSIZE(text), L"%ls    ·    %ls", send, play);
    else if (send[0]) wcscpy_s(text, ARRAYSIZE(text), send);
    else if (play[0]) wcscpy_s(text, ARRAYSIZE(text), play);
    set_text(s_audio_status, text);
}
static void set_font(HWND h, HFONT f) {
    if (h && f) {SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);for(int i=0;i<s_node_count;++i)if(s_nodes[i].hwnd==h)s_nodes[i].font=f==s_font_title?1:f==s_font_small?2:0;}
}
static void utf8_to_wide(const char *src, wchar_t *dst, int cap) {
    if (!cap) return;
    dst[0] = 0;
    if (src && src[0]) MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
}
static int wide_to_utf8(HWND h, char *dst, int cap) {
    wchar_t w[1024];
    if (!h || !dst || cap <= 0) return 0;
    GetWindowTextW(h, w, (int)(sizeof(w) / sizeof(w[0])));
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, cap, NULL, NULL) > 0;
}
static void set_int(HWND h, int v) {
    wchar_t b[32]; swprintf(b, 32, L"%d", v); set_text(h, b);
}
static unsigned long parse_number(HWND h, unsigned long fallback) {
    wchar_t b[64], *end = NULL;
    (void)fallback;
    if (!h || GetWindowTextLengthW(h)>=64) return 0xFFFFFFFFUL;
    GetWindowTextW(h, b, 64);
    if (!b[0]) return 0xFFFFFFFFUL;
    unsigned long v = wcstoul(b, &end, (b[0] == L'0' && (b[1] == L'x' || b[1] == L'X')) ? 16 : 10);
    return end == b || *end || b[0]==L'-' ? 0xFFFFFFFFUL : v;
}
static void show_page(int page) {
    if (page < 0 || page >= UI_PAGE_COUNT) page = UI_PAGE_OVERVIEW;
    s_page = page;
    /* Hide every host first: no old page may repaint over the selected one. */
    for(int i=0;i<UI_PAGE_COUNT;++i)if(s_pages[i])ShowWindow(s_pages[i],SW_HIDE);
    if(s_pages[page])SetWindowPos(s_pages[page],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    for(int i=0;i<UI_PAGE_COUNT;++i)if(s_nav[i])InvalidateRect(s_nav[i],NULL,TRUE);
    RedrawWindow(s_main,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    set_text(s_title, kPageNames[page]);
    if (page == UI_PAGE_TRANSFER) refresh_transfers();
    if (page == UI_PAGE_SETTINGS) refresh_logs();
}
static const wchar_t *link_state(void) {
    if (!g_app.net) return L"网络服务未启动";
    if (g_app.cfg.role==HOP_ROLE_SERVER && (!net_server_listening(g_app.net) || !xfer_listening())) return L"端口不可用，正在重试";
    if (g_app.state == HOP_LINK_SHARING) return g_app.cfg.role==HOP_ROLE_SERVER?L"键鼠已移交客户端":L"正在接收服务器键鼠";
    if (g_app.state == HOP_LINK_LOCAL) return L"已连接";
    return g_app.cfg.role==HOP_ROLE_SERVER?L"等待客户端连接":g_app.cfg.peer_host[0]?L"正在连接服务器":L"请先填写服务器 IP";
}
static void refresh_connection(void) {
    wchar_t b[512];
    set_text(s_header_status, link_state());
    set_text(s_overview_status, link_state());
    if(g_app.cfg.role==HOP_ROLE_SERVER)
        swprintf(b,512,L"服务器 · 本机 IP：%ls    控制端口 %u    数据端口 %u",
            s_addresses,(unsigned)g_app.cfg.port,(unsigned)g_app.cfg.data_port);
    else {
        wchar_t host[128];utf8_to_wide(g_app.cfg.peer_host,host,128);
        swprintf(b,512,L"客户端 · 服务器：%ls    控制端口 %u    数据端口 %u",
            host[0]?host:L"未配置",(unsigned)g_app.cfg.port,(unsigned)g_app.cfg.data_port);
    }
    set_text(s_overview_peer,b);
    set_text(s_overview_control,g_app.state==HOP_LINK_DOWN?L"等待连接":L"已连接");
    set_text(s_overview_data,xfer_connected()?L"已连接":L"等待连接");
    set_text(s_overview_sharing,g_app.state==HOP_LINK_SHARING?L"已移交":
        g_app.state==HOP_LINK_LOCAL?L"已就绪 · 等待边缘移交":L"等待连接");
    set_text(s_connection_role,g_app.cfg.role==HOP_ROLE_SERVER?L"本机角色：服务器 · 提供键鼠":L"本机角色：客户端 · 接收键鼠");
    set_text(s_connection_status, link_state());
    if(g_app.cfg.role==HOP_ROLE_SERVER)swprintf(b,512,L"本机 IP：%ls",s_addresses);
    else {wchar_t host[128];utf8_to_wide(g_app.cfg.peer_host,host,128);swprintf(b,512,L"服务器 IP：%ls",host[0]?host:L"未配置 · 请前往设置");}
    set_text(s_connection_peer, b);
    swprintf(b, 512, L"控制端口：%u    数据端口：%u",
             (unsigned)g_app.cfg.port, (unsigned)g_app.cfg.data_port);
    set_text(s_connection_ports, b);
    swprintf(b, 512, L"HopDesk  v%hs    ·    %ls", HOP_VERSION, link_state());
    if(s_pending_restart)wcscat(b,L" · 设置已保存，重启生效");
    set_text(s_footer, b);
    refresh_audio_status();
}
static wchar_t *load_logs(void) {
    if (!g_app.log_path[0]) return _wcsdup(L"日志路径尚未初始化。");
    wchar_t path[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, g_app.log_path, -1, path, 1024))
        return _wcsdup(L"日志路径无法转换。");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return _wcsdup(L"暂无日志记录。");
        wchar_t message[128];swprintf_s(message,_countof(message),L"读取日志失败（系统错误 %lu）。",(unsigned long)error);
        return _wcsdup(message);
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size)) {
        DWORD error=GetLastError();CloseHandle(f);
        wchar_t message[128];swprintf_s(message,_countof(message),L"无法读取日志大小（系统错误 %lu）。",(unsigned long)error);
        return _wcsdup(message);
    }
    if(size.QuadPart<=0){CloseHandle(f);return _wcsdup(L"暂无日志记录。");}
    const DWORD maxBytes = 256u * 1024u;
    DWORD take = (DWORD)(size.QuadPart > maxBytes ? maxBytes : size.QuadPart);
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - take;
    if(!SetFilePointerEx(f,pos,NULL,FILE_BEGIN)){
        DWORD error=GetLastError();CloseHandle(f);
        wchar_t message[128];swprintf_s(message,_countof(message),L"无法定位日志内容（系统错误 %lu）。",(unsigned long)error);
        return _wcsdup(message);
    }
    char *bytes = (char *)malloc((size_t)take + 1);
    if (!bytes) { CloseHandle(f); return _wcsdup(L"内存不足，无法读取日志。"); }
    DWORD got = 0;
    BOOL ok = ReadFile(f, bytes, take, &got, NULL);
    DWORD readError=ok?ERROR_SUCCESS:GetLastError();
    CloseHandle(f);
    if (!ok) {
        free(bytes);wchar_t message[128];
        swprintf_s(message,_countof(message),L"读取日志失败（系统错误 %lu）。",(unsigned long)readError);
        return _wcsdup(message);
    }
    bytes[got] = 0;
    if (pos.QuadPart > 0) {
        char *nl = memchr(bytes, '\n', got);
        if (nl) { size_t skip = (size_t)(nl - bytes) + 1; memmove(bytes, bytes + skip, got - skip + 1); got -= (DWORD)skip; }
    }
    if(!got){free(bytes);return _wcsdup(L"暂无日志记录。");}
    int needed = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, NULL, 0);
    if (needed <= 0) { free(bytes); return _wcsdup(L"日志编码无法识别。"); }
    wchar_t *wide = (wchar_t *)calloc((size_t)needed + 1, sizeof(wchar_t));
    if (!wide) { free(bytes); return _wcsdup(L"内存不足，无法显示日志。"); }
    MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, wide, needed);
    free(bytes);return wide;
}
static int log_result_is_error(const wchar_t *text) {
    return !text || wcsncmp(text,L"读取",2)==0 || wcsncmp(text,L"无法",2)==0 ||
        wcsncmp(text,L"日志路径",4)==0 || wcsncmp(text,L"日志编码",4)==0 ||
        wcsncmp(text,L"内存不足",4)==0;
}
static void set_log_note(const wchar_t *message,int error) {
    s_log_note_error=error;set_text(s_log_note,message);
    if(s_log_note)InvalidateRect(s_log_note,NULL,TRUE);
}
static int copy_logs_to_clipboard(void) {
    if(!s_log_edit)return 0;
    int length=GetWindowTextLengthW(s_log_edit);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,((SIZE_T)length+1)*sizeof(wchar_t));
    if(!memory)return 0;
    wchar_t *text=(wchar_t*)GlobalLock(memory);
    if(!text){GlobalFree(memory);return 0;}
    GetWindowTextW(s_log_edit,text,length+1);
    GlobalUnlock(memory);
    if(!OpenClipboard(s_main)){GlobalFree(memory);return 0;}
    BOOL emptied=EmptyClipboard();
    HANDLE placed=emptied?SetClipboardData(CF_UNICODETEXT,memory):NULL;
    CloseClipboard();
    if(!placed){GlobalFree(memory);return 0;}
    return 1;
}
typedef struct {hop_config config;int ok;} save_result;
static DWORD WINAPI log_worker(void *arg){
    HWND target=(HWND)arg;wchar_t *text=load_logs();
    if(!PostMessageW(target,UI_LOG_RESULT,0,(LPARAM)text))free(text);
    return 0;
}
static DWORD WINAPI save_worker(void *arg){
    save_result *result=(save_result*)arg;
    result->ok=hop_config_save_atomic(&result->config,g_app.config_path);
    if(!PostMessageW(s_main,UI_SAVE_RESULT,0,(LPARAM)result))free(result);
    return 0;
}
static void refresh_logs(void){
    if(s_log_busy||!s_log_edit)return;
    s_log_busy=1;set_log_note(L"正在读取日志…",0);
    HANDLE t=CreateThread(NULL,0,log_worker,s_main,0,NULL);
    if(t)CloseHandle(t);
    else{s_log_busy=0;set_text(s_log_edit,L"无法启动日志读取。");set_log_note(L"读取失败，请重试",1);}
}
static void set_settings_note(const wchar_t *message,int error) {
    s_cfg_note_error=error;
    set_text(s_cfg_note,message);
    if(s_cfg_note)InvalidateRect(s_cfg_note,NULL,TRUE);
}
static void update_settings_actions(void) {
    if(s_cfg_save_button)EnableWindow(s_cfg_save_button,s_settings_dirty&&!s_save_busy);
    if(s_cfg_reset_button)EnableWindow(s_cfg_reset_button,!s_save_busy);
}
static void mark_settings_dirty(void) {
    if(s_populating_settings||s_save_busy)return;
    s_settings_dirty=1;
    set_settings_note(L"有未保存的更改。音频设置可立即生效；网络与认证设置需重启。",0);
    update_settings_actions();
}
static void settings_validation_error(int field,const wchar_t *message) {
    s_settings_dirty=1;
    set_settings_note(message,1);
    update_settings_actions();
    if(field>=0&&field<CFG_COUNT&&s_cfg[field])SetFocus(s_cfg[field]);
}
static void update_settings_role_fields(void) {
    int client=SendMessageW(s_cfg[CFG_ROLE],CB_GETCURSEL,0,0)==1;
    EnableWindow(s_cfg[CFG_HOST],client);
    set_text(s_host_label,L"服务器地址");
    set_text(s_host_hint,client?
        L"客户端：接收服务器键鼠；音频发送/播放独立配置。":
        L"服务器：向客户端提供键鼠；音频发送/播放独立配置。");
    SendMessageW(s_cfg[CFG_HOST],EM_SETCUEBANNER,TRUE,
        (LPARAM)(client?L"例如 192.168.1.20":L"服务器模式无需填写"));
}
static void set_settings_editors_enabled(int enabled) {
    for(int i=0;i<CFG_COUNT;++i)if(s_cfg[i])EnableWindow(s_cfg[i],enabled);
    if(s_audio_send)EnableWindow(s_audio_send,enabled);
    if(s_audio_play)EnableWindow(s_audio_play,enabled);
    if(enabled)update_settings_role_fields();
    else if(s_cfg[CFG_HOST])EnableWindow(s_cfg[CFG_HOST],FALSE);
}
static void populate_settings(void) {
    hop_config *c=&s_draft;
    wchar_t b[128];
    s_populating_settings=1;
    SendMessageW(s_cfg[CFG_ROLE],CB_SETCURSEL,c->role==HOP_ROLE_CLIENT?1:0,0);
    utf8_to_wide(c->device_name,b,128);set_text(s_cfg[CFG_NAME],b);
    if(c->peer_host[0]){utf8_to_wide(c->peer_host,b,128);set_text(s_cfg[CFG_HOST],b);}
    else set_text(s_cfg[CFG_HOST],L"");
    set_int(s_cfg[CFG_PORT],c->port);
    set_int(s_cfg[CFG_DATA_PORT],c->data_port);
    utf8_to_wide(c->token,b,128);set_text(s_cfg[CFG_TOKEN],b);
    SendMessageW(s_cfg[CFG_SERVER_EDGE],CB_SETCURSEL,
        c->server_edge>=HOP_EDGE_LEFT&&c->server_edge<=HOP_EDGE_BOTTOM?c->server_edge:HOP_EDGE_LEFT,0);
    SendMessageW(s_cfg[CFG_CLIENT_EDGE],CB_SETCURSEL,
        c->client_edge>=HOP_EDGE_LEFT&&c->client_edge<=HOP_EDGE_BOTTOM?c->client_edge:HOP_EDGE_RIGHT,0);
    set_int(s_cfg[CFG_SWITCH_DELAY],c->switch_delay_ms);
    set_int(s_cfg[CFG_COOLDOWN],c->switch_cooldown_ms);
    set_int(s_cfg[CFG_MARGIN],c->edge_margin_px);
    swprintf(b,128,L"0x%02X",(unsigned)c->disp_win_input);set_text(s_cfg[CFG_DISP_WIN],b);
    swprintf(b,128,L"0x%02X",(unsigned)c->disp_mac_input);set_text(s_cfg[CFG_DISP_MAC],b);
    set_int(s_cfg[CFG_WAKE],c->disp_wake_sec);
    set_int(s_cfg[CFG_CHUNK],c->xfer_chunk_kb);
    utf8_to_wide(c->mac_input_cmd,b,128);set_text(s_cfg[CFG_MAC_INPUT],b);
    utf8_to_wide(c->mac_sleep_cmd,b,128);set_text(s_cfg[CFG_MAC_SLEEP],b);
    SendMessageW(s_audio_send,BM_SETCHECK,(c->audio_mode & HOP_AUDIO_SEND)?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(s_audio_play,BM_SETCHECK,(c->audio_mode & HOP_AUDIO_PLAY)?BST_CHECKED:BST_UNCHECKED,0);
    refresh_audio_status();
    update_settings_role_fields();
    s_populating_settings=0;
    s_settings_dirty=0;
    set_settings_note(L"设置已加载。音频设置保存后立即生效；网络、认证和角色参数重启后生效。",0);
    update_settings_actions();
}
static void reset_settings(void) {
    if(s_save_busy)return;
    hop_config_defaults(&s_draft);
    snprintf(s_draft.device_name,sizeof(s_draft.device_name),"%s","Windows");
    populate_settings();
    s_settings_dirty=1;
    set_settings_note(L"已恢复默认值，但尚未写入配置文件。确认后点击“保存设置”。",0);
    update_settings_actions();
}
static void save_settings(void) {
    hop_config next=s_draft;
    if(s_save_busy||!s_settings_dirty)return;
    next.role=SendMessageW(s_cfg[CFG_ROLE],CB_GETCURSEL,0,0)==1?HOP_ROLE_CLIENT:HOP_ROLE_SERVER;
    char utf8[256];
    if(!wide_to_utf8(s_cfg[CFG_HOST],utf8,sizeof(utf8))||strlen(utf8)>=sizeof(next.peer_host)||
        (next.role==HOP_ROLE_CLIENT&&!utf8[0])){
        settings_validation_error(CFG_HOST,L"客户端模式需要填写服务器 IP 地址或主机名。");return;
    }
    memcpy(next.peer_host,utf8,strlen(utf8)+1);
    if(!wide_to_utf8(s_cfg[CFG_NAME],utf8,sizeof(utf8))||!utf8[0]||strlen(utf8)>=sizeof(next.device_name)){
        settings_validation_error(CFG_NAME,L"设备名称不能为空，且长度不能超过 31 个字节。");return;
    }
    memcpy(next.device_name,utf8,strlen(utf8)+1);
    if(!wide_to_utf8(s_cfg[CFG_TOKEN],utf8,sizeof(utf8))||!utf8[0]||strlen(utf8)>=sizeof(next.token)){
        settings_validation_error(CFG_TOKEN,L"共享 Token 不能为空，且不能超过 128 个字节；两台设备必须使用相同 Token。");return;
    }
    memcpy(next.token,utf8,strlen(utf8)+1);
    unsigned long port=parse_number(s_cfg[CFG_PORT],0),data_port=parse_number(s_cfg[CFG_DATA_PORT],0);
    unsigned long delay=parse_number(s_cfg[CFG_SWITCH_DELAY],250),cooldown=parse_number(s_cfg[CFG_COOLDOWN],300);
    unsigned long margin=parse_number(s_cfg[CFG_MARGIN],40),win_input=parse_number(s_cfg[CFG_DISP_WIN],0x0F);
    unsigned long mac_input=parse_number(s_cfg[CFG_DISP_MAC],0x11),wake=parse_number(s_cfg[CFG_WAKE],60);
    unsigned long chunk=parse_number(s_cfg[CFG_CHUNK],256);
    if(!port||port>65535){settings_validation_error(CFG_PORT,L"控制端口必须在 1–65535 之间。");return;}
    if(!data_port||data_port>65535||data_port==port){settings_validation_error(CFG_DATA_PORT,L"数据端口必须在 1–65535 之间，且不能与控制端口相同。");return;}
    if(delay>5000){settings_validation_error(CFG_SWITCH_DELAY,L"边缘触发延迟不能超过 5000 ms。");return;}
    if(cooldown>10000){settings_validation_error(CFG_COOLDOWN,L"切换冷却不能超过 10000 ms。");return;}
    if(margin>1000){settings_validation_error(CFG_MARGIN,L"边缘内缩不能超过 1000 px。");return;}
    if(win_input>65535){settings_validation_error(CFG_DISP_WIN,L"Windows 输入源必须是 0–65535 的十进制或十六进制数值。");return;}
    if(mac_input>65535){settings_validation_error(CFG_DISP_MAC,L"Mac 输入源必须是 0–65535 的十进制或十六进制数值。");return;}
    if(wake>3600){settings_validation_error(CFG_WAKE,L"唤醒时长不能超过 3600 秒。");return;}
    if(chunk<64||chunk>4096){settings_validation_error(CFG_CHUNK,L"传输分块必须在 64–4096 KB 之间。");return;}
    next.port=(uint16_t)port;next.data_port=(uint16_t)data_port;
    next.server_edge=(int)SendMessageW(s_cfg[CFG_SERVER_EDGE],CB_GETCURSEL,0,0);
    next.client_edge=(int)SendMessageW(s_cfg[CFG_CLIENT_EDGE],CB_GETCURSEL,0,0);
    next.switch_delay_ms=(int)delay;next.switch_cooldown_ms=(int)cooldown;next.edge_margin_px=(int)margin;
    next.disp_win_input=(uint16_t)win_input;next.disp_mac_input=(uint16_t)mac_input;
    next.disp_wake_sec=(int)wake;next.xfer_chunk_kb=(int)chunk;
    next.audio_mode =
        (SendMessageW(s_audio_send,BM_GETCHECK,0,0)==BST_CHECKED?HOP_AUDIO_SEND:0u) |
        (SendMessageW(s_audio_play,BM_GETCHECK,0,0)==BST_CHECKED?HOP_AUDIO_PLAY:0u);
    if(next.audio_mode && port==HOP_AUDIO_PORT){
        settings_validation_error(CFG_PORT,L"控制端口 24819 已保留给系统音频，请选择其他端口。");return;
    }
    if(next.audio_mode && data_port==HOP_AUDIO_PORT){
        settings_validation_error(CFG_DATA_PORT,L"数据端口 24819 已保留给系统音频，请选择其他端口。");return;
    }
    if(!wide_to_utf8(s_cfg[CFG_MAC_INPUT],utf8,sizeof(utf8)))utf8[0]=0;
    if(strlen(utf8)>=sizeof(next.mac_input_cmd)){settings_validation_error(CFG_MAC_INPUT,L"Mac 切源命令过长。");return;}
    memcpy(next.mac_input_cmd,utf8,strlen(utf8)+1);
    if(!wide_to_utf8(s_cfg[CFG_MAC_SLEEP],utf8,sizeof(utf8)))utf8[0]=0;
    if(strlen(utf8)>=sizeof(next.mac_sleep_cmd)){settings_validation_error(CFG_MAC_SLEEP,L"Mac 睡眠命令过长。");return;}
    memcpy(next.mac_sleep_cmd,utf8,strlen(utf8)+1);
    save_result *result=calloc(1,sizeof(*result));
    if(!result){settings_validation_error(-1,L"内存不足，无法保存设置。");return;}
    result->config=next;s_save_busy=1;
    set_settings_editors_enabled(0);
    set_settings_note(L"正在保存设置…",0);update_settings_actions();
    HANDLE t=CreateThread(NULL,0,save_worker,result,0,NULL);
    if(t)CloseHandle(t);
    else{
        free(result);s_save_busy=0;s_settings_dirty=1;
        set_settings_editors_enabled(1);
        set_settings_note(L"无法启动配置保存，请重试。",1);update_settings_actions();
    }
}
/* Snapshot only: selection stores Task ID, never a pointer into manager state. */
static hop_xfer_task_id selected_task(int direction) {
    HWND list=s_task_lists[direction];
    int selected=list?(int)SendMessageW(list,LB_GETCURSEL,0,0):LB_ERR;
    if(selected==LB_ERR || selected<0 || selected>=s_task_counts[direction])return 0;
    return s_task_ids[direction][selected];
}
static const wchar_t *task_state_name(const hop_xfer_task_snapshot *t) {
    if(t->state<=HOP_XFER_TASK_ACTIVE && t->cancel_requested)return L"取消中";
    if(t->state<=HOP_XFER_TASK_ACTIVE && t->completing)return L"完成处理中";
    switch(t->state) {
        case HOP_XFER_TASK_CREATED:return L"准备中";
        case HOP_XFER_TASK_SCANNING:return L"扫描中";
        case HOP_XFER_TASK_QUEUED:return L"排队中";
        case HOP_XFER_TASK_ACTIVE:return L"进行中";
        case HOP_XFER_TASK_COMPLETED:return L"已完成";
        case HOP_XFER_TASK_PARTIAL:return L"部分完成";
        case HOP_XFER_TASK_FAILED:return L"失败";
        case HOP_XFER_TASK_CANCELLED:return L"已取消";
        case HOP_XFER_TASK_INTERRUPTED:return L"已中断";
        default:return L"未知";
    }
}
static int resize_task_snapshot(int capacity) {
    if (capacity < 0 || capacity > (int)UI_TASK_CAP) return 0;
    if (capacity == s_tasks_snapshot_capacity) return 1;
    if (capacity == 0) {
        free(s_tasks_snapshot);
        s_tasks_snapshot = NULL;
        s_tasks_snapshot_capacity = 0;
        return 1;
    }
    hop_xfer_task_snapshot *next = (hop_xfer_task_snapshot *)realloc(
        s_tasks_snapshot, (size_t)capacity * sizeof(*s_tasks_snapshot));
    if (!next) return 0;
    s_tasks_snapshot = next;
    s_tasks_snapshot_capacity = capacity;
    return 1;
}
static void refresh_task_lists(void) {
    if(!s_task_lists[0] || !s_task_lists[1])return;
    int total=0,n=-1;
    uint64_t revision=0;
    for(int attempt=0;attempt<3;++attempt){
        if(hop_xfer_task_list_revisioned(NULL,0,&total,&revision)<0 ||
           total<0 || total>(int)UI_TASK_CAP)return;
        if(!resize_task_snapshot(total))return;
        int actual_total=0;
        uint64_t actual_revision=0;
        n=hop_xfer_task_list_revisioned(s_tasks_snapshot,total,
                                        &actual_total,&actual_revision);
        if(n<0)return;
        if(actual_total==total && actual_revision==revision)break;
        if(attempt==2)return;
    }
    if(n<0)return;
    hop_xfer_task_id selected[2]={selected_task(0),selected_task(1)};
    int top[2]={
        (int)SendMessageW(s_task_lists[0],LB_GETTOPINDEX,0,0),
        (int)SendMessageW(s_task_lists[1],LB_GETTOPINDEX,0,0)
    };
    hop_xfer_status_pair pair;memset(&pair,0,sizeof(pair));xfer_get_status_pair(&pair);
    for(int d=0;d<2;++d){s_task_counts[d]=0;SendMessageW(s_task_lists[d],LB_RESETCONTENT,0,0);}
    for(int i=0;i<n;++i){
        hop_xfer_task_snapshot *t=&s_tasks_snapshot[i];
        int d=t->direction==HOP_XFER_DIR_SEND?0:t->direction==HOP_XFER_DIR_RECEIVE?1:-1;
        if(d<0 || s_task_counts[d]>=(int)UI_TASK_CAP)continue;
        if(t->state>=HOP_XFER_TASK_COMPLETED || t->kind==HOP_TASK_KIND_IMAGE_WORK)continue;
        hop_xfer_status current=d==0?pair.send:pair.receive;
        int current_batch=t->transfer_id && current.batch==t->transfer_id;
        wchar_t name[HOP_XFER_STATUS_NAME],row[512];
        utf8_to_wide(t->name,name,HOP_XFER_STATUS_NAME);
        if(!name[0] && current_batch)utf8_to_wide(current.name,name,HOP_XFER_STATUS_NAME);
        if(!name[0])swprintf(name,HOP_XFER_STATUS_NAME,L"任务 #%llu",(unsigned long long)t->id);
        uint32_t done=current_batch?current.files_done:t->files_done;
        uint32_t count=current_batch?current.files_total:t->files_total;
        uint64_t bytes_done=current_batch?current.bytes_done:t->bytes_done;
        uint64_t bytes_total=current_batch?current.bytes_total:t->bytes_total;
        double progress=bytes_total?100.0*(double)bytes_done/(double)bytes_total:0.0;
        if(current_batch||bytes_total)
            swprintf(row,512,L"%ls  ·  %ls  ·  %.0f%%  ·  文件 %u/%u  ·  %llu/%llu MiB",
                name,task_state_name(t),progress,done,count,
                (unsigned long long)(bytes_done/1048576ULL),(unsigned long long)(bytes_total/1048576ULL));
        else
            swprintf(row,512,L"%ls  ·  %ls  ·  %ls",name,task_state_name(t),
                t->state==HOP_XFER_TASK_SCANNING?L"正在准备":L"等待开始");
        LRESULT index=SendMessageW(s_task_lists[d],LB_ADDSTRING,0,(LPARAM)row);
        if(index==LB_ERR || index==LB_ERRSPACE)continue;
        s_task_ids[d][s_task_counts[d]++]=t->id;
        if(t->id==selected[d])SendMessageW(s_task_lists[d],LB_SETCURSEL,(WPARAM)index,0);
    }
    for(int d=0;d<2;++d){
        if(s_task_counts[d]==0)
            SendMessageW(s_task_lists[d],LB_ADDSTRING,0,(LPARAM)(d?L"暂无活动的接收任务":L"暂无活动的发送任务"));
        int rows=s_task_counts[d]>0?s_task_counts[d]:1;
        int restore=top[d]<0?0:top[d];if(restore>=rows)restore=rows-1;
        SendMessageW(s_task_lists[d],LB_SETTOPINDEX,(WPARAM)restore,0);
    }
    (void)total;
}
static void cancel_selected_task(int direction) {
    hop_xfer_task_id id=selected_task(direction);
    hop_xfer_task_snapshot task;
    if(!id || !hop_xfer_task_get_snapshot(id,&task))return;
    wchar_t name[HOP_XFER_STATUS_NAME],prompt[768];
    utf8_to_wide(task.name,name,HOP_XFER_STATUS_NAME);
    if(!name[0])swprintf(name,HOP_XFER_STATUS_NAME,L"任务 #%llu",(unsigned long long)id);
    if(xfer_connected())
        swprintf(prompt,768,L"确定取消“%ls”？\n\n当前状态：%ls。\n取消请求不可撤销；已经完成的文件不会回滚。",
            name,task_state_name(&task));
    else
        swprintf(prompt,768,L"确定取消“%ls”？\n\n当前状态：%ls。\n数据通道未连接，对端临时文件可能要等连接恢复或断线超时后清理。已经完成的文件不会回滚。",
            name,task_state_name(&task));
    if(MessageBoxW(s_main,prompt,L"取消传输任务",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    if(!xfer_cancel_task(id))
        MessageBoxW(s_main,L"任务已结束或正在完成，无法取消。",L"取消任务",MB_OK|MB_ICONINFORMATION);
    refresh_transfers();
}
static void refresh_task_history(void) {
    if(!s_task_history || !s_history_direction || !s_history_state || !s_history_order)return;
    static const int directions[]={HOP_XFER_DIR_NONE,HOP_XFER_DIR_SEND,HOP_XFER_DIR_RECEIVE};
    static const hop_xfer_task_state states[]={HOP_XFER_TASK_EMPTY,HOP_XFER_TASK_COMPLETED,
        HOP_XFER_TASK_PARTIAL,HOP_XFER_TASK_FAILED,HOP_XFER_TASK_CANCELLED,HOP_XFER_TASK_INTERRUPTED};
    int d=(int)SendMessageW(s_history_direction,CB_GETCURSEL,0,0);
    int st=(int)SendMessageW(s_history_state,CB_GETCURSEL,0,0);
    int order=(int)SendMessageW(s_history_order,CB_GETCURSEL,0,0);
    if(d<0 || d>2 || st<0 || st>5 || order<0 || order>1)return;
    hop_xfer_task_history_record history[HOP_TASK_HISTORY_CAPACITY];
    int total=0,n=hop_xfer_task_history_store_list(history,HOP_TASK_HISTORY_CAPACITY,directions[d],states[st],order==0,&total);
    if(n<0)return;
    int top=(int)SendMessageW(s_task_history,LB_GETTOPINDEX,0,0);
    SendMessageW(s_task_history,LB_RESETCONTENT,0,0);
    for(int i=0;i<n;++i){
        wchar_t row[512],name[HOP_XFER_STATUS_NAME];const hop_xfer_task_history_record *t=&history[i];
        hop_xfer_task_snapshot display={0};display.state=(hop_xfer_task_state)t->state;
        utf8_to_wide(t->name,name,HOP_XFER_STATUS_NAME);
        if(!name[0])swprintf(name,HOP_XFER_STATUS_NAME,L"任务 #%llu",(unsigned long long)t->record_id);
        swprintf(row,512,L"%ls  ·  %ls  ·  %ls  ·  文件 %u/%u  ·  %llu/%llu MiB",
            t->direction==HOP_XFER_DIR_SEND?L"发送":L"接收",name,task_state_name(&display),
            t->files_done,t->files_total,(unsigned long long)(t->bytes_done/1048576ULL),
            (unsigned long long)(t->bytes_total/1048576ULL));
        SendMessageW(s_task_history,LB_ADDSTRING,0,(LPARAM)row);
    }
    if(n==0)SendMessageW(s_task_history,LB_ADDSTRING,0,(LPARAM)L"暂无符合条件的传输记录");
    if(top>0 && top<n)SendMessageW(s_task_history,LB_SETTOPINDEX,(WPARAM)top,0);
    (void)total;
}
static int selected_visible_file_index(void) {
    int selected=s_xfer_files?(int)SendMessageW(s_xfer_files,LB_GETCURSEL,0,0):LB_ERR;
    if(selected==LB_ERR)return -1;
    int index=(int)SendMessageW(s_xfer_files,LB_GETITEMDATA,(WPARAM)selected,0);
    return index>=0 && index<HOP_XFER_FILE_LIMIT?index:-1;
}
static void update_transfer_actions(void) {
    int index=selected_visible_file_index();
    int can_retry=s_file_direction==HOP_XFER_DIR_SEND && !s_file_batch_active &&
        !(s_file_batch & HOP_XFER_PASTE_BATCH_FLAG) &&
        index>=0 && s_file_snapshot &&
        s_file_snapshot[index].phase==HOP_XFER_FILE_FAILED && !s_file_snapshot[index].is_directory;
    if(s_xfer_retry_button)EnableWindow(s_xfer_retry_button,can_retry);
    for(int d=0;d<2;++d){
        int can_cancel=0;
        hop_xfer_task_snapshot task;
        hop_xfer_task_id id=selected_task(d);
        if(id && hop_xfer_task_get_snapshot(id,&task))
            can_cancel=task.state<HOP_XFER_TASK_COMPLETED && !task.cancel_requested && !task.completing;
        if(s_cancel_task_buttons[d])EnableWindow(s_cancel_task_buttons[d],can_cancel);
    }
}
static int settings_require_restart(const hop_config *saved) {
    if (!saved) return 0;
    hop_config runtime = g_app.cfg, next = *saved;
    runtime.audio_mode = 0;
    next.audio_mode = 0;
    return memcmp(&runtime, &next, sizeof(runtime)) != 0;
}
static void refresh_transfers(void) {
    if(!s_xfer_title)return;
    refresh_task_lists();
    refresh_task_history();
    hop_xfer_status_pair pair;memset(&pair,0,sizeof(pair));xfer_get_status_pair(&pair);

    hop_xfer_task_snapshot selected_snapshot;memset(&selected_snapshot,0,sizeof(selected_snapshot));
    hop_xfer_task_id selected_id=selected_task(0);
    if(!selected_id)selected_id=selected_task(1);
    int has_selected=selected_id && hop_xfer_task_get_snapshot(selected_id,&selected_snapshot);
    int has_task_status=0;
    hop_xfer_status st=pair.primary_direction==HOP_XFER_DIR_SEND?pair.send:pair.receive;
    if(has_selected){
        hop_xfer_status candidate=selected_snapshot.direction==HOP_XFER_DIR_SEND?pair.send:pair.receive;
        if(selected_snapshot.transfer_id && candidate.batch==selected_snapshot.transfer_id){
            st=candidate;has_task_status=1;
        }else{
            memset(&st,0,sizeof(st));st.direction=selected_snapshot.direction;
        }
    }

    wchar_t name[HOP_XFER_STATUS_NAME],detail[HOP_XFER_STATUS_DETAIL],row[1800],path[1024];
    if(has_selected){
        utf8_to_wide(selected_snapshot.name,name,HOP_XFER_STATUS_NAME);
        if(!name[0] && has_task_status)utf8_to_wide(st.name,name,HOP_XFER_STATUS_NAME);
        if(!name[0])swprintf(name,HOP_XFER_STATUS_NAME,L"%ls任务 #%llu",
            selected_snapshot.direction==HOP_XFER_DIR_SEND?L"发送":L"接收",(unsigned long long)selected_id);
    }else{
        utf8_to_wide(st.name,name,HOP_XFER_STATUS_NAME);
        if(!name[0])wcscpy(name,L"暂无传输任务");
    }
    const wchar_t *phase=L"空闲";
    switch(st.phase){
        case HOP_XFER_QUEUED:phase=L"排队中";break;
        case HOP_XFER_SCANNING:phase=L"扫描中";break;
        case HOP_XFER_SENDING:phase=L"发送中";break;
        case HOP_XFER_RECEIVING:phase=L"接收中";break;
        case HOP_XFER_VERIFYING:phase=L"校验中";break;
        case HOP_XFER_COMPLETED:phase=L"已完成";break;
        case HOP_XFER_PARTIAL:phase=L"部分失败";break;
        case HOP_XFER_FAILED:phase=L"失败";break;
        case HOP_XFER_CANCELLED:phase=L"已取消";break;
    }
    if(has_selected && !has_task_status)phase=task_state_name(&selected_snapshot);
    const wchar_t *direction=has_selected?
        (selected_snapshot.direction==HOP_XFER_DIR_SEND?L"发送":L"接收"):
        (st.direction==HOP_XFER_DIR_SEND?L"发送":st.direction==HOP_XFER_DIR_RECEIVE?L"接收":L"待机");
    utf8_to_wide(has_task_status?st.detail:(has_selected?selected_snapshot.detail:""),detail,HOP_XFER_STATUS_DETAIL);
    if(!detail[0])wcscpy(detail,has_selected&&!has_task_status?L"实时进度将在任务开始后显示":L"");
    uint64_t now_ms=hop_monotonic_ms(),speed=has_task_status?st.speed_bytes_per_sec:0;
    if(!st.last_progress_ms || now_ms<st.last_progress_ms || now_ms-st.last_progress_ms>2000)speed=0;
    if(speed)swprintf(row,1800,L"%ls  ·  %ls  ·  %ls  ·  %.1f MiB/s  ·  ETA %llus",
        direction,phase,detail,(double)speed/1048576.0,(unsigned long long)st.eta_seconds);
    else swprintf(row,1800,L"%ls  ·  %ls  ·  %ls  ·  速率 --  ·  ETA --",direction,phase,detail);
    set_text(s_xfer_title,name);set_text(s_xfer_detail,row);

    uint32_t files_done=0,files_total=0,queued=0;
    uint64_t bytes_done=0,bytes_total=0;
    if(has_task_status){
        files_done=st.files_done;files_total=st.files_total;queued=st.queued_count;
        bytes_done=st.bytes_done;bytes_total=st.bytes_total;
    }else if(has_selected){
        files_done=selected_snapshot.files_done;files_total=selected_snapshot.files_total;
        bytes_done=selected_snapshot.bytes_done;bytes_total=selected_snapshot.bytes_total;
    }else{
        files_done=st.files_done;files_total=st.files_total;queued=st.queued_count;
        bytes_done=st.bytes_done;bytes_total=st.bytes_total;
    }
    int pct=bytes_total?(int)(100.0*(double)bytes_done/(double)bytes_total):
        (has_task_status&&st.phase==HOP_XFER_COMPLETED?100:0);
    if(pct>100)pct=100;
    if(has_selected&&!has_task_status&&!bytes_total&&!files_total)
        set_text(s_xfer_counts,L"任务正在准备或排队，开始后显示实时进度");
    else{
        swprintf(row,1800,L"%d%%  ·  文件 %u/%u    排队 %u    数据 %llu/%llu MiB",
            pct,files_done,files_total,queued,(unsigned long long)(bytes_done/1048576ULL),
            (unsigned long long)(bytes_total/1048576ULL));
        if(!has_selected && pair.send.active && pair.receive.active)
            swprintf(row,1800,L"双向传输  ·  发送 %llu/%llu MiB  ·  接收 %llu/%llu MiB  ·  排队 %u",
                (unsigned long long)(pair.send.bytes_done/1048576ULL),
                (unsigned long long)(pair.send.bytes_total/1048576ULL),
                (unsigned long long)(pair.receive.bytes_done/1048576ULL),
                (unsigned long long)(pair.receive.bytes_total/1048576ULL),pair.send.queued_count);
        set_text(s_xfer_counts,row);
    }
    SendMessageW(s_xfer_progress,PBM_SETPOS,pct,0);

    int file_direction=has_selected?selected_snapshot.direction:st.direction;
    uint32_t file_batch=has_selected?selected_snapshot.transfer_id:st.batch;
    s_file_batch_active=has_selected?(has_task_status&&st.active):st.active;
    /* File snapshots are display-only copies. Size them to this task's actual
     * entry count instead of reserving the maximum 8,192-record table. */
    int requested_file_count = 0;
    int file_details_available = !has_selected || has_task_status;
    if (file_details_available && file_direction != HOP_XFER_DIR_NONE && file_batch)
        requested_file_count = files_total > HOP_XFER_FILE_LIMIT ?
            HOP_XFER_FILE_LIMIT : (int)files_total;
    int snapshot_ready = resize_file_snapshot(requested_file_count);
    if (!snapshot_ready && s_file_snapshot &&
        s_file_snapshot_capacity >= requested_file_count)
        snapshot_ready = 1; /* keep rendering if a best-effort shrink failed */
    int n = snapshot_ready && requested_file_count > 0 ?
        xfer_get_files(file_direction,file_batch,s_file_snapshot,requested_file_count) : 0;
    if(s_file_batch!=file_batch || s_file_direction!=file_direction){
        memset(s_collapsed,0,sizeof(s_collapsed));s_file_batch=file_batch;s_file_direction=file_direction;
    }
    int old_selected=(int)SendMessageW(s_xfer_files,LB_GETCURSEL,0,0);
    int old_selected_index=old_selected==LB_ERR?-1:(int)SendMessageW(s_xfer_files,LB_GETITEMDATA,(WPARAM)old_selected,0);
    int old_top=(int)SendMessageW(s_xfer_files,LB_GETTOPINDEX,0,0);
    SendMessageW(s_xfer_files,LB_RESETCONTENT,0,0);s_visible_count=0;
    for(int i=0;i<n;++i){
        int hidden=0;
        for(int p=0;p<i;++p){
            if(!s_collapsed[p]||!s_file_snapshot[p].is_directory)continue;
            size_t len=strlen(s_file_snapshot[p].relative_path);
            if(!strncmp(s_file_snapshot[i].relative_path,s_file_snapshot[p].relative_path,len) &&
               s_file_snapshot[i].relative_path[len]=='/'){hidden=1;break;}
        }
        if(hidden)continue;
        utf8_to_wide(s_file_snapshot[i].relative_path,path,1024);
        int depth=0;for(const char *p=s_file_snapshot[i].relative_path;*p;++p)if(*p=='/')++depth;
        wchar_t indent[64];int spaces=depth*2;if(spaces>24)spaces=24;
        for(int j=0;j<spaces;++j)indent[j]=L' ';indent[spaces]=0;
        const wchar_t *state=s_file_snapshot[i].phase==HOP_XFER_FILE_COMPLETED?L"完成":
            s_file_snapshot[i].phase==HOP_XFER_FILE_FAILED?L"失败":
            s_file_snapshot[i].phase==HOP_XFER_FILE_SENDING?L"发送中":
            s_file_snapshot[i].phase==HOP_XFER_FILE_RECEIVING?L"接收中":
            s_file_snapshot[i].phase==HOP_XFER_FILE_VERIFYING?L"校验中":L"排队中";
        swprintf(row,1800,L"%ls%ls %ls   %llu / %llu B   %ls",indent,
            s_file_snapshot[i].is_directory?(s_collapsed[i]?L"[+目录]":L"[-目录]"):L"[文件]",
            path,(unsigned long long)s_file_snapshot[i].bytes_done,
            (unsigned long long)s_file_snapshot[i].bytes_total,state);
        LRESULT idx=SendMessageW(s_xfer_files,LB_ADDSTRING,0,(LPARAM)row);
        if(idx!=LB_ERR && idx!=LB_ERRSPACE){
            SendMessageW(s_xfer_files,LB_SETITEMDATA,(WPARAM)idx,(LPARAM)i);
            s_visible_indices[s_visible_count++]=i;
            if(i==old_selected_index)SendMessageW(s_xfer_files,LB_SETCURSEL,(WPARAM)idx,0);
        }
    }
    if(s_visible_count==0){
        const wchar_t *empty=has_selected?
            (selected_snapshot.state<=HOP_XFER_TASK_QUEUED?L"任务正在准备或排队，开始后显示文件明细":L"此任务的实时文件明细暂不可用"):
            L"当前没有文件明细";
        LRESULT idx=SendMessageW(s_xfer_files,LB_ADDSTRING,0,(LPARAM)empty);
        if(idx!=LB_ERR && idx!=LB_ERRSPACE)SendMessageW(s_xfer_files,LB_SETITEMDATA,(WPARAM)idx,(LPARAM)-1);
    }
    int fileRows=s_visible_count>0?s_visible_count:1;
    int restoreTop=old_top<0?0:old_top;if(restoreTop>=fileRows)restoreTop=fileRows-1;
    SendMessageW(s_xfer_files,LB_SETTOPINDEX,(WPARAM)restoreTop,0);
    update_transfer_actions();
}
static void create_settings_page(HWND p) {
    group(p,L"设备与连接",20,18,400,470);
    s_host_hint=label(p,L"服务器：向客户端提供键鼠；音频发送/播放独立配置。",38,72,360,18);
    set_font(s_host_hint,s_font_small);
    const int y[]={98,138,178,218,258,298,338,378};
    const int ids[]={CFG_NAME,CFG_ROLE,CFG_HOST,CFG_PORT,CFG_DATA_PORT,CFG_TOKEN,CFG_SERVER_EDGE,CFG_CLIENT_EDGE};
    const wchar_t *names[]={L"设备名称",L"本机角色",L"服务器地址",L"控制端口",L"数据端口",L"共享 Token",L"服务端边缘",L"客户端边缘"};
    for(int i=0;i<8;++i){
        HWND fieldLabel=label(p,names[i],38,y[i]+7,112,20);
        set_font(fieldLabel,s_font_small);
        if(ids[i]==CFG_HOST)s_host_label=fieldLabel;
        if(ids[i]==CFG_ROLE){
            s_cfg[CFG_ROLE]=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,156,y[i],242,120,UI_ROLE_ID);
            SendMessageW(s_cfg[CFG_ROLE],CB_ADDSTRING,0,(LPARAM)L"服务器 · 提供键鼠");
            SendMessageW(s_cfg[CFG_ROLE],CB_ADDSTRING,0,(LPARAM)L"客户端 · 接收键鼠");
        }else if(ids[i]==CFG_SERVER_EDGE||ids[i]==CFG_CLIENT_EDGE){
            int height=ids[i]==CFG_CLIENT_EDGE?110:120;
            s_cfg[ids[i]]=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,156,y[i],242,height,0);
            SendMessageW(s_cfg[ids[i]],CB_ADDSTRING,0,(LPARAM)L"左边缘");
            SendMessageW(s_cfg[ids[i]],CB_ADDSTRING,0,(LPARAM)L"右边缘");
            SendMessageW(s_cfg[ids[i]],CB_ADDSTRING,0,(LPARAM)L"上边缘");
            SendMessageW(s_cfg[ids[i]],CB_ADDSTRING,0,(LPARAM)L"下边缘");
        }else{
            s_cfg[ids[i]]=edit(p,L"",156,y[i],242,28,0,ids[i]==CFG_TOKEN);
            if(ids[i]==CFG_HOST)SendMessageW(s_cfg[CFG_HOST],EM_SETCUEBANNER,TRUE,(LPARAM)L"服务器模式无需填写");
        }
    }
    HWND audioTitle=label(p,L"音频能力（独立于本机角色）",38,416,360,20);
    set_font(audioTitle,s_font_small);
    s_audio_send=child(p,L"BUTTON",L"发送本机系统声音",BS_AUTOCHECKBOX|WS_TABSTOP,38,440,174,22,UI_ID_AUDIO_SEND);
    s_audio_play=child(p,L"BUTTON",L"播放对端系统声音",BS_AUTOCHECKBOX|WS_TABSTOP,220,440,178,22,UI_ID_AUDIO_PLAY);
    set_font(s_audio_send,s_font_small);set_font(s_audio_play,s_font_small);
    s_audio_status=label(p,L"音频已关闭",38,466,360,16);set_font(s_audio_status,s_font_small);

    group(p,L"交互与显示器",440,18,400,470);
    HWND displayHelp=label(p,L"按需调整跨屏切换行为；输入源编号支持十六进制。",458,72,362,18);
    set_font(displayHelp,s_font_small);
    const int settingIDs[]={CFG_SWITCH_DELAY,CFG_COOLDOWN,CFG_MARGIN,CFG_DISP_WIN,CFG_DISP_MAC,CFG_WAKE,CFG_CHUNK};
    const wchar_t *settingNames[]={L"边缘触发延迟（ms）",L"切换冷却（ms）",L"边缘内缩（px）",
        L"Windows 输入源",L"Mac 输入源",L"唤醒时长（秒）",L"传输分块（KB）"};
    const int ry[]={98,138,178,218,258,298,338};
    for(int i=0;i<7;++i){
        HWND fieldLabel=label(p,settingNames[i],458,ry[i]+7,170,20);
        set_font(fieldLabel,s_font_small);
        s_cfg[settingIDs[i]]=edit(p,L"",630,ry[i],190,28,0,0);
    }
    HWND inputCommandLabel=label(p,L"Mac 切源命令",458,372,160,20);
    set_font(inputCommandLabel,s_font_small);
    s_cfg[CFG_MAC_INPUT]=edit(p,L"",458,396,360,28,0,0);
    HWND sleepCommandLabel=label(p,L"Mac 睡眠命令",458,432,160,20);
    set_font(sleepCommandLabel,s_font_small);
    s_cfg[CFG_MAC_SLEEP]=edit(p,L"",458,454,360,28,0,0);

    group(p,L"",20,506,840,104);
    label(p,L"应用设置",38,508,360,20);
    HWND saveHelp=label(p,L"音频保存后立即应用；网络、认证和角色参数重启后生效。",38,560,560,18);
    set_font(saveHelp,s_font_small);
    s_cfg_note=label(p,L"设置已加载。修改后保存。",38,586,560,18);
    set_font(s_cfg_note,s_font_small);
    s_cfg_save_button=button(p,L"保存设置",630,558,112,36,UI_ID_SAVE);
    s_cfg_reset_button=button(p,L"恢复默认",758,558,92,36,UI_ID_RESET);
}
static void create_pages(HWND h) {
    for(int i=0;i<UI_PAGE_COUNT;++i) s_pages[i]=page_create(h,i);
    HWND p=s_pages[UI_PAGE_OVERVIEW];
    group(p,L"连接概览",20,18,800,150);
    s_overview_status=label(p,L"等待连接",42,54,560,30);set_font(s_overview_status,s_font_title);
    s_overview_peer=label(p,L"设备与对端信息",42,96,560,22);
    button(p,L"重新连接",650,58,140,36,UI_ID_RECONNECT);

    group(p,L"控制通道",20,188,250,104);
    s_overview_control=label(p,L"等待连接",38,238,214,26);
    group(p,L"数据通道",295,188,250,104);
    s_overview_data=label(p,L"等待连接",313,238,214,26);
    group(p,L"键鼠共享",570,188,250,104);
    s_overview_sharing=label(p,L"等待连接",588,238,214,26);

    group(p,L"显示器控制",20,310,800,160);
    button(p,L"显示器切至 Mac",42,366,150,36,UI_ID_SWITCH_MAC);
    button(p,L"显示器切至 Windows",208,366,160,36,UI_ID_SWITCH_WIN);
    button(p,L"唤醒显示",384,366,120,36,UI_ID_WAKE);
    button(p,L"显示睡眠",520,366,120,36,UI_ID_SLEEP);
    group(p,L"常用入口",20,488,800,112);
    button(p,L"打开传输管理",42,530,160,38,UI_ID_OPEN_TRANSFERS);
    label(p,L"查看活动任务、实时进度、取消操作和历史记录。",228,536,540,24);

    p=s_pages[UI_PAGE_DEVICE];
    group(p,L"连接状态",20,18,800,235);
    s_connection_role=label(p,L"本机角色：Windows 服务端",42,58,720,24);
    s_connection_status=label(p,L"服务状态：等待连接",42,98,720,24);
    s_connection_peer=label(p,L"对端设备：尚未取得设备信息",42,138,720,24);
    s_connection_ports=label(p,L"监听端口：24817（控制） / 24818（数据）",42,178,720,24);
    button(p,L"重新连接",42,210,130,34,UI_ID_RECONNECT);
    label(p,L"HopDesk 会自动重连。点击“重新连接”会关闭当前控制连接，并立即尝试重新建立连接。",42,280,760,40);

    p=s_pages[UI_PAGE_TRANSFER];
    group(p,L"当前传输",20,14,800,164);
    s_xfer_title=label(p,L"暂无传输任务",40,42,740,26);set_font(s_xfer_title,s_font_title);
    s_xfer_detail=label(p,L"等待文件传输",40,72,740,20);
    s_xfer_counts=label(p,L"0%  ·  文件 0/0    排队 0    数据 0/0 MiB",40,98,740,20);
    s_xfer_progress=child(p,PROGRESS_CLASSW,L"",PBS_SMOOTH,40,132,740,12,0);

    group(p,L"活动任务",20,196,800,220);
    label(p,L"发送任务",40,228,350,20);
    label(p,L"接收任务",430,228,350,20);
    s_task_lists[0]=child(p,L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS|WS_TABSTOP,40,252,350,112,UI_ID_SEND_TASKS);
    s_task_lists[1]=child(p,L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS|WS_TABSTOP,430,252,350,112,UI_ID_RECEIVE_TASKS);
    s_cancel_task_buttons[0]=button(p,L"取消选中发送任务",40,374,180,34,UI_ID_SEND_CANCEL);
    s_cancel_task_buttons[1]=button(p,L"取消选中接收任务",430,374,180,34,UI_ID_RECEIVE_CANCEL);
    EnableWindow(s_cancel_task_buttons[0],FALSE);EnableWindow(s_cancel_task_buttons[1],FALSE);

    group(p,L"文件明细",20,434,800,236);
    s_xfer_files=child(p,L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS|WS_TABSTOP,40,472,760,150,UI_ID_FILES);
    button(p,L"展开/折叠目录",470,632,140,34,UI_ID_TOGGLE);
    s_xfer_retry_button=button(p,L"重试选中文件",630,632,150,34,UI_ID_XFER_RETRY);
    EnableWindow(s_xfer_retry_button,FALSE);

    group(p,L"传输历史 · 最多保留 256 条",20,688,800,240);
    s_history_direction=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,40,724,150,180,0);
    s_history_state=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,205,724,170,200,0);
    s_history_order=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,390,724,170,160,0);
    const wchar_t *directions[]={L"全部方向",L"发送",L"接收"};
    const wchar_t *states[]={L"全部状态",L"已完成",L"部分完成",L"失败",L"已取消",L"已中断"};
    const wchar_t *orders[]={L"最近完成优先",L"最早完成优先"};
    for(int i=0;i<3;++i)SendMessageW(s_history_direction,CB_ADDSTRING,0,(LPARAM)directions[i]);
    for(int i=0;i<6;++i)SendMessageW(s_history_state,CB_ADDSTRING,0,(LPARAM)states[i]);
    for(int i=0;i<2;++i)SendMessageW(s_history_order,CB_ADDSTRING,0,(LPARAM)orders[i]);
    SendMessageW(s_history_direction,CB_SETCURSEL,0,0);SendMessageW(s_history_state,CB_SETCURSEL,0,0);SendMessageW(s_history_order,CB_SETCURSEL,0,0);
    s_task_history=child(p,L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS|WS_TABSTOP,40,760,760,148,0);
    RECT page_rect;GetClientRect(p,&page_rect);
    SCROLLINFO task_scroll={sizeof(task_scroll),SIF_RANGE|SIF_PAGE,0,dip(960),(UINT)page_rect.bottom,0,0};SetScrollInfo(p,SB_VERT,&task_scroll,TRUE);

    p=s_pages[UI_PAGE_SETTINGS]; create_settings_page(p);
    group(p,L"运行日志",20,620,800,590);
    s_log_note=label(p,L"尚未读取",620,626,180,20);set_font(s_log_note,s_font_small);
    s_log_edit=child(p,L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL,40,654,760,500,0);
    SendMessageW(s_log_edit,EM_SETLIMITTEXT,512*1024,0);
    button(p,L"刷新日志",560,1160,110,34,UI_ID_LOG_REFRESH);
    button(p,L"复制日志",690,1160,110,34,UI_ID_LOG_COPY);

    group(p,L"软件更新",20,1240,800,300);
    wchar_t version[80];swprintf(version,80,L"当前版本：v%hs",HOP_VERSION);
    s_update_version=label(p,version,42,1280,740,28);set_font(s_update_version,s_font_title);
    s_update_source=label(p,L"更新源：GitHub Releases",42,1324,740,24);
    label(p,L"检查更新会读取公开更新清单。发现新版本后，可下载、校验并自动安装。",42,1368,740,44);
    button(p,L"检查更新",42,1430,140,36,UI_ID_UPDATE);
    label(p,L"如果更新源尚未发布 latest.txt，检查会明确提示读取失败，不会将其误报为“已是最新版本”。",42,1480,740,48);

    for(int i=0;i<UI_PAGE_COUNT;++i) ShowWindow(s_pages[i],SW_HIDE);
    populate_settings();
}
static void layout_nodes(void){
    for(int i=0;i<s_node_count;++i){ui_node *n=&s_nodes[i];if(!IsWindow(n->hwnd))continue;
        HWND parent=GetParent(n->hwnd);int scroll=parent==s_main?0:GetScrollPos(parent,SB_VERT);
        MoveWindow(n->hwnd,dip(n->x),dip(n->y)-scroll,dip(n->w),dip(n->h),TRUE);
    }
}
static void layout_main(HWND h) {
    RECT r; GetClientRect(h,&r);int w=r.right,height=r.bottom;
    for(int i=0;i<UI_PAGE_COUNT;++i)if(s_pages[i])MoveWindow(s_pages[i],dip(228),dip(88),w-dip(248),height-dip(124),TRUE);
    layout_nodes();
    if(s_footer)MoveWindow(s_footer,dip(250),height-dip(28),w-dip(270),dip(22),TRUE);
    if(s_header_status)MoveWindow(s_header_status,w-dip(320),dip(30),dip(290),dip(26),TRUE);
}
static void create_main_controls(HWND h) {
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    s_font=CreateFontW(-dip(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    s_font_title=CreateFontW(-dip(23),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    s_font_small=CreateFontW(-dip(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    HWND brand=label(h,L"HopDesk",24,22,170,36); set_font(brand,s_font_title);
    label(h,L"跨设备协作",26,58,160,20);
    for(int i=0;i<UI_PAGE_COUNT;++i){
        s_nav[i]=child(h,L"BUTTON",kPageNames[i],BS_OWNERDRAW|WS_TABSTOP,20,112+i*48,180,40,UI_NAV_BASE+i);
        set_font(s_nav[i],s_font);
    }
    s_title=label(h,L"概览",250,22,400,40); set_font(s_title,s_font_title);
    s_header_status=label(h,L"等待连接",790,30,300,26);
    s_footer=label(h,L"HopDesk",228,0,850,22); set_font(s_footer,s_font_small);
    create_pages(h);
    layout_main(h);
}
static LRESULT CALLBACK main_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg){
        case WM_CREATE:
            create_main_controls(h);
            SetTimer(h,UI_TIMER,500,NULL);
            return 0;
        case WM_SIZE:
            layout_main(h); return 0;
        case WM_CTLCOLORSTATIC:{
            HWND control=(HWND)lp;SetBkMode((HDC)wp,TRANSPARENT);
            if(control==s_header_status || control==s_overview_status)
                SetTextColor((HDC)wp,g_app.state==HOP_LINK_SHARING?RGB(36,111,190):
                    g_app.state==HOP_LINK_LOCAL?RGB(27,132,82):RGB(112,122,137));
            else SetTextColor((HDC)wp,RGB(48,58,73));
            return (LRESULT)s_canvas;
        }
        case WM_DRAWITEM:{
            DRAWITEMSTRUCT *d=(DRAWITEMSTRUCT*)lp;int index=(int)d->CtlID-UI_NAV_BASE;
            if(index>=0&&index<UI_PAGE_COUNT){
                COLORREF bg=index==s_page?RGB(229,240,252):RGB(249,250,252);
                HBRUSH brush=CreateSolidBrush(bg);HPEN pen=CreatePen(PS_SOLID,1,bg);
                HGDIOBJ old_brush=SelectObject(d->hDC,brush),old_pen=SelectObject(d->hDC,pen);
                RoundRect(d->hDC,d->rcItem.left+1,d->rcItem.top+1,d->rcItem.right-1,d->rcItem.bottom-1,dip(9),dip(9));
                SelectObject(d->hDC,old_brush);SelectObject(d->hDC,old_pen);DeleteObject(brush);DeleteObject(pen);
                RECT r=d->rcItem;r.left+=dip(18);SetBkMode(d->hDC,TRANSPARENT);
                SetTextColor(d->hDC,index==s_page?RGB(30,101,180):RGB(75,86,103));
                SelectObject(d->hDC,s_font);DrawTextW(d->hDC,kPageNames[index],-1,&r,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
                if(d->itemState&ODS_FOCUS)DrawFocusRect(d->hDC,&d->rcItem);
                return TRUE;
            }
            if(d->CtlType==ODT_BUTTON){paint_action_button(d);return TRUE;}
            break;
        }
        case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)wp,&r,s_canvas);r.right=dip(216);FillRect((HDC)wp,&r,s_sidebar);return 1;}
        case WM_DPICHANGED:{
            UINT old_dpi=s_dpi;s_dpi=HIWORD(wp);HFONT old[3]={s_font,s_font_title,s_font_small};
            s_font=CreateFontW(-dip(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            s_font_title=CreateFontW(-dip(23),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            s_font_small=CreateFontW(-dip(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            HFONT fonts[3]={s_font,s_font_title,s_font_small};for(int i=0;i<s_node_count;++i)SendMessageW(s_nodes[i].hwnd,WM_SETFONT,(WPARAM)fonts[s_nodes[i].font],TRUE);
            RECT *r=(RECT*)lp;SetWindowPos(h,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            layout_main(h);for(int i=0;i<3;++i)DeleteObject(old[i]);(void)old_dpi;return 0;
        }
        case WM_GETMINMAXINFO:{
            MINMAXINFO *m=(MINMAXINFO *)lp;
            m->ptMinTrackSize.x=dip(1140); m->ptMinTrackSize.y=dip(620);
            return 0;
        }
        case WM_TIMER:
            if(wp==UI_TIMER){refresh_connection(); if(s_page==UI_PAGE_TRANSFER)refresh_transfers(); return 0;}
            break;
        case WM_COMMAND:{
            int id=LOWORD(wp);
            HWND source=(HWND)lp;
            if(id==UI_ROLE_ID && HIWORD(wp)==CBN_SELCHANGE){
                update_settings_role_fields();mark_settings_dirty();return 0;
            }
            if(!s_populating_settings && HIWORD(wp)==EN_CHANGE){
                for(int i=0;i<CFG_COUNT;++i)if(source&&source==s_cfg[i]){mark_settings_dirty();break;}
            }
            if(!s_populating_settings && HIWORD(wp)==BN_CLICKED &&
                (source==s_audio_send||source==s_audio_play))mark_settings_dirty();
            if(!s_populating_settings && HIWORD(wp)==CBN_SELCHANGE &&
                (source==s_cfg[CFG_SERVER_EDGE]||source==s_cfg[CFG_CLIENT_EDGE]))mark_settings_dirty();
            if(id>=UI_NAV_BASE && id<UI_NAV_BASE+UI_PAGE_COUNT){show_page(id-UI_NAV_BASE);return 0;}
            switch(id){
                case UI_ID_SAVE: save_settings(); return 0;
                case UI_ID_RESET: reset_settings(); return 0;
                case UI_ID_RECONNECT: if(g_app.net)net_server_drop(g_app.net); return 0;
                case UI_ID_SEND_CANCEL: cancel_selected_task(0); return 0;
                case UI_ID_RECEIVE_CANCEL: cancel_selected_task(1); return 0;
                case UI_ID_SEND_TASKS:
                    if(HIWORD(wp)==LBN_SELCHANGE){SendMessageW(s_task_lists[1],LB_SETCURSEL,(WPARAM)-1,0);refresh_transfers();}
                    return 0;
                case UI_ID_RECEIVE_TASKS:
                    if(HIWORD(wp)==LBN_SELCHANGE){SendMessageW(s_task_lists[0],LB_SETCURSEL,(WPARAM)-1,0);refresh_transfers();}
                    return 0;
                case UI_ID_XFER_RETRY:{
                    int index=selected_visible_file_index();
                    if(index>=0 && s_file_direction==HOP_XFER_DIR_SEND && !xfer_retry_file(s_file_batch,index))
                        MessageBoxW(s_main,L"无法重试：任务可能仍在进行、文件并非失败状态，或这是跨端粘贴任务。跨端粘贴请在目标设备确认剪贴板后重新发起粘贴。",L"无法重试",MB_OK|MB_ICONINFORMATION);
                    return 0;
                }
                case UI_ID_TOGGLE:{
                    int index=selected_visible_file_index();
                    if(index>=0 && s_file_snapshot && s_file_snapshot[index].is_directory){
                        s_collapsed[index]=!s_collapsed[index];refresh_transfers();
                    }
                    return 0;
                }
                case UI_ID_FILES:
                    if(HIWORD(wp)==LBN_SELCHANGE){update_transfer_actions();return 0;}
                    if(HIWORD(wp)==LBN_DBLCLK){
                        int selected=(int)SendMessageW(s_xfer_files,LB_GETCURSEL,0,0);
                        if(selected!=LB_ERR){
                            int index=(int)SendMessageW(s_xfer_files,LB_GETITEMDATA,(WPARAM)selected,0);
                            if(index>=0 && index<HOP_XFER_FILE_LIMIT && s_file_snapshot && s_file_snapshot[index].is_directory){
                                s_collapsed[index]=!s_collapsed[index]; refresh_transfers();
                            }
                        }
                    } return 0;
                case UI_ID_LOG_REFRESH: refresh_logs(); return 0;
                case UI_ID_LOG_COPY:{
                    int copied=copy_logs_to_clipboard();
                    set_log_note(copied?L"日志已复制到剪贴板":L"复制失败，请重试",!copied);
                    return 0;
                }
                case UI_ID_UPDATE: update_check_async(s_main); return 0;
                case UI_ID_OPEN_TRANSFERS: show_page(UI_PAGE_TRANSFER); return 0;
                case UI_ID_SWITCH_MAC: SendMessageW(g_app.hwnd,WM_COMMAND,1003,0); return 0;
                case UI_ID_SWITCH_WIN: SendMessageW(g_app.hwnd,WM_COMMAND,1004,0); return 0;
                case UI_ID_WAKE: SendMessageW(g_app.hwnd,WM_COMMAND,1009,0); return 0;
                case UI_ID_SLEEP: SendMessageW(g_app.hwnd,WM_COMMAND,1005,0); return 0;
            }
            break;
        }
        case UI_LOG_RESULT:{
            s_log_busy=0;
            const wchar_t *text=lp?(const wchar_t*)lp:L"读取日志失败。";
            set_text(s_log_edit,text);
            if(!wcscmp(text,L"暂无日志记录。"))set_log_note(L"暂无日志记录",0);
            else if(log_result_is_error(text))set_log_note(L"读取失败，请重试",1);
            else set_log_note(L"日志已更新",0);
            free((void*)lp);return 0;
        }
        case UI_SAVE_RESULT:{
            save_result *result=(save_result*)lp;s_save_busy=0;
            set_settings_editors_enabled(1);
            if(result->ok){
                s_pending_restart=settings_require_restart(&result->config);
                s_draft=result->config;s_settings_dirty=0;
                g_app.cfg.audio_mode=result->config.audio_mode;
                if(hop_audio_set_mode(&g_app.cfg,g_app.cfg.audio_mode))
                    set_settings_note(s_pending_restart?L"设置已保存。音频已立即应用；网络、认证和角色参数重启后生效。":L"设置已保存，音频设置已立即应用。",0);
                else
                    set_settings_note(L"设置已保存，但音频后台未能启动。请查看日志；重启后会按已保存配置重试。",1);
                refresh_audio_status();
            } else {
                s_settings_dirty=1;
                set_settings_note(L"保存失败；原配置未改变。请检查文件权限和磁盘空间后重试。",1);
            }
            update_settings_actions();
            free(result);refresh_connection();return 0;
        }
        case WM_CLOSE:
            ShowWindow(h,SW_HIDE);
            KillTimer(h,UI_TIMER);
            return 0;
        case WM_DESTROY:
            KillTimer(h,UI_TIMER);
            if(s_file_snapshot){free(s_file_snapshot);s_file_snapshot=NULL;}
            s_file_snapshot_capacity=0;
            if(s_tasks_snapshot){free(s_tasks_snapshot);s_tasks_snapshot=NULL;}
            s_tasks_snapshot_capacity=0;
            if(s_font)DeleteObject(s_font);
            if(s_font_title)DeleteObject(s_font_title);
            if(s_font_small)DeleteObject(s_font_small);
            s_main=NULL;
            return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

void main_ui_show_page(int page) {
    if(!s_main){
        s_node_count=0;s_card_count=0;
        s_draft=g_app.cfg;s_canvas=CreateSolidBrush(RGB(246,248,251));s_sidebar=CreateSolidBrush(RGB(250,251,253));
        HDC dc=GetDC(NULL);s_dpi=(UINT)GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(NULL,dc);
        char addresses[512];local_addresses(addresses,sizeof(addresses));utf8_to_wide(addresses,s_addresses,512);
        WNDCLASSW wc; memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=main_wnd_proc; wc.hInstance=g_app.inst;
        wc.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); wc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName=L"HopDeskMainWindow";
        RegisterClassW(&wc);
        WNDCLASSW pc; memset(&pc,0,sizeof(pc));
        pc.lpfnWndProc=page_proc; pc.hInstance=g_app.inst;
        pc.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); pc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        pc.lpszClassName=L"HopDeskMainPage";
        RegisterClassW(&pc);
        s_main=CreateWindowExW(WS_EX_APPWINDOW,L"HopDeskMainWindow",L"HopDesk",
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,CW_USEDEFAULT,CW_USEDEFAULT,dip(1140),dip(800),
            NULL,NULL,g_app.inst,NULL);
    }
    if(!s_main)return;
    show_page(page);
    RECT work,wr;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);GetWindowRect(s_main,&wr);
    if(wr.bottom-wr.top>work.bottom-work.top)SetWindowPos(s_main,NULL,work.left,work.top,wr.right-wr.left,work.bottom-work.top,SWP_NOZORDER);
    ShowWindow(s_main,SW_SHOWNORMAL);
    SetForegroundWindow(s_main);
    SetTimer(s_main,UI_TIMER,500,NULL);
    refresh_connection();
}
void main_ui_refresh(void) {
    if(s_main && IsWindowVisible(s_main)) refresh_connection();
}
void main_ui_shutdown(void) {
    if(s_main) DestroyWindow(s_main);
    s_main=NULL;
}

int main_ui_translate(MSG *message){return s_main&&IsWindowVisible(s_main)&&IsDialogMessageW(s_main,message);}
