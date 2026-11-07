/* =====================================================================
 * HopDesk - macOS native main window
 * Cocoa implementation of the shared six-page application UI.
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ApplicationServices/ApplicationServices.h>
#import <dispatch/dispatch.h>
#include "mac_app.h"
#include "xfer.h"
#include "../core/xfer_status.h"
#include "../core/hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CFG_NAME, CFG_HOST, CFG_PORT, CFG_DATA_PORT, CFG_TOKEN,
    CFG_SERVER_EDGE, CFG_CLIENT_EDGE, CFG_SWITCH_DELAY, CFG_COOLDOWN,
    CFG_MARGIN, CFG_DISP_WIN, CFG_DISP_MAC, CFG_WAKE, CFG_CHUNK,
    CFG_MAC_INPUT, CFG_MAC_SLEEP, CFG_ROLE, CFG_COUNT
};

static NSWindow *s_mainWindow;
static NSView *s_pages[6];
static NSScrollView *s_hosts[6];static int s_hostCount;
static NSPopUpButton *s_rolePopup;
static hop_config s_draft;
static NSString *s_addresses;
static BOOL s_saveBusy,s_logBusy;
static NSButton *s_nav[6];
static NSTextField *s_pageTitle, *s_headerStatus, *s_footer;
static NSTextField *s_overviewStatus, *s_overviewPeer, *s_overviewChannels, *s_overviewSharing;
static NSTextField *s_connectionRole, *s_connectionStatus, *s_connectionPeer, *s_connectionPorts;
static NSTextField *s_cfg[CFG_COUNT], *s_cfgNote;
static NSPopUpButton *s_edgePopups[2];
static NSTextField *s_xferTitle, *s_xferDetail, *s_xferCounts;
static NSTextField *s_xferBudget, *s_xferMemory, *s_xferBudgetStats, *s_xferBudgetLifecycle;
static NSProgressIndicator *s_xferProgress;
static NSTextView *s_xferFiles, *s_xferHistory, *s_logView, *s_taskHistory;
static NSPopUpButton *s_historyDirection, *s_historyState, *s_historyOrder;
static NSButton *s_retryButton;
static NSTableView *s_taskTables[2];
static NSArray<NSDictionary *> *s_taskRows[2];
static hop_xfer_task_snapshot *s_tasksSnapshot;
#define UI_TASK_CAP HOP_TASK_MANAGER_MAX_LIVE
static NSTextField *s_updateVersion, *s_updateSource;
static NSTimer *s_mainTimer;
static NSObject *s_uiController;
static int s_page;
static int s_connected, s_sharing;
static hop_xfer_file_status *s_fileSnapshot;
static int s_collapsedFolders[HOP_XFER_FILE_LIMIT];
static int s_visibleFileIndices[HOP_XFER_FILE_LIMIT];
static int s_visibleFileCount;
static uint32_t s_fileBatch;
static int s_fileDirection;
static BOOL s_accessibilityPermissionRequestStarted;

/* CoreGraphics event posting can fail silently without Post Event permission.
 * Request it once, then take the user directly to the Accessibility privacy pane.
 * Cocoa and System Settings navigation are always performed on the main queue. */
