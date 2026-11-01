/* =====================================================================
 * HopDesk - macOS native main window
 * Cocoa implementation of the shared six-page application UI.
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#include "mac_app.h"
#include "xfer.h"
#include "main_ui.h"
#include "../core/xfer_status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CFG_NAME, CFG_HOST, CFG_PORT, CFG_DATA_PORT, CFG_TOKEN,
    CFG_SERVER_EDGE, CFG_CLIENT_EDGE, CFG_SWITCH_DELAY, CFG_COOLDOWN,
    CFG_MARGIN, CFG_DISP_WIN, CFG_DISP_MAC, CFG_WAKE, CFG_CHUNK,
    CFG_MAC_INPUT, CFG_MAC_SLEEP, CFG_COUNT
};

static NSWindow *s_mainWindow;
static NSView *s_pages[6];
static NSButton *s_nav[6];
static NSTextField *s_pageTitle, *s_headerStatus, *s_footer;
static NSTextField *s_overviewStatus, *s_overviewPeer, *s_overviewChannels, *s_overviewSharing;
static NSTextField *s_connectionRole, *s_connectionStatus, *s_connectionPeer, *s_connectionPorts;
static NSTextField *s_cfg[CFG_COUNT], *s_cfgNote;
static NSPopUpButton *s_edgePopups[2];
static NSTextField *s_xferTitle, *s_xferDetail, *s_xferCounts;
static NSProgressIndicator *s_xferProgress;
static NSTextView *s_xferFiles, *s_xferHistory, *s_logView;
static NSButton *s_retryButton;
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

static NSString *const kPageNames[6] = {@"概览",@"连接",@"设置",@"传输",@"日志",@"更新"};

@interface HopDeskMainController : NSObject <NSWindowDelegate>
- (void)selectPage:(id)sender;
- (void)saveSettings:(id)sender;
- (void)resetSettings:(id)sender;
- (void)reconnect:(id)sender;
- (void)switchMac:(id)sender;
- (void)switchWin:(id)sender;
- (void)wakeDisplay:(id)sender;
- (void)sleepDisplay:(id)sender;
- (void)cancelTransfer:(id)sender;
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
    [parent addSubview:v positioned:NSWindowBelow relativeTo:nil];
    return v;
}
static NSView *ui_page(NSView *root) {
    NSView *v=[[NSView alloc] initWithFrame:NSMakeRect(230,28,880,680)];
    v.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    v.hidden=YES;
    [root addSubview:v];
    return v;
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
    if (!s || !s[0]) return fallback;
    char *end=NULL;
    unsigned long n=strtoul(s,&end,0);
    return end==s || *end ? fallback : n;
}
static void populate_settings(void) {
    hop_config *c=&g_mac.cfg;
    set_cfg_text(CFG_NAME,utf8(c->device_name));
    set_cfg_text(CFG_HOST,utf8(c->peer_host));
    set_cfg_text(CFG_PORT,[NSString stringWithFormat:@"%u",(unsigned)c->port]);
    set_cfg_text(CFG_DATA_PORT,[NSString stringWithFormat:@"%u",(unsigned)c->data_port]);
    set_cfg_text(CFG_TOKEN,utf8(c->token));
    [s_edgePopups[0] selectItemAtIndex:c->server_edge==HOP_EDGE_RIGHT?1:0];
    [s_edgePopups[1] selectItemAtIndex:c->client_edge==HOP_EDGE_RIGHT?1:0];
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
    hop_config next=g_mac.cfg;
    const char *name=s_cfg[CFG_NAME].stringValue.UTF8String ?: "";
    const char *host=s_cfg[CFG_HOST].stringValue.UTF8String ?: "";
    const char *token=s_cfg[CFG_TOKEN].stringValue.UTF8String ?: "";
    const char *input=s_cfg[CFG_MAC_INPUT].stringValue.UTF8String ?: "";
    const char *sleep=s_cfg[CFG_MAC_SLEEP].stringValue.UTF8String ?: "";
    if (!name[0] || strlen(name)>=sizeof(next.device_name) || strlen(host)>=sizeof(next.peer_host) ||
        strlen(token)>=sizeof(next.token) || strlen(input)>=sizeof(next.mac_input_cmd) ||
        strlen(sleep)>=sizeof(next.mac_sleep_cmd)) {
        NSAlert *a=[[NSAlert alloc] init]; a.messageText=@"设置无效";
        a.informativeText=@"设备名称不能为空；地址、Token 和命令长度也必须在允许范围内。";
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
    if (!hop_config_save_atomic(&next,g_mac.config_path)) {
        NSAlert *a=[[NSAlert alloc] init]; a.messageText=@"保存失败";
        a.informativeText=@"配置保存失败。原有配置文件保持不变，请检查目录权限和磁盘空间。";
        a.alertStyle=NSAlertStyleCritical; [a runModal]; return;
    }
    g_mac.cfg=next;
    s_cfgNote.stringValue=@"设置已保存。连接、认证和显示器参数将在重启 HopDesk 后完整生效。";
    hop_log(HOP_LOG_INFO,"ui: configuration saved; restart required for network settings");
    main_ui_refresh();
}
static void reset_settings(void) {
    hop_config_defaults(&g_mac.cfg);
    snprintf(g_mac.cfg.device_name,sizeof(g_mac.cfg.device_name),"%s","Mac");
    populate_settings();
    s_cfgNote.stringValue=@"已恢复默认值；点击“保存设置”写入配置文件。";
}
static void refresh_transfer_page(void) {
    if (!s_xferTitle) return;
    hop_xfer_status st; memset(&st,0,sizeof(st)); xfer_get_status(&st);
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
    s_xferDetail.stringValue=[NSString stringWithFormat:@"%@  ·  %@  ·  %@",direction,phase,utf8(st.detail)];
    s_xferCounts.stringValue=[NSString stringWithFormat:@"文件 %u / %u    排队 %u    数据 %@ / %@",
        st.files_done,st.files_total,st.queued_count,byte_count(st.bytes_done),byte_count(st.bytes_total)];
    s_xferProgress.doubleValue=st.bytes_total?MIN(100.0,100.0*(double)st.bytes_done/(double)st.bytes_total):(st.active?0:100);
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
    s_xferFiles.string=files.length?files:@"当前没有文件明细";
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
    s_xferHistory.string=rows.length?rows:@"暂无传输历史";
}
static void refresh_logs(void) {
    if (!s_logView) return;
    NSError *error=nil;
    NSString *path=utf8(g_mac.log_path);
    NSString *log=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    if (!log) log=error.localizedDescription ?: @"暂无日志记录。";
    if (log.length>220000) log=[log substringFromIndex:log.length-220000];
    s_logView.string=log;
}
static NSString *link_status(void) {
    if (!s_connected) return @"等待连接";
    return s_sharing?@"键鼠正在 Mac":@"已连接";
}
static void refresh_connection_labels(void) {
    NSString *status=link_status();
    s_headerStatus.stringValue=status;
    s_overviewStatus.stringValue=status;
    s_overviewPeer.stringValue=[NSString stringWithFormat:@"macOS 客户端  ·  服务端 %@  ·  控制端口 %u  ·  数据端口 %u",
        utf8(g_mac.cfg.peer_host), (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port];
    s_overviewChannels.stringValue=[NSString stringWithFormat:@"控制通道：%@    数据通道：%@",
        s_connected?@"已连接":@"等待连接",xfer_connected()?@"已连接":@"等待连接"];
    s_overviewSharing.stringValue=s_sharing?@"键鼠共享：当前由 Mac 接收输入":@"键鼠共享：等待鼠标边缘切换";
    s_connectionRole.stringValue=@"本机角色：macOS 客户端";
    s_connectionStatus.stringValue=status;
    s_connectionPeer.stringValue=[NSString stringWithFormat:@"服务端地址：%@",utf8(g_mac.cfg.peer_host)];
    s_connectionPorts.stringValue=[NSString stringWithFormat:@"控制端口：%u    数据端口：%u",
        (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port];
    s_footer.stringValue=[NSString stringWithFormat:@"HopDesk  v%s    ·    %@",HOP_VERSION,status];
}
static void show_page(int page) {
    if(page<0||page>=6)page=0;
    s_page=page;
    for(int i=0;i<6;++i){
        s_pages[i].hidden=i!=page;
        s_nav[i].state=i==page?NSControlStateValueOn:NSControlStateValueOff;
    }
    s_pageTitle.stringValue=kPageNames[page];
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
        [pop addItemsWithTitles:@[@"左边缘",@"右边缘"]];
        [left addSubview:pop]; s_edgePopups[i]=pop;
    }
    ui_label(right,@"交互与显示器",NSMakeRect(18,424,360,24),15,YES);
    const int ids[]={CFG_SWITCH_DELAY,CFG_COOLDOWN,CFG_MARGIN,CFG_DISP_WIN,CFG_DISP_MAC,CFG_WAKE,CFG_CHUNK};
    NSString *names[]={@"边缘触发延迟（ms）",@"切换冷却（ms）",@"边缘内缩（px）",@"Windows 输入源",@"Mac 输入源",@"唤醒时长（秒）",@"传输分块（KB）"};
    for(int i=0;i<7;++i){
        ui_label(right,names[i],NSMakeRect(18,y[i]+8,170,22),12,NO);
        s_cfg[ids[i]]=ui_field(right,@"",NSMakeRect(190,y[i],190,30),NO);
    }
    ui_label(right,@"Mac 切源命令",NSMakeRect(18,96,160,20),12,NO);
    s_cfg[CFG_MAC_INPUT]=ui_field(right,@"",NSMakeRect(18,64,360,28),NO);
    ui_label(right,@"Mac 睡眠命令",NSMakeRect(18,36,160,20),12,NO);
    s_cfg[CFG_MAC_SLEEP]=ui_field(right,@"",NSMakeRect(18,6,360,28),NO);
    ui_label(p,@"输入源支持十六进制（如 0x0F）。保存后，连接和认证参数将在重启后完整生效。",NSMakeRect(22,132,810,22),11,NO);
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
    ui_label(p,@"Mac 客户端会持续自动重连。点击“重新连接”会关闭当前控制连接并立即进入重连流程。",
        NSMakeRect(22,360,810,42),11,NO);

    p=s_pages[2]; build_settings_page(p);

    p=s_pages[3];
    card=ui_card(p,NSMakeRect(20,458,840,184));
    ui_label(card,@"当前任务",NSMakeRect(22,142,780,22),13,YES);
    s_xferTitle=ui_label(card,@"暂无传输任务",NSMakeRect(22,104,780,28),18,YES);
    s_xferDetail=ui_label(card,@"等待文件传输",NSMakeRect(22,76,780,20),11,NO);
    s_xferCounts=ui_label(card,@"文件 0 / 0    排队 0    数据 0 B / 0 B",NSMakeRect(22,48,780,20),11,NO);
    s_xferProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(22,22,780,12)];
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
    ui_label(card,@"传输历史",NSMakeRect(22,144,780,22),13,YES);
    NSScrollView *histScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,16,804,116)];
    histScroll.hasVerticalScroller=YES;histScroll.hasHorizontalScroller=YES;
    s_xferHistory=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,110)];
    s_xferHistory.editable=NO;s_xferHistory.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    histScroll.documentView=s_xferHistory;[card addSubview:histScroll];
    ui_button(p,@"展开/折叠目录",NSMakeRect(510,0,130,30),s_uiController,@selector(toggleFolder:));
    s_retryButton=ui_button(p,@"重试选中文件",NSMakeRect(650,0,112,30),s_uiController,@selector(retryFile:));
    ui_button(p,@"取消传输",NSMakeRect(772,0,88,30),s_uiController,@selector(cancelTransfer:));

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
    for(int i=0;i<6;++i)s_pages[i].hidden=i!=0;
    populate_settings();
}

@implementation HopDeskMainController
- (void)selectPage:(id)sender {
    NSInteger page=[sender tag];
    if ([sender isKindOfClass:[NSButton class]] && page==0 && [sender title].length==0) page=0;
    show_page((int)page);
}
- (void)saveSettings:(id)sender {(void)sender;save_settings();}
- (void)resetSettings:(id)sender {(void)sender;reset_settings();}
- (void)reconnect:(id)sender {(void)sender;net_request_reconnect();}
- (void)switchMac:(id)sender {(void)sender;disp_wake(g_mac.cfg.disp_wake_sec);disp_set_input(g_mac.cfg.disp_mac_input);}
- (void)switchWin:(id)sender {(void)sender;disp_set_input(g_mac.cfg.disp_win_input);}
- (void)wakeDisplay:(id)sender {(void)sender;disp_wake(g_mac.cfg.disp_wake_sec);}
- (void)sleepDisplay:(id)sender {(void)sender;disp_sleep();}
- (void)cancelTransfer:(id)sender {(void)sender;xfer_cancel();}
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
    NSRect frame=NSMakeRect(0,0,1140,800);
    s_mainWindow=[[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    s_mainWindow.releasedWhenClosed=NO;
    s_mainWindow.title=@"HopDesk";
    s_mainWindow.minSize=NSMakeSize(1040,700);
    s_uiController=[[HopDeskMainController alloc] init];
    s_mainWindow.delegate=(id<NSWindowDelegate>)s_uiController;
    NSView *root=s_mainWindow.contentView;
    root.wantsLayer=YES;root.layer.backgroundColor=[NSColor windowBackgroundColor].CGColor;

    NSVisualEffectView *sidebar=[[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0,0,210,800)];
    sidebar.material=NSVisualEffectMaterialSidebar;sidebar.blendingMode=NSVisualEffectBlendingModeBehindWindow;
    sidebar.state=NSVisualEffectStateFollowsWindow;sidebar.autoresizingMask=NSViewHeightSizable;
    [root addSubview:sidebar];
    NSTextField *brand=ui_label(sidebar,@"HopDesk",NSMakeRect(24,740,160,32),22,YES);
    ui_label(sidebar,@"跨设备协作",NSMakeRect(26,714,150,20),11,NO);
    for(int i=0;i<6;++i){
        NSButton *b=[NSButton buttonWithTitle:kPageNames[i] target:s_uiController action:@selector(selectPage:)];
        b.frame=NSMakeRect(18,620-i*52,174,38);b.bezelStyle=NSBezelStyleTexturedRounded;
        b.tag=i;b.alignment=NSTextAlignmentLeft;b.font=[NSFont systemFontOfSize:13];
        [sidebar addSubview:b];s_nav[i]=b;
    }
    s_pageTitle=ui_label(root,@"概览",NSMakeRect(246,736,360,34),23,YES);
    s_headerStatus=ui_label(root,@"等待连接",NSMakeRect(850,742,250,22),12,YES);
    s_footer=ui_label(root,@"HopDesk",NSMakeRect(236,6,860,18),10,NO);
    build_pages(root);
    [root addSubview:brand];
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
void statusbar_show_transfers(void) { main_ui_show_page(3); }
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
