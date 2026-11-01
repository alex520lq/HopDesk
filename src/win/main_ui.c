/* =====================================================================
 * HopDesk - Windows native main window
 * A single persistent Win32 window; tray menu only opens it or exits.
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "update.h"
#include "../core/xfer_status.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define UI_PAGE_COUNT 6
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

enum {
    CFG_NAME, CFG_HOST, CFG_PORT, CFG_DATA_PORT, CFG_TOKEN,
    CFG_SERVER_EDGE, CFG_CLIENT_EDGE, CFG_SWITCH_DELAY, CFG_COOLDOWN,
    CFG_MARGIN, CFG_DISP_WIN, CFG_DISP_MAC, CFG_WAKE, CFG_CHUNK,
    CFG_MAC_INPUT, CFG_MAC_SLEEP, CFG_COUNT
};

static HWND s_main;
static HWND s_pages[UI_PAGE_COUNT];
static HWND s_nav[UI_PAGE_COUNT];
static HWND s_title, s_header_status, s_footer;
static HWND s_overview_status, s_overview_peer, s_overview_channels, s_overview_sharing;
static HWND s_connection_role, s_connection_status, s_connection_peer, s_connection_ports;
static HWND s_cfg[CFG_COUNT], s_cfg_note;
static HWND s_xfer_title, s_xfer_detail, s_xfer_counts, s_xfer_progress, s_xfer_files, s_xfer_history;
static HWND s_log_edit;
static HWND s_update_version, s_update_source;
static HFONT s_font, s_font_title, s_font_small;
static int s_page;
static hop_xfer_file_status *s_file_snapshot;
static int s_collapsed[HOP_XFER_FILE_LIMIT];
static int s_visible_indices[HOP_XFER_FILE_LIMIT];
static int s_visible_count;
static uint32_t s_file_batch;
static int s_file_direction;

static const wchar_t *const kPageNames[UI_PAGE_COUNT] = {
    L"概览", L"连接", L"设置", L"传输", L"日志", L"更新"
};

static HWND child(HWND parent, const wchar_t *klass, const wchar_t *text,
                  DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             g_app.inst, NULL);
    if (c && s_font) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}
static HWND label(HWND p, const wchar_t *text, int x, int y, int w, int h) {
    return child(p, L"STATIC", text, SS_LEFT, x, y, w, h, 0);
}
static HWND button(HWND p, const wchar_t *text, int x, int y, int w, int h, int id) {
    return child(p, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
}
static HWND edit(HWND p, const wchar_t *text, int x, int y, int w, int h, int id, int password) {
    return child(p, L"EDIT", text, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP |
                 (password ? ES_PASSWORD : 0), x, y, w, h, id);
}
static HWND group(HWND p, const wchar_t *text, int x, int y, int w, int h) {
    return child(p, L"BUTTON", text, BS_GROUPBOX, x, y, w, h, 0);
}
static HWND page_create(HWND parent, int index) {
    wchar_t cls[32];
    (void)index;
    wcscpy(cls, L"HopDeskMainPage");
    return CreateWindowExW(WS_EX_CONTROLPARENT, cls, L"",
        WS_CHILD | WS_CLIPCHILDREN, 228, 88, 860, 620,
        parent, NULL, g_app.inst, NULL);
}
static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        SendMessageW(GetParent(h), WM_COMMAND, w, l);
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}
static void set_text(HWND h, const wchar_t *s) {
    if (h) SetWindowTextW(h, s ? s : L"");
}
static void set_font(HWND h, HFONT f) {
    if (h && f) SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
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
    if (!h) return fallback;
    GetWindowTextW(h, b, 64);
    if (!b[0]) return fallback;
    unsigned long v = wcstoul(b, &end, 0);
    return end == b || *end ? fallback : v;
}
static void show_page(int page) {
    if (page < 0 || page >= UI_PAGE_COUNT) page = 0;
    s_page = page;
    for (int i = 0; i < UI_PAGE_COUNT; ++i) {
        if (s_pages[i]) ShowWindow(s_pages[i], i == page ? SW_SHOW : SW_HIDE);
        if (s_nav[i]) {
            EnableWindow(s_nav[i], i != page);
            SetWindowTextW(s_nav[i], i == page ? L"●  " : L"    ");
            wchar_t text[48];
            swprintf(text, 48, L"%ls%ls", i == page ? L"●  " : L"    ", kPageNames[i]);
            SetWindowTextW(s_nav[i], text);
        }
    }
    set_text(s_title, kPageNames[page]);
    if (page == 3) refresh_transfers();
    if (page == 4) refresh_logs();
}
static const wchar_t *link_state(void) {
    if (!g_app.net) return L"网络服务未启动";
    if (!net_server_listening(g_app.net) || !xfer_listening()) return L"端口不可用，正在重试";
    if (g_app.state == HOP_LINK_SHARING) return L"键鼠正在 Mac";
    if (g_app.state == HOP_LINK_LOCAL) return L"已连接";
    return L"等待 Mac 连接";
}
static void refresh_connection(void) {
    wchar_t b[512];
    set_text(s_header_status, link_state());
    set_text(s_overview_status, link_state());
    swprintf(b, 512, L"Windows 服务端  ·  控制端口 %u  ·  数据端口 %u",
             (unsigned)g_app.cfg.port, (unsigned)g_app.cfg.data_port);
    set_text(s_overview_peer, b);
    swprintf(b, 512, L"控制通道：%ls    数据通道：%ls",
             g_app.state == HOP_LINK_DOWN ? L"等待连接" : L"已连接",
             xfer_connected() ? L"已连接" : L"等待连接");
    set_text(s_overview_channels, b);
    set_text(s_overview_sharing, g_app.state == HOP_LINK_SHARING ? L"键鼠共享：已切换到 Mac" :
             g_app.state == HOP_LINK_LOCAL ? L"键鼠共享：可切换到 Mac" : L"键鼠共享：等待连接");
    set_text(s_connection_role, L"本机角色：Windows 服务端");
    set_text(s_connection_status, link_state());
    if (g_app.peer_w > 0 && g_app.peer_h > 0)
        swprintf(b, 512, L"Mac 屏幕：%d × %d", g_app.peer_w, g_app.peer_h);
    else wcscpy(b, L"Mac 屏幕：尚未取得设备信息");
    set_text(s_connection_peer, b);
    swprintf(b, 512, L"监听端口：%u（控制） / %u（数据）",
             (unsigned)g_app.cfg.port, (unsigned)g_app.cfg.data_port);
    set_text(s_connection_ports, b);
    swprintf(b, 512, L"HopDesk  v%s    ·    %ls", HOP_VERSION, link_state());
    set_text(s_footer, b);
}
static void refresh_logs(void) {
    if (!s_log_edit) return;
    if (!g_app.log_path[0]) { set_text(s_log_edit, L"日志路径尚未初始化。"); return; }
    wchar_t path[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, g_app.log_path, -1, path, 1024)) {
        set_text(s_log_edit, L"日志路径无法转换。"); return;
    }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { set_text(s_log_edit, L"暂无日志记录。"); return; }
    LARGE_INTEGER size, zero; zero.QuadPart = 0;
    if (!GetFileSizeEx(f, &size)) { CloseHandle(f); set_text(s_log_edit, L"无法读取日志大小。"); return; }
    const DWORD maxBytes = 256u * 1024u;
    DWORD take = (DWORD)(size.QuadPart > maxBytes ? maxBytes : size.QuadPart);
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - take;
    SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
    char *bytes = (char *)malloc((size_t)take + 1);
    if (!bytes) { CloseHandle(f); set_text(s_log_edit, L"内存不足，无法读取日志。"); return; }
    DWORD got = 0;
    BOOL ok = ReadFile(f, bytes, take, &got, NULL);
    CloseHandle(f);
    if (!ok) { free(bytes); set_text(s_log_edit, L"读取日志失败。"); return; }
    bytes[got] = 0;
    if (pos.QuadPart > 0) {
        char *nl = memchr(bytes, '\n', got);
        if (nl) { size_t skip = (size_t)(nl - bytes) + 1; memmove(bytes, bytes + skip, got - skip + 1); got -= (DWORD)skip; }
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, NULL, 0);
    if (needed <= 0) { free(bytes); set_text(s_log_edit, L"日志编码无法识别。"); return; }
    wchar_t *wide = (wchar_t *)calloc((size_t)needed + 1, sizeof(wchar_t));
    if (!wide) { free(bytes); set_text(s_log_edit, L"内存不足，无法显示日志。"); return; }
    MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, wide, needed);
    set_text(s_log_edit, wide);
    SendMessageW(s_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(s_log_edit, EM_SCROLLCARET, 0, 0);
    free(wide); free(bytes);
}
static void populate_settings(void) {
    hop_config *c = &g_app.cfg;
    wchar_t b[128];
    utf8_to_wide(c->device_name, b, 128); set_text(s_cfg[CFG_NAME], b);
    if (c->peer_host[0]) { utf8_to_wide(c->peer_host, b, 128); set_text(s_cfg[CFG_HOST], b); }
    else set_text(s_cfg[CFG_HOST], L"");
    set_int(s_cfg[CFG_PORT], c->port);
    set_int(s_cfg[CFG_DATA_PORT], c->data_port);
    utf8_to_wide(c->token, b, 128); set_text(s_cfg[CFG_TOKEN], b);
    SendMessageW(s_cfg[CFG_SERVER_EDGE], CB_SETCURSEL, c->server_edge == HOP_EDGE_RIGHT ? 1 : 0, 0);
    SendMessageW(s_cfg[CFG_CLIENT_EDGE], CB_SETCURSEL, c->client_edge == HOP_EDGE_RIGHT ? 1 : 0, 0);
    set_int(s_cfg[CFG_SWITCH_DELAY], c->switch_delay_ms);
    set_int(s_cfg[CFG_COOLDOWN], c->switch_cooldown_ms);
    set_int(s_cfg[CFG_MARGIN], c->edge_margin_px);
    swprintf(b, 128, L"0x%02X", (unsigned)c->disp_win_input); set_text(s_cfg[CFG_DISP_WIN], b);
    swprintf(b, 128, L"0x%02X", (unsigned)c->disp_mac_input); set_text(s_cfg[CFG_DISP_MAC], b);
    set_int(s_cfg[CFG_WAKE], c->disp_wake_sec);
    set_int(s_cfg[CFG_CHUNK], c->xfer_chunk_kb);
    utf8_to_wide(c->mac_input_cmd, b, 128); set_text(s_cfg[CFG_MAC_INPUT], b);
    utf8_to_wide(c->mac_sleep_cmd, b, 128); set_text(s_cfg[CFG_MAC_SLEEP], b);
}
static void reset_settings(void) {
    hop_config_defaults(&g_app.cfg);
    snprintf(g_app.cfg.device_name, sizeof(g_app.cfg.device_name), "%s", "Windows");
    populate_settings();
    set_text(s_cfg_note, L"已恢复默认值；点击“保存设置”写入配置文件。");
}
static void save_settings(void) {
    hop_config next = g_app.cfg;
    wchar_t b[256];
    char utf8[256];
    if (!wide_to_utf8(s_cfg[CFG_NAME], utf8, sizeof(utf8)) || !utf8[0] || strlen(utf8) >= sizeof(next.device_name)) {
        MessageBoxW(s_main, L"设备名称不能为空，且长度不能超过 31 个字节。", L"HopDesk 设置", MB_OK | MB_ICONWARNING); return;
    }
    snprintf(next.device_name, sizeof(next.device_name), "%s", utf8);
    if (!wide_to_utf8(s_cfg[CFG_TOKEN], utf8, sizeof(utf8)) || strlen(utf8) >= sizeof(next.token)) {
        MessageBoxW(s_main, L"Token 长度不能超过 128 个字节。", L"HopDesk 设置", MB_OK | MB_ICONWARNING); return;
    }
    snprintf(next.token, sizeof(next.token), "%s", utf8);
    unsigned long port = parse_number(s_cfg[CFG_PORT], 0), data_port = parse_number(s_cfg[CFG_DATA_PORT], 0);
    unsigned long delay = parse_number(s_cfg[CFG_SWITCH_DELAY], 250), cooldown = parse_number(s_cfg[CFG_COOLDOWN], 300);
    unsigned long margin = parse_number(s_cfg[CFG_MARGIN], 40), win_input = parse_number(s_cfg[CFG_DISP_WIN], 0x0F);
    unsigned long mac_input = parse_number(s_cfg[CFG_DISP_MAC], 0x11), wake = parse_number(s_cfg[CFG_WAKE], 60);
    unsigned long chunk = parse_number(s_cfg[CFG_CHUNK], 256);
    if (!port || port > 65535 || !data_port || data_port > 65535 || port == data_port ||
        delay > 5000 || cooldown > 10000 || margin > 1000 ||
        win_input > 65535 || mac_input > 65535 || wake > 3600 || chunk < 64 || chunk > 4096) {
        MessageBoxW(s_main, L"请检查端口、边缘参数、显示器输入源和传输分块范围。", L"HopDesk 设置", MB_OK | MB_ICONWARNING); return;
    }
    next.port=(uint16_t)port; next.data_port=(uint16_t)data_port;
    next.server_edge=(int)SendMessageW(s_cfg[CFG_SERVER_EDGE],CB_GETCURSEL,0,0);
    next.client_edge=(int)SendMessageW(s_cfg[CFG_CLIENT_EDGE],CB_GETCURSEL,0,0);
    next.switch_delay_ms=(int)delay; next.switch_cooldown_ms=(int)cooldown; next.edge_margin_px=(int)margin;
    next.disp_win_input=(uint16_t)win_input; next.disp_mac_input=(uint16_t)mac_input;
    next.disp_wake_sec=(int)wake; next.xfer_chunk_kb=(int)chunk;
    if (!wide_to_utf8(s_cfg[CFG_MAC_INPUT], utf8, sizeof(utf8))) utf8[0]=0;
    snprintf(next.mac_input_cmd,sizeof(next.mac_input_cmd),"%s",utf8);
    if (!wide_to_utf8(s_cfg[CFG_MAC_SLEEP], utf8, sizeof(utf8))) utf8[0]=0;
    snprintf(next.mac_sleep_cmd,sizeof(next.mac_sleep_cmd),"%s",utf8);
    if (!hop_config_save_atomic(&next, g_app.config_path)) {
        MessageBoxW(s_main, L"配置保存失败。原有配置文件保持不变，请检查目录权限和磁盘空间。", L"HopDesk 设置", MB_OK | MB_ICONERROR); return;
    }
    g_app.cfg = next;
    set_text(s_cfg_note, L"设置已保存。端口、认证和连接参数将在重启 HopDesk 后完整生效。");
    hop_log(HOP_LOG_INFO, "ui: configuration saved; restart required for network settings");
    refresh_connection();
}
static void refresh_transfers(void) {
    if (!s_xfer_title) return;
    hop_xfer_status st; memset(&st, 0, sizeof(st)); xfer_get_status(&st);
    wchar_t name[HOP_XFER_STATUS_NAME], detail[HOP_XFER_STATUS_DETAIL], row[1800], path[1024];
    utf8_to_wide(st.name, name, HOP_XFER_STATUS_NAME);
    const wchar_t *phase = L"空闲";
    switch (st.phase) {
        case HOP_XFER_QUEUED: phase=L"排队中"; break;
        case HOP_XFER_SCANNING: phase=L"扫描中"; break;
        case HOP_XFER_SENDING: phase=L"发送中"; break;
        case HOP_XFER_RECEIVING: phase=L"接收中"; break;
        case HOP_XFER_VERIFYING: phase=L"校验中"; break;
        case HOP_XFER_COMPLETED: phase=L"已完成"; break;
        case HOP_XFER_PARTIAL: phase=L"部分失败"; break;
        case HOP_XFER_FAILED: phase=L"失败"; break;
        case HOP_XFER_CANCELLED: phase=L"已取消"; break;
    }
    swprintf(row, 1800, L"%ls", name[0] ? name : L"暂无传输任务"); set_text(s_xfer_title, row);
    utf8_to_wide(st.detail, detail, HOP_XFER_STATUS_DETAIL);
    swprintf(row, 1800, L"%ls  ·  %ls  ·  %ls", st.direction == HOP_XFER_DIR_SEND ? L"发送" :
             st.direction == HOP_XFER_DIR_RECEIVE ? L"接收" : L"待机", phase, detail);
    set_text(s_xfer_detail, row);
    swprintf(row, 1800, L"文件 %u / %u    排队 %u    数据 %llu / %llu MB",
             st.files_done, st.files_total, st.queued_count,
             (unsigned long long)(st.bytes_done / 1048576ULL),
             (unsigned long long)(st.bytes_total / 1048576ULL));
    set_text(s_xfer_counts, row);
    int pct = st.bytes_total ? (int)(100ULL * st.bytes_done / st.bytes_total) : (st.active ? 0 : 100);
    SendMessageW(s_xfer_progress, PBM_SETPOS, pct > 100 ? 100 : pct, 0);

    if (!s_file_snapshot) s_file_snapshot = (hop_xfer_file_status *)calloc(HOP_XFER_FILE_LIMIT, sizeof(*s_file_snapshot));
    int n = s_file_snapshot ? xfer_get_files(st.direction, st.batch, s_file_snapshot, HOP_XFER_FILE_LIMIT) : 0;
    if (s_file_batch != st.batch || s_file_direction != st.direction) {
        memset(s_collapsed, 0, sizeof(s_collapsed)); s_file_batch = st.batch; s_file_direction = st.direction;
    }
    SendMessageW(s_xfer_files, LB_RESETCONTENT, 0, 0);
    s_visible_count = 0;
    for (int i=0; i<n; ++i) {
        int hidden = 0;
        for (int p=0; p<i; ++p) {
            if (!s_collapsed[p] || !s_file_snapshot[p].is_directory) continue;
            size_t len = strlen(s_file_snapshot[p].relative_path);
            if (!strncmp(s_file_snapshot[i].relative_path,s_file_snapshot[p].relative_path,len) &&
                s_file_snapshot[i].relative_path[len]=='/') { hidden=1; break; }
        }
        if (hidden) continue;
        utf8_to_wide(s_file_snapshot[i].relative_path, path, 1024);
        int depth=0; for (const char *p=s_file_snapshot[i].relative_path; *p; ++p) if (*p=='/') ++depth;
        wchar_t indent[64]; int spaces=depth*2; if(spaces>24)spaces=24;
        for(int j=0;j<spaces;++j)indent[j]=L' '; indent[spaces]=0;
        const wchar_t *state = s_file_snapshot[i].phase==HOP_XFER_FILE_COMPLETED?L"完成":
            s_file_snapshot[i].phase==HOP_XFER_FILE_FAILED?L"失败":
            s_file_snapshot[i].phase==HOP_XFER_FILE_SENDING?L"发送中":
            s_file_snapshot[i].phase==HOP_XFER_FILE_RECEIVING?L"接收中":
            s_file_snapshot[i].phase==HOP_XFER_FILE_VERIFYING?L"校验中":L"排队中";
        swprintf(row,1800,L"%ls%ls %ls   %llu / %llu B   %ls",indent,
            s_file_snapshot[i].is_directory?(s_collapsed[i]?L"[+目录]":L"[-目录]"):L"[文件]",
            path,(unsigned long long)s_file_snapshot[i].bytes_done,
            (unsigned long long)s_file_snapshot[i].bytes_total,state);
        LRESULT idx=SendMessageW(s_xfer_files,LB_ADDSTRING,0,(LPARAM)row);
        if(idx!=LB_ERR){SendMessageW(s_xfer_files,LB_SETITEMDATA,(WPARAM)idx,(LPARAM)i);s_visible_indices[s_visible_count++]=i;}
    }
    hop_xfer_history_entry hist[HOP_XFER_HISTORY_LIMIT];
    int hn=xfer_get_history(hist,HOP_XFER_HISTORY_LIMIT);
    SendMessageW(s_xfer_history,LB_RESETCONTENT,0,0);
    for(int i=0;i<hn;++i){
        wchar_t hname[HOP_XFER_STATUS_NAME];
        utf8_to_wide(hist[i].name,hname,HOP_XFER_STATUS_NAME);
        const wchar_t *d=hist[i].direction==HOP_XFER_DIR_SEND?L"发送":L"接收";
        const wchar_t *p=hist[i].phase==HOP_XFER_COMPLETED?L"完成":hist[i].phase==HOP_XFER_PARTIAL?L"部分失败":
            hist[i].phase==HOP_XFER_CANCELLED?L"已取消":L"失败";
        swprintf(row,1800,L"%ls  ·  %ls  ·  %ls  ·  文件 %u/%u  ·  %llu MB",d,p,hname,
            hist[i].files_done,hist[i].files_total,(unsigned long long)(hist[i].bytes_total/1048576ULL));
        SendMessageW(s_xfer_history,LB_ADDSTRING,0,(LPARAM)row);
    }
}
static void create_settings_page(HWND p) {
    group(p,L"连接与身份",20,14,390,275);
    label(p,L"设备名称",38,42,120,20); s_cfg[CFG_NAME]=edit(p,L"",160,38,225,26,0,0);
    label(p,L"服务端地址",38,78,120,20); s_cfg[CFG_HOST]=edit(p,L"",160,74,225,26,0,0);
    label(p,L"控制端口",38,114,120,20); s_cfg[CFG_PORT]=edit(p,L"",160,110,225,26,0,0);
    label(p,L"数据端口",38,150,120,20); s_cfg[CFG_DATA_PORT]=edit(p,L"",160,146,225,26,0,0);
    label(p,L"共享 Token",38,186,120,20); s_cfg[CFG_TOKEN]=edit(p,L"",160,182,225,26,0,1);
    label(p,L"服务端边缘",38,222,120,20);
    s_cfg[CFG_SERVER_EDGE]=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,160,218,225,120,0);
    SendMessageW(s_cfg[CFG_SERVER_EDGE],CB_ADDSTRING,0,(LPARAM)L"左边缘"); SendMessageW(s_cfg[CFG_SERVER_EDGE],CB_ADDSTRING,0,(LPARAM)L"右边缘");
    label(p,L"客户端边缘",38,258,120,20);
    s_cfg[CFG_CLIENT_EDGE]=child(p,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,160,254,225,120,0);
    SendMessageW(s_cfg[CFG_CLIENT_EDGE],CB_ADDSTRING,0,(LPARAM)L"左边缘"); SendMessageW(s_cfg[CFG_CLIENT_EDGE],CB_ADDSTRING,0,(LPARAM)L"右边缘");

    group(p,L"交互与显示器",430,14,390,455);
    label(p,L"边缘触发延迟（ms）",448,42,170,20); s_cfg[CFG_SWITCH_DELAY]=edit(p,L"",635,38,165,26,0,0);
    label(p,L"切换冷却（ms）",448,78,170,20); s_cfg[CFG_COOLDOWN]=edit(p,L"",635,74,165,26,0,0);
    label(p,L"边缘内缩（px）",448,114,170,20); s_cfg[CFG_MARGIN]=edit(p,L"",635,110,165,26,0,0);
    label(p,L"Windows 输入源",448,150,170,20); s_cfg[CFG_DISP_WIN]=edit(p,L"",635,146,165,26,0,0);
    label(p,L"Mac 输入源",448,186,170,20); s_cfg[CFG_DISP_MAC]=edit(p,L"",635,182,165,26,0,0);
    label(p,L"唤醒时长（秒）",448,222,170,20); s_cfg[CFG_WAKE]=edit(p,L"",635,218,165,26,0,0);
    label(p,L"传输分块（KB）",448,258,170,20); s_cfg[CFG_CHUNK]=edit(p,L"",635,254,165,26,0,0);
    label(p,L"Mac 切源命令",448,294,170,20); s_cfg[CFG_MAC_INPUT]=edit(p,L"",448,318,352,26,0,0);
    label(p,L"Mac 睡眠命令",448,354,170,20); s_cfg[CFG_MAC_SLEEP]=edit(p,L"",448,378,352,26,0,0);
    label(p,L"输入源支持十六进制，例如 0x0F。Mac 命令仅由 Mac 客户端执行。",448,416,350,34);
    label(p,L"Windows 为服务端，此处的服务端地址由 Mac 客户端填写。",38,288,370,28);
    EnableWindow(s_cfg[CFG_HOST],FALSE);
    button(p,L"保存设置",448,500,130,34,UI_ID_SAVE);
    button(p,L"恢复默认",590,500,130,34,UI_ID_RESET);
    s_cfg_note=label(p,L"修改后点击保存；网络参数将在重启后生效。",38,548,760,28);
}
static void create_pages(HWND h) {
    for(int i=0;i<UI_PAGE_COUNT;++i) s_pages[i]=page_create(h,i);

    HWND p=s_pages[0];
    group(p,L"设备状态",20,18,800,145);
    s_overview_status=label(p,L"等待 Mac 连接",42,52,740,34); set_font(s_overview_status,s_font_title);
    s_overview_peer=label(p,L"Windows 服务端",42,94,740,24);
    s_overview_channels=label(p,L"控制通道：等待连接    数据通道：等待连接",42,124,740,24);
    group(p,L"常用操作",20,185,800,145);
    s_overview_sharing=label(p,L"键鼠共享：等待连接",42,218,740,24);
    button(p,L"切换键鼠",42,260,120,34,UI_ID_TOGGLE);
    button(p,L"切换到 Mac 显示",180,260,150,34,UI_ID_SWITCH_MAC);
    button(p,L"切回 Windows 显示",348,260,160,34,UI_ID_SWITCH_WIN);
    button(p,L"唤醒显示",526,260,120,34,UI_ID_WAKE);
    button(p,L"显示睡眠",664,260,120,34,UI_ID_SLEEP);
    group(p,L"快速入口",20,352,800,125);
    button(p,L"打开传输管理",42,398,150,36,UI_ID_OPEN_TRANSFERS);
    label(p,L"连接、设备设置、传输记录、运行日志和软件更新都集中在左侧导航中。",220,404,560,28);

    p=s_pages[1];
    group(p,L"连接状态",20,18,800,235);
    s_connection_role=label(p,L"本机角色：Windows 服务端",42,58,720,24);
    s_connection_status=label(p,L"服务状态：等待连接",42,98,720,24);
    s_connection_peer=label(p,L"Mac 屏幕：尚未取得设备信息",42,138,720,24);
    s_connection_ports=label(p,L"监听端口：24817（控制） / 24818（数据）",42,178,720,24);
    button(p,L"重新连接",42,210,130,34,UI_ID_RECONNECT);
    label(p,L"Windows 端持续监听；Mac 端自动重连。点击重新连接会断开当前会话并等待对端重新握手。",42,280,760,40);

    p=s_pages[2]; create_settings_page(p);

    p=s_pages[3];
    group(p,L"当前任务",20,14,800,145);
    s_xfer_title=label(p,L"暂无传输任务",40,42,740,24); set_font(s_xfer_title,s_font_title);
    s_xfer_detail=label(p,L"等待文件传输",40,72,740,22);
    s_xfer_counts=label(p,L"文件 0 / 0    排队 0    数据 0 / 0 MB",40,100,740,22);
    s_xfer_progress=child(p,PROGRESS_CLASSW,L"",PBS_SMOOTH,40,128,740,14,0);
    group(p,L"文件明细",20,174,800,195);
    s_xfer_files=child(p,L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS|WS_TABSTOP,40,198,760,155,UI_ID_FILES);
    group(p,L"传输历史",20,386,800,175);
    s_xfer_history=child(p,L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS,40,410,760,135,0);
    button(p,L"重试选中文件",500,572,140,34,UI_ID_XFER_RETRY);
    button(p,L"取消传输",660,572,140,34,UI_ID_XFER_CANCEL);

    p=s_pages[4];
    group(p,L"运行日志",20,14,800,590);
    s_log_edit=child(p,L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL,40,40,760,500,0);
    SendMessageW(s_log_edit,EM_SETLIMITTEXT,512*1024,0);
    button(p,L"刷新日志",560,552,110,34,UI_ID_LOG_REFRESH);
    button(p,L"复制日志",690,552,110,34,UI_ID_LOG_COPY);

    p=s_pages[5];
    group(p,L"软件更新",20,18,800,260);
    s_update_version=label(p,L"当前版本",42,58,740,28); set_font(s_update_version,s_font_title);
    s_update_source=label(p,L"更新源：GitHub Releases",42,104,740,24);
    label(p,L"检查更新会读取公开更新清单。发现新版本后，可下载、校验并自动安装。",42,152,740,44);
    button(p,L"检查更新",42,220,140,36,UI_ID_UPDATE);
    label(p,L"如果更新源尚未发布 latest.txt，检查会明确提示读取失败，不会将其误报为“已是最新版本”。",42,300,740,48);

    for(int i=0;i<UI_PAGE_COUNT;++i) ShowWindow(s_pages[i],i==0?SW_SHOW:SW_HIDE);
    populate_settings();
}
static void layout_main(HWND h) {
    RECT r; GetClientRect(h,&r);
    int w=r.right-r.left, height=r.bottom-r.top;
    /* Sidebar and page host remain aligned at every DPI scale. */
    for(int i=0;i<UI_PAGE_COUNT;++i)
        if(s_pages[i]) MoveWindow(s_pages[i],228,88,w-248,height-118,TRUE);
    if(s_footer) MoveWindow(s_footer,228,height-26,w-250,22,TRUE);
}
static void create_main_controls(HWND h) {
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    s_font=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    s_font_title=CreateFontW(-23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    s_font_small=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    HWND brand=label(h,L"HopDesk",24,22,170,36); set_font(brand,s_font_title);
    label(h,L"跨设备协作",26,58,160,20);
    for(int i=0;i<UI_PAGE_COUNT;++i){
        s_nav[i]=button(h,kPageNames[i],20,112+i*48,180,38,UI_NAV_BASE+i);
        set_font(s_nav[i],s_font);
    }
    s_title=label(h,L"概览",250,22,400,40); set_font(s_title,s_font_title);
    s_header_status=label(h,L"等待 Mac 连接",790,30,300,26);
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
        case WM_GETMINMAXINFO:{
            MINMAXINFO *m=(MINMAXINFO *)lp;
            m->ptMinTrackSize.x=1040; m->ptMinTrackSize.y=700;
            return 0;
        }
        case WM_TIMER:
            if(wp==UI_TIMER){refresh_connection(); if(s_page==3)refresh_transfers(); return 0;}
            break;
        case WM_COMMAND:{
            int id=LOWORD(wp);
            if(id>=UI_NAV_BASE && id<UI_NAV_BASE+UI_PAGE_COUNT){show_page(id-UI_NAV_BASE);return 0;}
            switch(id){
                case UI_ID_SAVE: save_settings(); return 0;
                case UI_ID_RESET: reset_settings(); return 0;
                case UI_ID_RECONNECT: if(g_app.net)net_server_drop(g_app.net); return 0;
                case UI_ID_XFER_CANCEL: xfer_cancel(); return 0;
                case UI_ID_XFER_RETRY:{
                    int selected=(int)SendMessageW(s_xfer_files,LB_GETCURSEL,0,0);
                    if(selected!=LB_ERR){
                        int index=(int)SendMessageW(s_xfer_files,LB_GETITEMDATA,(WPARAM)selected,0);
                        if(s_file_direction==HOP_XFER_DIR_SEND && index>=0)
                            (void)xfer_retry_file(s_file_batch,index);
                    } return 0;
                }
                case UI_ID_FILES:
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
                case UI_ID_LOG_COPY:
                    if(s_log_edit){SendMessageW(s_log_edit,EM_SETSEL,0,-1);SendMessageW(s_log_edit,WM_COPY,0,0);}
                    return 0;
                case UI_ID_UPDATE: update_check_async(s_main); return 0;
                case UI_ID_OPEN_TRANSFERS: show_page(3); return 0;
                case UI_ID_TOGGLE: SendMessageW(g_app.hwnd,WM_COMMAND,1001,0); return 0;
                case UI_ID_SWITCH_MAC: SendMessageW(g_app.hwnd,WM_COMMAND,1003,0); return 0;
                case UI_ID_SWITCH_WIN: SendMessageW(g_app.hwnd,WM_COMMAND,1004,0); return 0;
                case UI_ID_WAKE: SendMessageW(g_app.hwnd,WM_COMMAND,1003,0); return 0;
                case UI_ID_SLEEP: SendMessageW(g_app.hwnd,WM_COMMAND,1005,0); return 0;
            }
            break;
        }
        case WM_CLOSE:
            ShowWindow(h,SW_HIDE);
            KillTimer(h,UI_TIMER);
            return 0;
        case WM_DESTROY:
            KillTimer(h,UI_TIMER);
            if(s_file_snapshot){free(s_file_snapshot);s_file_snapshot=NULL;}
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
        WNDCLASSW wc; memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=main_wnd_proc; wc.hInstance=g_app.inst;
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName=L"HopDeskMainWindow";
        RegisterClassW(&wc);
        WNDCLASSW pc; memset(&pc,0,sizeof(pc));
        pc.lpfnWndProc=page_proc; pc.hInstance=g_app.inst;
        pc.hCursor=LoadCursorW(NULL,IDC_ARROW); pc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        pc.lpszClassName=L"HopDeskMainPage";
        RegisterClassW(&pc);
        s_main=CreateWindowExW(WS_EX_APPWINDOW,L"HopDeskMainWindow",L"HopDesk",
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1140,800,
            NULL,NULL,g_app.inst,NULL);
    }
    if(!s_main)return;
    show_page(page);
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