void mac_request_accessibility_permission(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (CGPreflightPostEventAccess()) {
            hop_log(HOP_LOG_INFO, "permission: macOS event-posting access is already granted");
            return;
        }

        if (s_accessibilityPermissionRequestStarted) return;
        s_accessibilityPermissionRequestStarted = YES;

        hop_log(HOP_LOG_WARN, "permission: requesting macOS event-posting access");
        if (CGRequestPostEventAccess() || CGPreflightPostEventAccess()) {
            hop_log(HOP_LOG_INFO, "permission: macOS event-posting access granted");
            return;
        }

        NSURL *settingsURL = nil;
        if (@available(macOS 13.0, *)) {
            settingsURL = [NSURL URLWithString:
                @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility"];
        } else {
            settingsURL = [NSURL URLWithString:
                @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
        }

        BOOL opened = settingsURL && [NSWorkspace.sharedWorkspace openURL:settingsURL];
        if (!opened) {
            NSURL *legacyURL = [NSURL URLWithString:
                @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
            opened = legacyURL && [NSWorkspace.sharedWorkspace openURL:legacyURL];
        }

        if (opened) {
            hop_log(HOP_LOG_WARN,
                    "permission: opened System Settings > Privacy & Security > Accessibility; enable HopDesk, then relaunch it");
        } else {
            hop_log(HOP_LOG_ERROR,
                    "permission: unable to open the Accessibility settings pane; open System Settings > Privacy & Security > Accessibility manually");
        }
    });
}

static NSString *const kPageNames[6] = {@"概览",@"连接",@"设置",@"传输",@"日志",@"更新"};

@interface HopDeskMainController : NSObject <NSWindowDelegate, NSTableViewDataSource>
- (void)selectPage:(id)sender;
- (void)saveSettings:(id)sender;
- (void)roleChanged:(id)sender;
- (void)resetSettings:(id)sender;
- (void)reconnect:(id)sender;
- (void)switchMac:(id)sender;
- (void)switchWin:(id)sender;
- (void)wakeDisplay:(id)sender;
- (void)sleepDisplay:(id)sender;
- (void)cancelTask:(id)sender;
- (void)retryFile:(id)sender;
- (void)toggleFolder:(id)sender;
- (void)refreshLogs:(id)sender;
- (void)copyLogs:(id)sender;
- (void)checkUpdates:(id)sender;
- (BOOL)windowShouldClose:(NSWindow *)sender;
@end

static NSTextField *ui_label(NSView *parent, NSString *value, NSRect frame, CGFloat size, BOOL bold) {
    NSTextField *f=[NSTextField labelWithString:value ?: @""];
    f.frame=frame;
    f.font=bold?[NSFont systemFontOfSize:size weight:NSFontWeightSemibold]:[NSFont systemFontOfSize:size];
    f.lineBreakMode=NSLineBreakByTruncatingTail;
    [parent addSubview:f];
    return f;
}
static NSTextField *ui_field(NSView *parent, NSString *value, NSRect frame, BOOL secure) {
    NSTextField *f=secure?[[NSSecureTextField alloc] initWithFrame:frame]:[[NSTextField alloc] initWithFrame:frame];
    f.stringValue=value ?: @"";
    f.font=[NSFont systemFontOfSize:13];
    [parent addSubview:f];
    return f;
}
static NSButton *ui_button(NSView *parent, NSString *title, NSRect frame, id target, SEL action) {
    NSButton *b=[NSButton buttonWithTitle:title target:target action:action];
    b.frame=frame;
    b.bezelStyle=NSBezelStyleRounded;
    [parent addSubview:b];
    return b;
}
static NSView *ui_card(NSView *parent, NSRect frame) {
    NSView *v=[[NSView alloc] initWithFrame:frame];
    v.wantsLayer=YES;
    v.layer.backgroundColor=[NSColor controlBackgroundColor].CGColor;
    v.layer.cornerRadius=12.0;
    v.layer.borderWidth=1.0;
    v.layer.borderColor=[NSColor separatorColor].CGColor;
    [parent addSubview:v];
    return v;
}
static NSView *ui_page(NSView *root) {
    NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(230,28,880,680)];
    scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.drawsBackground=NO;
    NSView *document=[[NSView alloc] initWithFrame:NSMakeRect(0,0,880,680)];
    document.autoresizingMask=NSViewWidthSizable;scroll.documentView=document;
    scroll.hidden=YES;s_hosts[s_hostCount++]=scroll;[root addSubview:scroll];return document;
}
static NSString *utf8(const char *s) {
    if (!s || !s[0]) return @"";
    return [NSString stringWithUTF8String:s] ?: @"";
}
static NSString *byte_count(uint64_t n) {
    return [NSByteCountFormatter stringFromByteCount:(long long)n countStyle:NSByteCountFormatterCountStyleFile];
}
static void set_cfg_text(int idx, NSString *value) {
    if (s_cfg[idx]) s_cfg[idx].stringValue=value ?: @"";
}
static unsigned long parse_cfg_number(NSString *value, unsigned long fallback) {
    const char *s=value.UTF8String;
    (void)fallback;
    if (!s || !s[0]) return 0xFFFFFFFFUL;
    char *end=NULL;
    unsigned long n=strtoul(s,&end,(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))?16:10);
    return end==s || *end || s[0]=='-' ? 0xFFFFFFFFUL : n;
}
static void populate_settings(void) {
    hop_config *c=&s_draft;
    [s_rolePopup selectItemAtIndex:c->role==HOP_ROLE_CLIENT?1:0];
    s_cfg[CFG_HOST].enabled=c->role==HOP_ROLE_CLIENT;
    set_cfg_text(CFG_NAME,utf8(c->device_name));
    set_cfg_text(CFG_HOST,utf8(c->peer_host));
    set_cfg_text(CFG_PORT,[NSString stringWithFormat:@"%u",(unsigned)c->port]);
    set_cfg_text(CFG_DATA_PORT,[NSString stringWithFormat:@"%u",(unsigned)c->data_port]);
    set_cfg_text(CFG_TOKEN,utf8(c->token));
    [s_edgePopups[0] selectItemAtIndex:(c->server_edge>=HOP_EDGE_LEFT&&c->server_edge<=HOP_EDGE_BOTTOM)?c->server_edge:HOP_EDGE_LEFT];
    [s_edgePopups[1] selectItemAtIndex:(c->client_edge>=HOP_EDGE_LEFT&&c->client_edge<=HOP_EDGE_BOTTOM)?c->client_edge:HOP_EDGE_RIGHT];
    set_cfg_text(CFG_SWITCH_DELAY,[NSString stringWithFormat:@"%d",c->switch_delay_ms]);
    set_cfg_text(CFG_COOLDOWN,[NSString stringWithFormat:@"%d",c->switch_cooldown_ms]);
    set_cfg_text(CFG_MARGIN,[NSString stringWithFormat:@"%d",c->edge_margin_px]);
    set_cfg_text(CFG_DISP_WIN,[NSString stringWithFormat:@"0x%02X",(unsigned)c->disp_win_input]);
    set_cfg_text(CFG_DISP_MAC,[NSString stringWithFormat:@"0x%02X",(unsigned)c->disp_mac_input]);
    set_cfg_text(CFG_WAKE,[NSString stringWithFormat:@"%d",c->disp_wake_sec]);
    set_cfg_text(CFG_CHUNK,[NSString stringWithFormat:@"%d",c->xfer_chunk_kb]);
    set_cfg_text(CFG_MAC_INPUT,utf8(c->mac_input_cmd));
    set_cfg_text(CFG_MAC_SLEEP,utf8(c->mac_sleep_cmd));
}
static void save_settings(void) {
    hop_config next=s_draft;
    if(s_saveBusy)return;
    next.role=s_rolePopup.indexOfSelectedItem==1?HOP_ROLE_CLIENT:HOP_ROLE_SERVER;
    const char *name=s_cfg[CFG_NAME].stringValue.UTF8String ?: "";
    const char *host=s_cfg[CFG_HOST].stringValue.UTF8String ?: "";
    const char *token=s_cfg[CFG_TOKEN].stringValue.UTF8String ?: "";
    const char *input=s_cfg[CFG_MAC_INPUT].stringValue.UTF8String ?: "";
    const char *sleep=s_cfg[CFG_MAC_SLEEP].stringValue.UTF8String ?: "";
    if (!name[0] || !token[0] || (next.role==HOP_ROLE_CLIENT&&!host[0]) || strlen(name)>=sizeof(next.device_name) || strlen(host)>=sizeof(next.peer_host) ||
        strlen(token)>=sizeof(next.token) || strlen(input)>=sizeof(next.mac_input_cmd) ||
        strlen(sleep)>=sizeof(next.mac_sleep_cmd)) {
        NSAlert *a=[[NSAlert alloc] init]; a.messageText=@"设置无效";
        a.informativeText=@"设备名称和共享 Token 不能为空；客户端地址、Token 和命令长度也必须在允许范围内。两端必须设置相同 Token。";
        [a runModal]; return;
    }
    snprintf(next.device_name,sizeof(next.device_name),"%s",name);
    snprintf(next.peer_host,sizeof(next.peer_host),"%s",host);
    snprintf(next.token,sizeof(next.token),"%s",token);
    unsigned long port=parse_cfg_number(s_cfg[CFG_PORT].stringValue,0);
    unsigned long dataPort=parse_cfg_number(s_cfg[CFG_DATA_PORT].stringValue,0);
    unsigned long delay=parse_cfg_number(s_cfg[CFG_SWITCH_DELAY].stringValue,250);
    unsigned long cooldown=parse_cfg_number(s_cfg[CFG_COOLDOWN].stringValue,300);
    unsigned long margin=parse_cfg_number(s_cfg[CFG_MARGIN].stringValue,40);
    unsigned long winInput=parse_cfg_number(s_cfg[CFG_DISP_WIN].stringValue,0x0F);
    unsigned long macInput=parse_cfg_number(s_cfg[CFG_DISP_MAC].stringValue,0x11);
    unsigned long wake=parse_cfg_number(s_cfg[CFG_WAKE].stringValue,60);
    unsigned long chunk=parse_cfg_number(s_cfg[CFG_CHUNK].stringValue,256);
    if (!port || port>65535 || !dataPort || dataPort>65535 || port==dataPort ||
        delay>5000 || cooldown>10000 || margin>1000 || winInput>65535 ||
        macInput>65535 || wake>3600 || chunk<64 || chunk>4096) {
        NSAlert *a=[[NSAlert alloc] init]; a.messageText=@"设置无效";
        a.informativeText=@"请检查端口、边缘参数、显示器输入源和传输分块范围。";
        [a runModal]; return;
    }
    next.port=(uint16_t)port; next.data_port=(uint16_t)dataPort;
    next.server_edge=(int)s_edgePopups[0].indexOfSelectedItem;
    next.client_edge=(int)s_edgePopups[1].indexOfSelectedItem;
    next.switch_delay_ms=(int)delay; next.switch_cooldown_ms=(int)cooldown;
    next.edge_margin_px=(int)margin; next.disp_win_input=(uint16_t)winInput;
    next.disp_mac_input=(uint16_t)macInput; next.disp_wake_sec=(int)wake;
    next.xfer_chunk_kb=(int)chunk;
    snprintf(next.mac_input_cmd,sizeof(next.mac_input_cmd),"%s",input);
    snprintf(next.mac_sleep_cmd,sizeof(next.mac_sleep_cmd),"%s",sleep);
    s_saveBusy=YES;
    s_cfgNote.stringValue=@"正在保存…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        int ok=hop_config_save_atomic(&next,g_mac.config_path);
        dispatch_async(dispatch_get_main_queue(), ^{
            s_saveBusy=NO;
            if(ok){s_draft=next;s_cfgNote.stringValue=@"设置已保存；重启 HopDesk 后生效。";}
            else s_cfgNote.stringValue=@"保存失败；原配置未改变，请检查权限和磁盘空间。";
        });
    });
}
static void reset_settings(void) {
    if(s_saveBusy)return;
    hop_config_defaults(&s_draft);
    snprintf(s_draft.device_name,sizeof(s_draft.device_name),"%s","Mac");
    populate_settings();
    s_cfgNote.stringValue=@"已恢复默认值；点击“保存设置”写入配置文件。";
}
static NSString *task_state_name(const hop_xfer_task_snapshot *t) {
    if(t->state<=HOP_XFER_TASK_ACTIVE && t->cancel_requested)return @"取消中";
    if(t->state<=HOP_XFER_TASK_ACTIVE && t->completing)return @"完成处理中";
    switch(t->state){
        case HOP_XFER_TASK_CREATED:return @"准备中";
        case HOP_XFER_TASK_SCANNING:return @"扫描中";
        case HOP_XFER_TASK_QUEUED:return @"排队中";
        case HOP_XFER_TASK_ACTIVE:return @"进行中";
        case HOP_XFER_TASK_COMPLETED:return @"已完成";
        case HOP_XFER_TASK_PARTIAL:return @"部分完成";
        case HOP_XFER_TASK_FAILED:return @"失败";
        case HOP_XFER_TASK_CANCELLED:return @"已取消";
        case HOP_XFER_TASK_INTERRUPTED:return @"已中断";
        default:return @"未知";
    }
}
static hop_xfer_task_id selected_task(int d) {
    NSInteger row=s_taskTables[d].selectedRow;
    return row>=0 && (NSUInteger)row<s_taskRows[d].count ? [s_taskRows[d][(NSUInteger)row][@"id"] unsignedLongLongValue] : 0;
}
static void refresh_task_lists(void) {
    if(!s_taskTables[0]||!s_taskTables[1])return;
    if(!s_tasksSnapshot)s_tasksSnapshot=calloc(UI_TASK_CAP,sizeof(*s_tasksSnapshot));
    if(!s_tasksSnapshot)return;
    hop_xfer_task_id selected[2]={selected_task(0),selected_task(1)};
    int total=0,n=hop_xfer_task_list(s_tasksSnapshot,UI_TASK_CAP,&total);
    if(n<0)return;
    NSMutableArray<NSDictionary *> *rows[2]={[NSMutableArray array],[NSMutableArray array]};
    for(int i=0;i<n;++i){
        hop_xfer_task_snapshot *t=&s_tasksSnapshot[i];
        int d=t->direction==HOP_XFER_DIR_SEND?0:t->direction==HOP_XFER_DIR_RECEIVE?1:-1;
        if(d<0)continue;
        NSString *text=[NSString stringWithFormat:@"#%llu  ·  批次 %u  ·  %@",(unsigned long long)t->id,t->transfer_id,task_state_name(t)];
        [rows[d] addObject:@{@"id":@(t->id),@"text":text}];
    }
    for(int d=0;d<2;++d){
        s_taskRows[d]=[rows[d] copy];[s_taskTables[d] reloadData];[s_taskTables[d] deselectAll:nil];
        if(selected[d])for(NSUInteger i=0;i<s_taskRows[d].count;++i)
            if([s_taskRows[d][i][@"id"] unsignedLongLongValue]==selected[d]){
                [s_taskTables[d] selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];break;
            }
    }
    (void)total;
}
static void refresh_task_history(void) {
    if(!s_taskHistory || !s_historyDirection || !s_historyState || !s_historyOrder)return;
    static const int directions[]={HOP_XFER_DIR_NONE,HOP_XFER_DIR_SEND,HOP_XFER_DIR_RECEIVE};
    static const hop_xfer_task_state states[]={HOP_XFER_TASK_EMPTY,HOP_XFER_TASK_COMPLETED,
        HOP_XFER_TASK_PARTIAL,HOP_XFER_TASK_FAILED,HOP_XFER_TASK_CANCELLED,HOP_XFER_TASK_INTERRUPTED};
    NSInteger d=s_historyDirection.indexOfSelectedItem,st=s_historyState.indexOfSelectedItem,order=s_historyOrder.indexOfSelectedItem;
    if(d<0 || d>2 || st<0 || st>5 || order<0 || order>1)return;
    hop_xfer_task_snapshot history[HOP_TASK_HISTORY_CAPACITY];
    int total=0,n=hop_xfer_task_history_list(history,HOP_TASK_HISTORY_CAPACITY,directions[d],states[st],order==0,&total);
    if(n<0)return;
    NSMutableString *rows=[NSMutableString string];
    for(int i=0;i<n;++i){const hop_xfer_task_snapshot *t=&history[i];
        [rows appendFormat:@"#%llu  ·  %@  ·  批次 %u  ·  %@\n",(unsigned long long)t->id,
         t->direction==HOP_XFER_DIR_SEND?@"发送":@"接收",t->transfer_id,task_state_name(t)];
    }
    NSString *next=rows.length?rows:@"暂无符合条件的任务";
    if(![s_taskHistory.string isEqualToString:next])s_taskHistory.string=next;
    (void)total;
}
static void refresh_transfer_page(void) {
    if (!s_xferTitle) return;
    refresh_task_lists();
    refresh_task_history();
    hop_xfer_status_pair pair; memset(&pair,0,sizeof(pair)); xfer_get_status_pair(&pair);
    hop_xfer_status st=pair.primary_direction==HOP_XFER_DIR_SEND?pair.send:pair.receive;
    NSString *phase=@"空闲";
    switch(st.phase){
        case HOP_XFER_QUEUED:phase=@"排队中";break; case HOP_XFER_SCANNING:phase=@"扫描中";break;
        case HOP_XFER_SENDING:phase=@"发送中";break; case HOP_XFER_RECEIVING:phase=@"接收中";break;
        case HOP_XFER_VERIFYING:phase=@"校验中";break; case HOP_XFER_COMPLETED:phase=@"已完成";break;
        case HOP_XFER_PARTIAL:phase=@"部分失败";break; case HOP_XFER_FAILED:phase=@"失败";break;
        case HOP_XFER_CANCELLED:phase=@"已取消";break;
    }
    s_xferTitle.stringValue=st.name[0]?utf8(st.name):@"暂无传输任务";
    NSString *direction=st.direction==HOP_XFER_DIR_SEND?@"发送":st.direction==HOP_XFER_DIR_RECEIVE?@"接收":@"待机";
    uint64_t now_ms=hop_monotonic_ms();
    uint64_t speed=st.speed_bytes_per_sec;
    if(!st.last_progress_ms || now_ms<st.last_progress_ms || now_ms-st.last_progress_ms>2000) speed=0;
    NSString *rate=speed?[NSString stringWithFormat:@"%@/s",byte_count(speed)]:@"速率 --";
    NSString *eta=speed?[NSString stringWithFormat:@"ETA %llus",(unsigned long long)st.eta_seconds]:@"ETA --";
    s_xferDetail.stringValue=[NSString stringWithFormat:@"%@  ·  %@  ·  %@  ·  %@  ·  %@",
        direction,phase,utf8(st.detail),rate,eta];
    s_xferCounts.stringValue=[NSString stringWithFormat:@"文件 %u / %u    排队 %u    数据 %@ / %@",
        st.files_done,st.files_total,st.queued_count,byte_count(st.bytes_done),byte_count(st.bytes_total)];
    if(pair.send.active && pair.receive.active)
        s_xferCounts.stringValue=[NSString stringWithFormat:@"同时进行：发送 %@/%@ · 接收 %@/%@ · 排队 %u",
            byte_count(pair.send.bytes_done),byte_count(pair.send.bytes_total),
            byte_count(pair.receive.bytes_done),byte_count(pair.receive.bytes_total),pair.send.queued_count];
    hop_xfer_task_audit audit; memset(&audit,0,sizeof(audit)); hop_xfer_task_reconcile(&audit);
    hop_xfer_budget_snapshot budget=audit.ledger;
    s_xferBudget.stringValue=[NSString stringWithFormat:@"预算  jobs %u/%u  ·  entries %u/%u  ·  待传输 %@/%@",
        budget.jobs,budget.limits.max_jobs,budget.entries,budget.limits.max_entries,
        byte_count(budget.pending_bytes),byte_count(budget.limits.max_pending_bytes)];
    s_xferMemory.stringValue=[NSString stringWithFormat:@"内存  image %@/%@ (temp %@/ %@)  ·  RX %u/%u slots  ·  %@/%@",
        byte_count(budget.image_bytes),byte_count(budget.limits.max_image_bytes),
        byte_count(budget.image_temp_bytes),byte_count(budget.limits.max_image_temp_bytes),
        budget.rx_queue_slots,budget.limits.max_rx_queue_slots,
        byte_count(budget.rx_queue_bytes),byte_count(budget.limits.max_rx_queue_bytes)];
    uint64_t rejected=budget.rejected_jobs+budget.rejected_entries+budget.rejected_bytes+
        budget.rejected_images+budget.rejected_rx_buffers+budget.rejected_image_temp_bytes+
        budget.rejected_image_transfers;
    s_xferBudgetStats.stringValue=[NSString stringWithFormat:@"峰值 jobs %u · entries %u · 待传输 %@ · image %@ · RX %u slots/%@ · 拒绝 %llu",
        budget.jobs_high_water,budget.entries_high_water,
        byte_count(budget.pending_bytes_high_water),byte_count(budget.image_bytes_high_water),
        budget.rx_queue_slots_high_water,byte_count(budget.rx_queue_bytes_high_water),
        (unsigned long long)rejected];
    s_xferBudgetLifecycle.stringValue=[NSString stringWithFormat:@"释放 tasks %llu · RX buffers %llu · image temp %@ · image handoff %@ · 账本异常 %llu · 对账 %@ (0x%X)",
        (unsigned long long)budget.released_reservations,
        (unsigned long long)budget.released_rx_buffers,
        byte_count(budget.released_image_temp_bytes),
        byte_count(budget.transferred_image_temp_bytes),
        (unsigned long long)budget.accounting_errors,
        audit.mismatches?@"异常":@"正常",(unsigned)audit.mismatches];
    s_xferProgress.doubleValue=st.bytes_total?MIN(100.0,100.0*(double)st.bytes_done/(double)st.bytes_total):(st.phase==HOP_XFER_COMPLETED?100:0);
    s_retryButton.enabled=st.direction==HOP_XFER_DIR_SEND;
    if (!s_fileSnapshot) s_fileSnapshot=calloc(HOP_XFER_FILE_LIMIT,sizeof(*s_fileSnapshot));
    int count=s_fileSnapshot?xfer_get_files(st.direction,st.batch,s_fileSnapshot,HOP_XFER_FILE_LIMIT):0;
    if (s_fileBatch!=st.batch || s_fileDirection!=st.direction) {
        memset(s_collapsedFolders,0,sizeof(s_collapsedFolders));
        s_fileBatch=st.batch; s_fileDirection=st.direction;
    }
    NSMutableString *files=[NSMutableString string]; s_visibleFileCount=0;
    for(int i=0;i<count;++i){
        NSString *path=utf8(s_fileSnapshot[i].relative_path);
        BOOL hidden=NO;
        for(int p=0;p<i;++p){
            if(!s_collapsedFolders[p]||!s_fileSnapshot[p].is_directory)continue;
            NSString *parent=utf8(s_fileSnapshot[p].relative_path);
            if([path hasPrefix:[parent stringByAppendingString:@"/"]]){hidden=YES;break;}
        }
        if(hidden)continue;
        s_visibleFileIndices[s_visibleFileCount++]=i;
        NSUInteger depth=[[path componentsSeparatedByString:@"/"] count]-1;
        NSString *indent=[@"" stringByPaddingToLength:MIN(depth*2,24) withString:@" " startingAtIndex:0];
        NSString *state=s_fileSnapshot[i].phase==HOP_XFER_FILE_COMPLETED?@"完成":
            s_fileSnapshot[i].phase==HOP_XFER_FILE_FAILED?@"失败":
            s_fileSnapshot[i].phase==HOP_XFER_FILE_SENDING?@"发送中":
            s_fileSnapshot[i].phase==HOP_XFER_FILE_RECEIVING?@"接收中":
            s_fileSnapshot[i].phase==HOP_XFER_FILE_VERIFYING?@"校验中":@"排队中";
        NSString *kind=s_fileSnapshot[i].is_directory?(s_collapsedFolders[i]?@"▸ 目录":@"▾ 目录"):@"文件";
        [files appendFormat:@"%@%@ %@    %@ / %@    %@\n",indent,kind,path,
            byte_count(s_fileSnapshot[i].bytes_done),byte_count(s_fileSnapshot[i].bytes_total),state];
    }
    NSString *nextFiles=files.length?files:@"当前没有文件明细";
    if(![s_xferFiles.string isEqualToString:nextFiles])s_xferFiles.string=nextFiles;
    hop_xfer_history_entry history[HOP_XFER_HISTORY_LIMIT];
    int hn=xfer_get_history(history,HOP_XFER_HISTORY_LIMIT);
    NSMutableString *rows=[NSMutableString string];
    for(int i=0;i<hn;++i){
        NSString *d=history[i].direction==HOP_XFER_DIR_SEND?@"发送":@"接收";
        NSString *p=history[i].phase==HOP_XFER_COMPLETED?@"完成":
            history[i].phase==HOP_XFER_PARTIAL?@"部分失败":
            history[i].phase==HOP_XFER_CANCELLED?@"已取消":@"失败";
        [rows appendFormat:@"%@  ·  %@  ·  %@  ·  文件 %u/%u  ·  %@\n",
            d,p,utf8(history[i].name),history[i].files_done,history[i].files_total,byte_count(history[i].bytes_total)];
    }
    NSString *nextHistory=rows.length?rows:@"暂无传输历史";
    if(![s_xferHistory.string isEqualToString:nextHistory])s_xferHistory.string=nextHistory;
}
static void refresh_logs(void) {
    if (!s_logView) return;
    if(s_logBusy)return;
    s_logBusy=YES;
    NSString *path=utf8(g_mac.log_path);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        @autoreleasepool {
            NSString *text=@"暂无日志记录。";
            @try {
                NSFileHandle *file=[NSFileHandle fileHandleForReadingAtPath:path];
                if(file){unsigned long long size=[file seekToEndOfFile];
                    [file seekToFileOffset:size>262144?size-262144:0];
                    NSData *data=[file readDataToEndOfFile];[file closeFile];
                    /* A tail may begin in the middle of a UTF-8 sequence. */
                    if(size>262144){const unsigned char *p=data.bytes;NSUInteger i=0;
                        while(i<data.length&&p[i]!='\n')++i;
                        if(i<data.length)data=[data subdataWithRange:NSMakeRange(i+1,data.length-i-1)];}
                    text=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"日志编码无法识别。";
                }
            } @catch(NSException *exception){text=exception.reason ?: @"读取日志失败。";}
            dispatch_async(dispatch_get_main_queue(), ^{s_logBusy=NO;s_logView.string=text;});
        }
    });
}
static NSString *link_status(void) {
    if (!s_connected) return g_mac.cfg.role==HOP_ROLE_SERVER?@"等待客户端连接":g_mac.cfg.peer_host[0]?@"正在连接服务器":@"请先填写服务器 IP";
    return s_sharing?(g_mac.cfg.role==HOP_ROLE_SERVER?@"键鼠已移交客户端":@"正在接收服务器键鼠"):@"已连接";
}
static void refresh_connection_labels(void) {
    NSString *status=link_status();
    s_headerStatus.stringValue=status;
    s_overviewStatus.stringValue=status;
    s_overviewPeer.stringValue=[NSString stringWithFormat:@"%@  ·  %@  ·  控制端口 %u  ·  数据端口 %u",
        g_mac.cfg.role==HOP_ROLE_SERVER?@"服务器 · 提供键鼠":@"客户端 · 接收键鼠",g_mac.cfg.role==HOP_ROLE_SERVER?s_addresses:utf8(g_mac.cfg.peer_host), (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port];
    s_overviewChannels.stringValue=[NSString stringWithFormat:@"控制通道：%@    数据通道：%@",
        s_connected?@"已连接":@"等待连接",xfer_connected()?@"已连接":@"等待连接"];
    s_overviewSharing.stringValue=s_sharing?(g_mac.cfg.role==HOP_ROLE_SERVER?@"键鼠共享：已移交客户端":@"键鼠共享：正在接收服务器输入"):@"键鼠共享：等待鼠标边缘切换";
    s_connectionRole.stringValue=g_mac.cfg.role==HOP_ROLE_SERVER?@"本机角色：服务器 · 提供键鼠":@"本机角色：客户端 · 接收键鼠";
    s_connectionStatus.stringValue=status;
    s_connectionPeer.stringValue=g_mac.cfg.role==HOP_ROLE_SERVER?[NSString stringWithFormat:@"本机 IP：%@",s_addresses]:[NSString stringWithFormat:@"服务器 IP：%@",utf8(g_mac.cfg.peer_host)];
    s_connectionPorts.stringValue=[NSString stringWithFormat:@"控制端口：%u    数据端口：%u",
        (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port];
    s_footer.stringValue=[NSString stringWithFormat:@"HopDesk  v%s    ·    %@",HOP_VERSION,status];
}
static void show_page(int page) {
    if(page<0||page>=6)page=0;
    s_page=page;
    for(int i=0;i<6;++i){
        s_hosts[i].hidden=i!=page;
        s_nav[i].layer.backgroundColor=(i==page?[NSColor colorWithSRGBRed:.90 green:.95 blue:.99 alpha:1]:[NSColor clearColor]).CGColor;
        s_nav[i].contentTintColor=i==page?[NSColor systemBlueColor]:[NSColor labelColor];
        s_nav[i].state=i==page?NSControlStateValueOn:NSControlStateValueOff;
    }
    s_pageTitle.stringValue=kPageNames[page];
    NSClipView *clip=s_hosts[page].contentView;[clip scrollToPoint:NSMakePoint(0,MAX(0,s_pages[page].frame.size.height-clip.bounds.size.height))];[s_hosts[page] reflectScrolledClipView:clip];
    if(page==3)refresh_transfer_page();
    if(page==4)refresh_logs();
}
static NSView *add_page(NSView *root) {
    NSView *v=ui_page(root);
    return v;
}
static void build_settings_page(NSView *p) {
    NSView *left=ui_card(p,NSMakeRect(20,180,400,470));
    NSView *right=ui_card(p,NSMakeRect(440,180,400,470));
    ui_label(left,@"连接与身份",NSMakeRect(18,424,360,24),15,YES);
    const CGFloat y[]={380,334,288,242,196,150,104};
    NSString *labels[]={@"设备名称",@"服务端地址",@"控制端口",@"数据端口",@"共享 Token",@"服务端边缘",@"客户端边缘"};
    for(int i=0;i<5;++i){
        ui_label(left,labels[i],NSMakeRect(18,y[i]+8,112,22),12,NO);
        s_cfg[i]=ui_field(left,@"",NSMakeRect(136,y[i],242,30),i==CFG_TOKEN);
    }
    for(int i=0;i<2;++i){
        ui_label(left,labels[5+i],NSMakeRect(18,y[5+i]+8,112,22),12,NO);
        NSPopUpButton *pop=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(136,y[5+i],242,30) pullsDown:NO];
        [pop addItemsWithTitles:@[@"左边缘",@"右边缘",@"上边缘",@"下边缘"]];
        [left addSubview:pop]; s_edgePopups[i]=pop;
    }
    ui_label(left,@"本机角色",NSMakeRect(18,68,112,22),12,NO);
    s_rolePopup=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(136,60,242,30) pullsDown:NO];
    [s_rolePopup addItemsWithTitles:@[@"服务器 · 提供键鼠",@"客户端 · 接收键鼠"]];
    s_rolePopup.target=s_uiController;s_rolePopup.action=@selector(roleChanged:);[left addSubview:s_rolePopup];
    ui_label(left,@"角色与连接参数保存后重启生效。",NSMakeRect(18,22,360,22),11,NO);
    ui_label(right,@"交互与显示器",NSMakeRect(18,424,360,24),15,YES);
    const int ids[]={CFG_SWITCH_DELAY,CFG_COOLDOWN,CFG_MARGIN,CFG_DISP_WIN,CFG_DISP_MAC,CFG_WAKE,CFG_CHUNK};
    NSString *names[]={@"边缘触发延迟（ms）",@"切换冷却（ms）",@"边缘内缩（px）",@"Windows 输入源",@"Mac 输入源",@"唤醒时长（秒）",@"传输分块（KB）"};
    const CGFloat ry[]={380,340,300,260,220,180,140};
    for(int i=0;i<7;++i){
        ui_label(right,names[i],NSMakeRect(18,ry[i]+8,170,22),12,NO);
        s_cfg[ids[i]]=ui_field(right,@"",NSMakeRect(190,ry[i],190,30),NO);
    }
    ui_label(right,@"Mac 切源命令",NSMakeRect(18,96,160,20),12,NO);
    s_cfg[CFG_MAC_INPUT]=ui_field(right,@"",NSMakeRect(18,64,360,28),NO);
    ui_label(right,@"Mac 睡眠命令",NSMakeRect(18,36,160,20),12,NO);
    s_cfg[CFG_MAC_SLEEP]=ui_field(right,@"",NSMakeRect(18,6,360,28),NO);
    ui_label(p,@"输入源支持十六进制（例如 0x0F）。保存后，连接和认证参数将在重启后完整生效。",NSMakeRect(22,132,810,22),11,NO);
    ui_button(p,@"保存设置",NSMakeRect(610,78,112,34),s_uiController,@selector(saveSettings:));
    ui_button(p,@"恢复默认",NSMakeRect(738,78,112,34),s_uiController,@selector(resetSettings:));
    s_cfgNote=ui_label(p,@"修改配置后点击保存。",NSMakeRect(22,38,560,24),11,NO);
}
static void build_pages(NSView *root) {
    for(int i=0;i<6;++i)s_pages[i]=add_page(root);
    NSView *p=s_pages[0];
    NSView *card=ui_card(p,NSMakeRect(20,492,840,150));
    ui_label(card,@"设备状态",NSMakeRect(22,108,780,22),13,YES);
    s_overviewStatus=ui_label(card,@"等待连接",NSMakeRect(22,68,780,34),24,YES);
    s_overviewPeer=ui_label(card,@"服务端地址",NSMakeRect(22,38,780,22),12,NO);
    s_overviewChannels=ui_label(card,@"控制通道：等待连接    数据通道：等待连接",NSMakeRect(22,12,780,20),11,NO);
    card=ui_card(p,NSMakeRect(20,296,840,172));
    ui_label(card,@"显示器控制",NSMakeRect(22,128,780,22),13,YES);
    s_overviewSharing=ui_label(card,@"键鼠共享：等待鼠标边缘切换",NSMakeRect(22,98,780,22),12,NO);
    ui_button(card,@"切换到 Mac 显示",NSMakeRect(22,38,148,34),s_uiController,@selector(switchMac:));
    ui_button(card,@"切回 Windows 显示",NSMakeRect(184,38,160,34),s_uiController,@selector(switchWin:));
    ui_button(card,@"唤醒显示",NSMakeRect(358,38,112,34),s_uiController,@selector(wakeDisplay:));
    ui_button(card,@"显示睡眠",NSMakeRect(484,38,112,34),s_uiController,@selector(sleepDisplay:));
    card=ui_card(p,NSMakeRect(20,94,840,176));
    ui_label(card,@"快速入口",NSMakeRect(22,130,780,22),13,YES);
    ui_button(card,@"打开传输管理",NSMakeRect(22,72,148,36),s_uiController,@selector(selectPage:)).tag=3;
    ui_label(card,@"连接、设备设置、传输记录、运行日志和软件更新都集中在左侧导航中。",
        NSMakeRect(190,76,620,28),12,NO);

    p=s_pages[1];
    card=ui_card(p,NSMakeRect(20,420,840,220));
    ui_label(card,@"连接状态",NSMakeRect(22,176,780,22),13,YES);
    s_connectionRole=ui_label(card,@"本机角色：macOS 客户端",NSMakeRect(22,138,780,22),12,NO);
    s_connectionStatus=ui_label(card,@"等待连接",NSMakeRect(22,104,780,22),14,YES);
    s_connectionPeer=ui_label(card,@"服务端地址：未配置",NSMakeRect(22,70,780,22),12,NO);
    s_connectionPorts=ui_label(card,@"控制端口：24817    数据端口：24818",NSMakeRect(22,38,780,22),12,NO);
    ui_button(card,@"重新连接",NSMakeRect(22,0,130,32),s_uiController,@selector(reconnect:));
    ui_label(p,@"HopDesk 会自动重连。点击“重新连接”会关闭当前控制连接，并立即尝试重新建立连接。",
        NSMakeRect(22,360,810,42),11,NO);

    p=s_pages[2]; build_settings_page(p);

    p=s_pages[3];
    card=ui_card(p,NSMakeRect(20,438,840,204));
    ui_label(card,@"当前任务",NSMakeRect(22,178,780,18),12,YES);
    s_xferTitle=ui_label(card,@"暂无传输任务",NSMakeRect(22,150,780,24),16,YES);
    s_xferDetail=ui_label(card,@"等待文件传输",NSMakeRect(22,126,780,18),10,NO);
    s_xferCounts=ui_label(card,@"文件 0 / 0    排队 0    数据 0 B / 0 B",NSMakeRect(22,102,780,18),10,NO);
    s_xferBudget=ui_label(card,@"预算 jobs 0/16 · entries 0/32768 · pending 0 B/8 GB",NSMakeRect(22,80,780,16),9,NO);
    s_xferMemory=ui_label(card,@"内存 image 0/256 MB · RX 0/8 slots",NSMakeRect(22,60,780,16),9,NO);
    s_xferBudgetStats=ui_label(card,@"峰值与拒绝计数等待采样",NSMakeRect(22,40,780,16),9,NO);
    s_xferBudgetLifecycle=ui_label(card,@"资源释放与图片缓冲转移等待采样",NSMakeRect(22,20,780,16),9,NO);
    s_xferProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(22,4,780,10)];
    s_xferProgress.minValue=0;s_xferProgress.maxValue=100;s_xferProgress.indeterminate=NO;
    [card addSubview:s_xferProgress];
    card=ui_card(p,NSMakeRect(20,220,840,220));
    ui_label(card,@"文件明细",NSMakeRect(22,184,780,22),13,YES);
    NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,16,804,158)];
    scroll.hasVerticalScroller=YES;scroll.hasHorizontalScroller=YES;
    s_xferFiles=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,150)];
    s_xferFiles.editable=NO;s_xferFiles.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    scroll.documentView=s_xferFiles;[card addSubview:scroll];
    card=ui_card(p,NSMakeRect(20,24,840,180));
    ui_label(card,@"历史记录（旧版）",NSMakeRect(22,144,780,22),13,YES);
    NSScrollView *histScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,16,804,116)];
    histScroll.hasVerticalScroller=YES;histScroll.hasHorizontalScroller=YES;
    s_xferHistory=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,110)];
    s_xferHistory.editable=NO;s_xferHistory.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    histScroll.documentView=s_xferHistory;[card addSubview:histScroll];
    ui_button(p,@"展开/折叠目录",NSMakeRect(510,0,130,30),s_uiController,@selector(toggleFolder:));
    s_retryButton=ui_button(p,@"重试选中文件",NSMakeRect(650,0,112,30),s_uiController,@selector(retryFile:));
    /* Live Task Manager entries, separate from legacy file and history detail. */
    [p setFrameSize:NSMakeSize(880,890)];
    card=ui_card(p,NSMakeRect(20,660,840,210));
    ui_label(card,@"活动任务（选择后单独取消）",NSMakeRect(22,180,780,20),13,YES);
    for(int d=0;d<2;++d){
        CGFloat x=d?430:22;
        ui_label(card,d?@"接收任务":@"发送任务",NSMakeRect(x,154,370,20),11,YES);
        NSScrollView *taskScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(x,52,370,96)];
        taskScroll.hasVerticalScroller=YES;taskScroll.borderType=NSBezelBorder;
        NSTableView *table=[[NSTableView alloc] initWithFrame:NSMakeRect(0,0,370,96)];
        NSTableColumn *column=[[NSTableColumn alloc] initWithIdentifier:@"task"];
        column.width=360;[table addTableColumn:column];table.headerView=nil;table.dataSource=(id<NSTableViewDataSource>)s_uiController;
        table.allowsEmptySelection=YES;taskScroll.documentView=table;[card addSubview:taskScroll];s_taskTables[d]=table;
        NSButton *cancel=ui_button(card,d?@"取消选中接收任务":@"取消选中发送任务",NSMakeRect(x,12,168,30),s_uiController,@selector(cancelTask:));cancel.tag=d;
    }

    /* Keep active tasks at the top; add session-only history below existing content. */
    for(NSView *v in p.subviews){NSRect frame=v.frame;frame.origin.y+=230;v.frame=frame;}
    [p setFrameSize:NSMakeSize(880,1120)];
    card=ui_card(p,NSMakeRect(20,10,840,210));
    ui_label(card,@"本次运行的任务历史（最多 256 条，重启后清空）",NSMakeRect(22,180,780,20),13,YES);
    s_historyDirection=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(22,146,150,26) pullsDown:NO];
    [s_historyDirection addItemsWithTitles:@[@"全部方向",@"发送",@"接收"]];[card addSubview:s_historyDirection];
    s_historyState=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(185,146,160,26) pullsDown:NO];
    [s_historyState addItemsWithTitles:@[@"全部状态",@"已完成",@"部分完成",@"失败",@"已取消",@"已中断"]];[card addSubview:s_historyState];
    s_historyOrder=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(358,146,160,26) pullsDown:NO];
    [s_historyOrder addItemsWithTitles:@[@"最近完成优先",@"最早完成优先"]];[card addSubview:s_historyOrder];
    NSScrollView *taskHistoryScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,14,804,122)];
    taskHistoryScroll.hasVerticalScroller=YES;taskHistoryScroll.hasHorizontalScroller=YES;
    s_taskHistory=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,116)];
    s_taskHistory.editable=NO;s_taskHistory.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    taskHistoryScroll.documentView=s_taskHistory;[card addSubview:taskHistoryScroll];

    p=s_pages[4];
    card=ui_card(p,NSMakeRect(20,70,840,570));
    ui_label(card,@"运行日志",NSMakeRect(22,526,780,22),13,YES);
    NSScrollView *logScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,60,804,450)];
    logScroll.hasVerticalScroller=YES;logScroll.hasHorizontalScroller=YES;
    s_logView=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,440)];
    s_logView.editable=NO;s_logView.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    logScroll.documentView=s_logView;[card addSubview:logScroll];
    ui_button(card,@"刷新日志",NSMakeRect(584,12,108,32),s_uiController,@selector(refreshLogs:));
    ui_button(card,@"复制日志",NSMakeRect(710,12,108,32),s_uiController,@selector(copyLogs:));

    p=s_pages[5];
    card=ui_card(p,NSMakeRect(20,420,840,220));
    ui_label(card,@"软件更新",NSMakeRect(22,176,780,22),13,YES);
    s_updateVersion=ui_label(card,[NSString stringWithFormat:@"当前版本：v%s",HOP_VERSION],NSMakeRect(22,132,780,30),20,YES);
    s_updateSource=ui_label(card,@"更新源：GitHub Releases",NSMakeRect(22,96,780,22),12,NO);
    ui_label(card,@"检查更新会读取公开更新清单。发现新版本后，可下载、校验并自动安装。",
        NSMakeRect(22,58,780,24),11,NO);
    ui_button(card,@"检查更新",NSMakeRect(22,12,130,34),s_uiController,@selector(checkUpdates:));
    ui_label(p,@"如果更新源尚未发布 latest.txt，检查会明确提示读取失败，不会将其误报为“已是最新版本”。",
        NSMakeRect(22,360,820,42),11,NO);
    for(int i=0;i<6;++i)s_hosts[i].hidden=i!=0;
    populate_settings();
}

@implementation HopDeskMainController
- (void)selectPage:(id)sender {
    NSInteger page=[sender tag];
    if ([sender isKindOfClass:[NSButton class]] && page==0 && [sender title].length==0) page=0;
    show_page((int)page);
}
- (void)roleChanged:(id)sender {(void)sender;s_cfg[CFG_HOST].enabled=s_rolePopup.indexOfSelectedItem==1;}
- (void)saveSettings:(id)sender {(void)sender;save_settings();}
- (void)resetSettings:(id)sender {(void)sender;reset_settings();}
- (void)reconnect:(id)sender {(void)sender;net_request_reconnect();}
- (void)switchMac:(id)sender {
    (void)sender;
    disp_wake(g_mac.cfg.disp_wake_sec);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        disp_set_input(g_mac.cfg.disp_mac_input);
    });
}
- (void)switchWin:(id)sender {(void)sender;disp_set_input(g_mac.cfg.disp_win_input);}
- (void)wakeDisplay:(id)sender {(void)sender;disp_wake(g_mac.cfg.disp_wake_sec);}
- (void)sleepDisplay:(id)sender {(void)sender;disp_sleep();}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    int d=tableView==s_taskTables[1]?1:0;return (NSInteger)s_taskRows[d].count;
}
- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)column;int d=tableView==s_taskTables[1]?1:0;
    return row>=0 && (NSUInteger)row<s_taskRows[d].count ? s_taskRows[d][(NSUInteger)row][@"text"] : @"";
}
- (void)cancelTask:(id)sender {
    int d=[sender tag]==1?1:0;hop_xfer_task_id id=selected_task(d);
    if(!id){NSAlert *a=[NSAlert new];a.messageText=@"请先选择要取消的任务。";[a runModal];return;}
    if(!xfer_connected()){
        NSAlert *a=[NSAlert new];a.messageText=@"数据通道未连接";
        a.informativeText=@"取消后对端的临时文件可能要等待对端连接恢复或断线超时后清理。仍要取消吗？";
        [a addButtonWithTitle:@"取消任务"];[a addButtonWithTitle:@"返回"];
        if([a runModal]!=NSAlertFirstButtonReturn)return;
    }
    if(!xfer_cancel_task(id)){NSAlert *a=[NSAlert new];a.messageText=@"任务已经结束或正在完成，无法取消。";[a runModal];}
    refresh_transfer_page();
}
- (void)retryFile:(id)sender {
    (void)sender;
    if(!s_fileSnapshot||s_fileDirection!=HOP_XFER_DIR_SEND||!s_xferFiles)return;
    NSString *text=s_xferFiles.string ?: @"";
    NSUInteger line=0,limit=MIN(s_xferFiles.selectedRange.location,text.length);
    for(NSUInteger i=0;i<limit;++i)if([text characterAtIndex:i]=='\n')line++;
    if(line<(NSUInteger)s_visibleFileCount)(void)xfer_retry_file(s_fileBatch,s_visibleFileIndices[line]);
}
- (void)toggleFolder:(id)sender {
    (void)sender;
    if(!s_fileSnapshot||!s_xferFiles)return;
    NSString *text=s_xferFiles.string ?: @"";
    NSUInteger line=0,limit=MIN(s_xferFiles.selectedRange.location,text.length);
    for(NSUInteger i=0;i<limit;++i)if([text characterAtIndex:i]=='\n')line++;
    if(line<(NSUInteger)s_visibleFileCount){
        int index=s_visibleFileIndices[line];
        if(s_fileSnapshot[index].is_directory)s_collapsedFolders[index]=!s_collapsedFolders[index];
    }
}
- (void)refreshLogs:(id)sender {(void)sender;refresh_logs();}
- (void)copyLogs:(id)sender {
    (void)sender;
    NSPasteboard *pb=[NSPasteboard generalPasteboard];[pb clearContents];
    [pb setString:s_logView.string ?: @"" forType:NSPasteboardTypeString];
}
- (void)checkUpdates:(id)sender {(void)sender;statusbar_check_updates();}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if(sender!=s_mainWindow)return YES;
    [sender orderOut:nil];
    [s_mainTimer invalidate];s_mainTimer=nil;
    return NO;
}
@end

static void create_main_window(void) {
    if(s_mainWindow)return;
    s_draft=g_mac.cfg;char addresses[512];local_addresses(addresses,sizeof(addresses));s_addresses=utf8(addresses);
    NSRect frame=NSMakeRect(0,0,1140,800);
    s_mainWindow=[[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    s_mainWindow.releasedWhenClosed=NO;
    s_mainWindow.title=@"HopDesk";
    s_mainWindow.minSize=NSMakeSize(1140,620);
    s_uiController=[[HopDeskMainController alloc] init];
    s_mainWindow.delegate=(id<NSWindowDelegate>)s_uiController;
    NSView *root=s_mainWindow.contentView;
    root.wantsLayer=YES;root.layer.backgroundColor=[NSColor windowBackgroundColor].CGColor;

    NSVisualEffectView *sidebar=[[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0,0,210,800)];
    sidebar.material=NSVisualEffectMaterialSidebar;sidebar.blendingMode=NSVisualEffectBlendingModeBehindWindow;
    sidebar.autoresizingMask=NSViewHeightSizable;
    [root addSubview:sidebar];
    NSTextField *brand=ui_label(sidebar,@"HopDesk",NSMakeRect(24,740,160,32),22,YES);
    brand.autoresizingMask=NSViewMinYMargin;
    ui_label(sidebar,@"跨设备协作",NSMakeRect(26,714,150,20),11,NO).autoresizingMask=NSViewMinYMargin;
    for(int i=0;i<6;++i){
        NSButton *b=[NSButton buttonWithTitle:kPageNames[i] target:s_uiController action:@selector(selectPage:)];
        b.frame=NSMakeRect(18,620-i*52,174,38);b.bordered=NO;b.wantsLayer=YES;b.layer.cornerRadius=8;b.autoresizingMask=NSViewMinYMargin;
        [b setButtonType:NSButtonTypePushOnPushOff];
        b.tag=i;b.alignment=NSTextAlignmentLeft;b.font=[NSFont systemFontOfSize:13];
        [sidebar addSubview:b];s_nav[i]=b;
    }
    s_pageTitle=ui_label(root,@"概览",NSMakeRect(246,736,360,34),23,YES);
    s_pageTitle.autoresizingMask=NSViewMinYMargin;
    s_headerStatus=ui_label(root,@"等待连接",NSMakeRect(850,742,250,22),12,YES);
    s_headerStatus.autoresizingMask=NSViewMinYMargin|NSViewMinXMargin;
    s_footer=ui_label(root,@"HopDesk",NSMakeRect(236,6,860,18),10,NO);
    build_pages(root);
    NSRect visible=NSScreen.mainScreen.visibleFrame;
    if(s_mainWindow.frame.size.height>visible.size.height){NSRect adjusted=s_mainWindow.frame;adjusted.size.height=visible.size.height;adjusted.origin.y=visible.origin.y;[s_mainWindow setFrame:adjusted display:NO];}
    show_page(0);
}

void statusbar_show_main(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        create_main_window();
        [NSApp activateIgnoringOtherApps:YES];
        [s_mainWindow makeKeyAndOrderFront:nil];
        if(!s_mainTimer){
            s_mainTimer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer){
                (void)timer;main_ui_refresh();
            }];
        }
        main_ui_refresh();
    });
}
void main_ui_show_page(int page) {
    dispatch_async(dispatch_get_main_queue(), ^{
        create_main_window();
        show_page(page);
        [NSApp activateIgnoringOtherApps:YES];
        [s_mainWindow makeKeyAndOrderFront:nil];
        if(!s_mainTimer)s_mainTimer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer){(void)timer;main_ui_refresh();}];
        main_ui_refresh();
    });
}
void main_ui_set_status(int connected,int sharing) {
    s_connected=connected;s_sharing=sharing;
    dispatch_async(dispatch_get_main_queue(), ^{ if(s_mainWindow)refresh_connection_labels(); });
}
void main_ui_refresh(void) {
    if(!s_mainWindow)return;
    refresh_connection_labels();
    if(s_page==3)refresh_transfer_page();
}
void main_ui_shutdown(void) {
    [s_mainTimer invalidate];s_mainTimer=nil;
    [s_mainWindow orderOut:nil];
}
