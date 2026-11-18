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
#include "../core/xfer_task_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CFG_NAME, CFG_HOST, CFG_PORT, CFG_DATA_PORT, CFG_TOKEN,
    CFG_SERVER_EDGE, CFG_CLIENT_EDGE, CFG_SWITCH_DELAY, CFG_COOLDOWN,
    CFG_MARGIN, CFG_DISP_WIN, CFG_DISP_MAC, CFG_WAKE, CFG_CHUNK,
    CFG_MAC_INPUT, CFG_MAC_SLEEP, CFG_ROLE, CFG_COUNT
};

enum { UI_PAGE_OVERVIEW, UI_PAGE_DEVICE, UI_PAGE_TRANSFER, UI_PAGE_SETTINGS, UI_PAGE_COUNT };

static NSWindow *s_mainWindow;
static NSView *s_pages[UI_PAGE_COUNT];
static NSScrollView *s_hosts[UI_PAGE_COUNT];static int s_hostCount;
static NSPopUpButton *s_rolePopup;
static hop_config s_draft;
static NSString *s_addresses;
static BOOL s_saveBusy,s_logBusy,s_settingsDirty,s_populatingSettings;
static NSButton *s_nav[UI_PAGE_COUNT];
static NSTextField *s_pageTitle, *s_headerStatus, *s_footer;
static NSTextField *s_overviewStatus, *s_overviewPeer, *s_overviewControl, *s_overviewData, *s_overviewSharing;
static NSTextField *s_connectionRole, *s_connectionStatus, *s_connectionPeer, *s_connectionPorts;
static NSTextField *s_cfg[CFG_COUNT], *s_cfgNote, *s_hostLabel, *s_hostHint;
static NSButton *s_saveSettingsButton, *s_resetSettingsButton;
static NSPopUpButton *s_edgePopups[2];
static NSTextField *s_xferTitle, *s_xferDetail, *s_xferCounts;
static NSProgressIndicator *s_xferProgress;
static NSTextView *s_xferFiles, *s_logView, *s_taskHistory;
static NSTextField *s_logNote;
static BOOL s_fileSelectionValid;
static NSPopUpButton *s_historyDirection, *s_historyState, *s_historyOrder;
static NSButton *s_retryButton, *s_cancelTaskButtons[2];
static BOOL s_refreshingTransfers, s_syncTaskSelection, s_fileBatchActive;
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
        BOOL canPost = CGPreflightPostEventAccess();
        BOOL canListen = CGPreflightListenEventAccess();
        BOOL canAccess = AXIsProcessTrusted();
        if (canPost && canListen && canAccess) {
            hop_log(HOP_LOG_INFO, "permission: macOS event access is already granted");
            return;
        }

        if (s_accessibilityPermissionRequestStarted) return;
        s_accessibilityPermissionRequestStarted = YES;

        /* AX registers this exact bundle in the Accessibility list. CoreGraphics
         * PostEvent alone can leave the user staring at an empty list. */
        if (!canAccess) {
            NSDictionary *prompt = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
            canAccess = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)prompt);
        }
        if (!canPost) canPost = CGRequestPostEventAccess();
        if (!canListen) canListen = CGRequestListenEventAccess();
        if (canPost && canListen && canAccess) {
            hop_log(HOP_LOG_INFO, "permission: macOS event access granted");
            return;
        }

        NSString *pane = (!canAccess || !canPost) ? @"Privacy_Accessibility" : @"Privacy_ListenEvent";
        NSString *base = @"x-apple.systempreferences:com.apple.preference.security?";
        if (@available(macOS 13.0, *))
            base = @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?";
        NSString *url = [base stringByAppendingString:pane];
        NSURL *settingsURL = [NSURL URLWithString:url];

        BOOL opened = settingsURL && [NSWorkspace.sharedWorkspace openURL:settingsURL];
        if (!opened) {
            NSURL *legacyURL = [NSURL URLWithString:
                @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
            opened = legacyURL && [NSWorkspace.sharedWorkspace openURL:legacyURL];
        }

        if (opened) {
            hop_log(HOP_LOG_WARN,
                    "permission: opened privacy settings for %s; enable HopDesk at %s, then relaunch it",
                    (!canAccess || !canPost) ? "Accessibility" : "Input Monitoring",
                    NSBundle.mainBundle.bundlePath.UTF8String);
        } else {
            hop_log(HOP_LOG_ERROR,
                    "permission: unable to open the Accessibility settings pane; open System Settings > Privacy & Security > Accessibility manually");
        }
    });
}

static NSString *const kPageNames[UI_PAGE_COUNT] = {@"概览",@"设备",@"传输",@"设置"};

@interface HopDeskMainController : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextViewDelegate>
- (void)selectPage:(id)sender;
- (void)saveSettings:(id)sender;
- (void)roleChanged:(id)sender;
- (void)settingsChanged:(id)sender;
- (void)settingsTextDidChange:(NSNotification *)notification;
- (void)resetSettings:(id)sender;
- (void)reconnect:(id)sender;
- (void)switchMac:(id)sender;
- (void)switchWin:(id)sender;
- (void)wakeDisplay:(id)sender;
- (void)sleepDisplay:(id)sender;
- (void)cancelTask:(id)sender;
- (void)tableViewSelectionDidChange:(NSNotification *)notification;
- (void)textViewDidChangeSelection:(NSNotification *)notification;
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
enum { UI_BUTTON_SECONDARY, UI_BUTTON_PRIMARY, UI_BUTTON_DANGER };
static NSButton *ui_button_styled(NSView *parent, NSString *title, NSRect frame,
                                  id target, SEL action, int style) {
    NSButton *b=[NSButton buttonWithTitle:title target:target action:action];
    b.frame=frame;b.bezelStyle=NSBezelStyleRounded;
    b.font=[NSFont systemFontOfSize:13 weight:style==UI_BUTTON_PRIMARY?NSFontWeightSemibold:NSFontWeightMedium];
    if (style==UI_BUTTON_PRIMARY) {
        b.bezelColor=[NSColor systemBlueColor];b.contentTintColor=[NSColor whiteColor];
    } else if (style==UI_BUTTON_DANGER) {
        b.bezelColor=[NSColor colorWithSRGBRed:1.0 green:0.93 blue:0.92 alpha:1.0];
        b.contentTintColor=[NSColor systemRedColor];
    } else {
        b.bezelColor=[NSColor controlColor];b.contentTintColor=[NSColor labelColor];
    }
    [parent addSubview:b];return b;
}
static NSButton *ui_button(NSView *parent, NSString *title, NSRect frame, id target, SEL action) {
    return ui_button_styled(parent,title,frame,target,action,UI_BUTTON_SECONDARY);
}
static NSButton *ui_primary_button(NSView *parent, NSString *title, NSRect frame, id target, SEL action) {
    return ui_button_styled(parent,title,frame,target,action,UI_BUTTON_PRIMARY);
}
static NSButton *ui_danger_button(NSView *parent, NSString *title, NSRect frame, id target, SEL action) {
    return ui_button_styled(parent,title,frame,target,action,UI_BUTTON_DANGER);
}
static void ui_apply_status_badge(NSTextField *field, BOOL connected, BOOL sharing) {
    if (!field) return;
    NSColor *foreground=sharing?[NSColor colorWithSRGBRed:0.12 green:0.38 blue:0.72 alpha:1.0]:
        connected?[NSColor colorWithSRGBRed:0.10 green:0.48 blue:0.30 alpha:1.0]:[NSColor secondaryLabelColor];
    NSColor *background=sharing?[NSColor colorWithSRGBRed:0.89 green:0.94 blue:1.0 alpha:1.0]:
        connected?[NSColor colorWithSRGBRed:0.90 green:0.96 blue:0.92 alpha:1.0]:
        [NSColor colorWithSRGBRed:0.94 green:0.95 blue:0.97 alpha:1.0];
    field.wantsLayer=YES;field.layer.cornerRadius=9.0;field.layer.masksToBounds=YES;
    field.layer.backgroundColor=background.CGColor;field.textColor=foreground;
    field.alignment=NSTextAlignmentCenter;
    field.font=[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
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
static void set_settings_note(NSString *message, BOOL error) {
    if(!s_cfgNote)return;
    s_cfgNote.stringValue=message ?: @"";
    s_cfgNote.textColor=error?[NSColor systemRedColor]:[NSColor secondaryLabelColor];
}
static void update_settings_actions(void) {
    if(s_saveSettingsButton)s_saveSettingsButton.enabled=s_settingsDirty&&!s_saveBusy;
    if(s_resetSettingsButton)s_resetSettingsButton.enabled=!s_saveBusy;
}
static void mark_settings_dirty(void) {
    if(s_populatingSettings||s_saveBusy)return;
    s_settingsDirty=YES;
    set_settings_note(@"有未保存的更改。网络与认证设置保存后需要重启生效。",NO);
    update_settings_actions();
}
static void settings_validation_error(int field, NSString *message) {
    s_settingsDirty=YES;
    set_settings_note(message,YES);
    update_settings_actions();
    if(field>=0 && field<CFG_COUNT && s_cfg[field])
        [s_mainWindow makeFirstResponder:s_cfg[field]];
}
static void update_settings_role_fields(void) {
    BOOL client=s_rolePopup.indexOfSelectedItem==1;
    s_cfg[CFG_HOST].enabled=client;
    s_cfg[CFG_HOST].placeholderString=client?@"例如 192.168.1.20":@"服务器模式无需填写";
    s_hostLabel.stringValue=@"服务器地址";
    s_hostHint.stringValue=client?
        @"客户端模式：填写服务器 IP 地址或主机名。":
        @"服务器模式：无需填写地址，等待客户端连接。";
}
static void set_settings_editors_enabled(BOOL enabled) {
    for(int i=0;i<CFG_COUNT;++i)if(s_cfg[i])s_cfg[i].enabled=enabled;
    if(s_rolePopup)s_rolePopup.enabled=enabled;
    for(int i=0;i<2;++i)if(s_edgePopups[i])s_edgePopups[i].enabled=enabled;
    if(enabled)update_settings_role_fields();
}
static void populate_settings(void) {
    hop_config *c=&s_draft;
    s_populatingSettings=YES;
    [s_rolePopup selectItemAtIndex:c->role==HOP_ROLE_CLIENT?1:0];
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
    update_settings_role_fields();
    s_populatingSettings=NO;
    s_settingsDirty=NO;
    set_settings_note(@"设置已加载。修改后保存；网络与认证参数将在重启后生效。",NO);
    update_settings_actions();
}
static void save_settings(void) {
    hop_config next=s_draft;
    if(s_saveBusy||!s_settingsDirty)return;
    next.role=s_rolePopup.indexOfSelectedItem==1?HOP_ROLE_CLIENT:HOP_ROLE_SERVER;
    const char *name=s_cfg[CFG_NAME].stringValue.UTF8String ?: "";
    const char *host=s_cfg[CFG_HOST].stringValue.UTF8String ?: "";
    const char *token=s_cfg[CFG_TOKEN].stringValue.UTF8String ?: "";
    const char *input=s_cfg[CFG_MAC_INPUT].stringValue.UTF8String ?: "";
    const char *sleep=s_cfg[CFG_MAC_SLEEP].stringValue.UTF8String ?: "";
    if(!name[0]||strlen(name)>=sizeof(next.device_name)){
        settings_validation_error(CFG_NAME,@"设备名称不能为空，且长度不能超过 31 个字节。");return;
    }
    if(strlen(host)>=sizeof(next.peer_host)||(next.role==HOP_ROLE_CLIENT&&!host[0])){
        settings_validation_error(CFG_HOST,@"客户端模式需要填写服务器 IP 地址或主机名。");return;
    }
    if(!token[0]||strlen(token)>=sizeof(next.token)){
        settings_validation_error(CFG_TOKEN,@"共享 Token 不能为空，且不能超过 128 个字节；两台设备必须使用相同 Token。");return;
    }
    if(strlen(input)>=sizeof(next.mac_input_cmd)){
        settings_validation_error(CFG_MAC_INPUT,@"Mac 切源命令过长。");return;
    }
    if(strlen(sleep)>=sizeof(next.mac_sleep_cmd)){
        settings_validation_error(CFG_MAC_SLEEP,@"Mac 睡眠命令过长。");return;
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
    if(!port||port>65535){
        settings_validation_error(CFG_PORT,@"控制端口必须在 1–65535 之间。");return;
    }
    if(!dataPort||dataPort>65535||dataPort==port){
        settings_validation_error(CFG_DATA_PORT,@"数据端口必须在 1–65535 之间，且不能与控制端口相同。");return;
    }
    if(delay>5000){settings_validation_error(CFG_SWITCH_DELAY,@"边缘触发延迟不能超过 5000 ms。");return;}
    if(cooldown>10000){settings_validation_error(CFG_COOLDOWN,@"切换冷却不能超过 10000 ms。");return;}
    if(margin>1000){settings_validation_error(CFG_MARGIN,@"边缘内缩不能超过 1000 px。");return;}
    if(winInput>65535){settings_validation_error(CFG_DISP_WIN,@"Windows 输入源必须是 0–65535 的十进制或十六进制数值。");return;}
    if(macInput>65535){settings_validation_error(CFG_DISP_MAC,@"Mac 输入源必须是 0–65535 的十进制或十六进制数值。");return;}
    if(wake>3600){settings_validation_error(CFG_WAKE,@"唤醒时长不能超过 3600 秒。");return;}
    if(chunk<64||chunk>4096){settings_validation_error(CFG_CHUNK,@"传输分块必须在 64–4096 KB 之间。");return;}
    next.port=(uint16_t)port;next.data_port=(uint16_t)dataPort;
    next.server_edge=(int)s_edgePopups[0].indexOfSelectedItem;
    next.client_edge=(int)s_edgePopups[1].indexOfSelectedItem;
    next.switch_delay_ms=(int)delay;next.switch_cooldown_ms=(int)cooldown;
    next.edge_margin_px=(int)margin;next.disp_win_input=(uint16_t)winInput;
    next.disp_mac_input=(uint16_t)macInput;next.disp_wake_sec=(int)wake;
    next.xfer_chunk_kb=(int)chunk;
    snprintf(next.mac_input_cmd,sizeof(next.mac_input_cmd),"%s",input);
    snprintf(next.mac_sleep_cmd,sizeof(next.mac_sleep_cmd),"%s",sleep);
    s_saveBusy=YES;
    set_settings_editors_enabled(NO);
    set_settings_note(@"正在保存设置…",NO);
    update_settings_actions();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        int ok=hop_config_save_atomic(&next,g_mac.config_path);
        dispatch_async(dispatch_get_main_queue(), ^{
            s_saveBusy=NO;
            set_settings_editors_enabled(YES);
            if(ok){
                s_draft=next;s_settingsDirty=NO;
                set_settings_note(@"设置已保存。网络与认证参数将在重启 HopDesk 后生效。",NO);
            } else {
                s_settingsDirty=YES;
                set_settings_note(@"保存失败；原配置未改变。请检查文件权限和磁盘空间后重试。",YES);
            }
            update_settings_actions();
        });
    });
}
static void reset_settings(void) {
    if(s_saveBusy)return;
    hop_config_defaults(&s_draft);
    snprintf(s_draft.device_name,sizeof(s_draft.device_name),"%s","Mac");
    populate_settings();
    s_settingsDirty=YES;
    set_settings_note(@"已恢复默认值，但尚未写入配置文件。确认后点击“保存设置”。",NO);
    update_settings_actions();
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
    hop_xfer_status_pair pair;memset(&pair,0,sizeof(pair));xfer_get_status_pair(&pair);
    NSMutableArray<NSDictionary *> *rows[2]={[NSMutableArray array],[NSMutableArray array]};
    for(int i=0;i<n;++i){
        hop_xfer_task_snapshot *t=&s_tasksSnapshot[i];
        int d=t->direction==HOP_XFER_DIR_SEND?0:t->direction==HOP_XFER_DIR_RECEIVE?1:-1;
        if(d<0)continue;
        if(t->state>=HOP_XFER_TASK_COMPLETED || t->kind==HOP_TASK_KIND_IMAGE_WORK)continue;
        hop_xfer_status current=d==0?pair.send:pair.receive;
        BOOL currentBatch=t->transfer_id && current.batch==t->transfer_id;
        NSString *name=utf8(t->name);
        if(!name.length && currentBatch && current.name[0])name=utf8(current.name);
        if(!name.length)name=[NSString stringWithFormat:@"任务 #%llu",(unsigned long long)t->id];
        uint32_t done=currentBatch?current.files_done:t->files_done;
        uint32_t count=currentBatch?current.files_total:t->files_total;
        uint64_t bytesDone=currentBatch?current.bytes_done:t->bytes_done;
        uint64_t bytesTotal=currentBatch?current.bytes_total:t->bytes_total;
        double progress=bytesTotal?MIN(100.0,100.0*(double)bytesDone/(double)bytesTotal):0.0;
        NSString *progressText=currentBatch||bytesTotal?
            [NSString stringWithFormat:@"%.0f%%  ·  文件 %u/%u  ·  %@/%@",progress,done,count,byte_count(bytesDone),byte_count(bytesTotal)]:
            (t->state==HOP_XFER_TASK_SCANNING?@"正在准备":@"等待开始");
        NSString *text=[NSString stringWithFormat:@"%@  ·  %@  ·  %@",
            name,task_state_name(t),progressText];
        [rows[d] addObject:@{@"id":@(t->id),@"text":text}];
    }
    for(int d=0;d<2;++d)if(rows[d].count==0)
        [rows[d] addObject:@{@"id":@0,@"text":d?@"暂无活动的接收任务":@"暂无活动的发送任务"}];
    NSPoint scrollOrigins[2]={NSZeroPoint,NSZeroPoint};
    for(int d=0;d<2;++d){
        NSScrollView *scroll=s_taskTables[d].enclosingScrollView;
        if(scroll)scrollOrigins[d]=scroll.contentView.bounds.origin;
        s_taskRows[d]=[rows[d] copy];[s_taskTables[d] reloadData];[s_taskTables[d] deselectAll:nil];
        if(selected[d])for(NSUInteger i=0;i<s_taskRows[d].count;++i)
            if([s_taskRows[d][i][@"id"] unsignedLongLongValue]==selected[d]){
                [s_taskTables[d] selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];break;
            }
        if(scroll){
            NSClipView *clip=scroll.contentView;[clip scrollToPoint:scrollOrigins[d]];
            [scroll reflectScrolledClipView:clip];
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
    hop_xfer_task_history_record history[HOP_TASK_HISTORY_CAPACITY];
    int total=0,n=hop_xfer_task_history_store_list(history,HOP_TASK_HISTORY_CAPACITY,directions[d],states[st],order==0,&total);
    if(n<0)return;
    NSMutableString *rows=[NSMutableString string];
    for(int i=0;i<n;++i){const hop_xfer_task_history_record *t=&history[i];
        hop_xfer_task_snapshot display={0};display.state=(hop_xfer_task_state)t->state;
        NSString *name=utf8(t->name);
        if(!name.length)name=[NSString stringWithFormat:@"任务 #%llu",(unsigned long long)t->record_id];
        [rows appendFormat:@"%@  ·  %@  ·  %@  ·  文件 %u/%u  ·  %@/%@\n",
         t->direction==HOP_XFER_DIR_SEND?@"发送":@"接收",name,task_state_name(&display),
         t->files_done,t->files_total,byte_count(t->bytes_done),byte_count(t->bytes_total)];
    }
    NSString *next=rows.length?rows:@"暂无符合条件的任务";
    if(![s_taskHistory.string isEqualToString:next]){
        NSScrollView *scroll=s_taskHistory.enclosingScrollView;
        NSPoint origin=scroll?scroll.contentView.bounds.origin:NSZeroPoint;
        s_taskHistory.string=next;
        if(scroll){NSClipView *clip=scroll.contentView;[clip scrollToPoint:origin];[scroll reflectScrolledClipView:clip];}
    }
    (void)total;
}
static int selected_visible_file_index(void) {
    if(!s_fileSelectionValid || !s_xferFiles || !s_fileSnapshot)return -1;
    NSString *text=s_xferFiles.string ?: @"";
    NSUInteger location=MIN(s_xferFiles.selectedRange.location,text.length),line=0;
    for(NSUInteger i=0;i<location;++i)if([text characterAtIndex:i]=='\n')line++;
    if(line>=(NSUInteger)s_visibleFileCount)return -1;
    return s_visibleFileIndices[line];
}
static void update_transfer_actions(void) {
    int index=selected_visible_file_index();
    BOOL canRetry=s_fileDirection==HOP_XFER_DIR_SEND && !s_fileBatchActive &&
        !(s_fileBatch & HOP_XFER_PASTE_BATCH_FLAG) &&
        index>=0 && index<HOP_XFER_FILE_LIMIT &&
        s_fileSnapshot[index].phase==HOP_XFER_FILE_FAILED && !s_fileSnapshot[index].is_directory;
    if(s_retryButton)s_retryButton.enabled=canRetry;
    for(int d=0;d<2;++d){
        BOOL canCancel=NO;
        hop_xfer_task_snapshot task;
        hop_xfer_task_id id=selected_task(d);
        if(id && hop_xfer_task_get_snapshot(id,&task))
            canCancel=task.state<HOP_XFER_TASK_COMPLETED && !task.cancel_requested && !task.completing;
        if(s_cancelTaskButtons[d])s_cancelTaskButtons[d].enabled=canCancel;
    }
}
static void refresh_transfer_page(void) {
    if(!s_xferTitle || s_refreshingTransfers)return;
    int oldSelectedFileIndex=selected_visible_file_index();
    uint32_t oldFileBatch=s_fileBatch;int oldFileDirection=s_fileDirection;
    NSScrollView *fileScroll=s_xferFiles.enclosingScrollView;
    NSPoint oldFileScrollOrigin=fileScroll?fileScroll.contentView.bounds.origin:NSZeroPoint;
    s_refreshingTransfers=YES;
    refresh_task_lists();
    refresh_task_history();
    hop_xfer_status_pair pair; memset(&pair,0,sizeof(pair)); xfer_get_status_pair(&pair);

    hop_xfer_task_snapshot selectedSnapshot;memset(&selectedSnapshot,0,sizeof(selectedSnapshot));
    hop_xfer_task_id selectedId=selected_task(0);
    if(!selectedId)selectedId=selected_task(1);
    BOOL hasSelected=selectedId && hop_xfer_task_get_snapshot(selectedId,&selectedSnapshot);
    BOOL hasTaskStatus=NO;
    hop_xfer_status st=pair.primary_direction==HOP_XFER_DIR_SEND?pair.send:pair.receive;
    if(hasSelected){
        hop_xfer_status candidate=selectedSnapshot.direction==HOP_XFER_DIR_SEND?pair.send:pair.receive;
        if(selectedSnapshot.transfer_id && candidate.batch==selectedSnapshot.transfer_id){
            st=candidate;hasTaskStatus=YES;
        }else{
            memset(&st,0,sizeof(st));st.direction=selectedSnapshot.direction;
        }
    }

    NSString *name=hasSelected?utf8(selectedSnapshot.name):utf8(st.name);
    if(!name.length && hasTaskStatus && st.name[0])name=utf8(st.name);
    if(!name.length)name=hasSelected?
        [NSString stringWithFormat:@"%@任务 #%llu",selectedSnapshot.direction==HOP_XFER_DIR_SEND?@"发送":@"接收",(unsigned long long)selectedId]:
        (st.name[0]?utf8(st.name):@"暂无传输任务");

    NSString *phase=@"空闲";
    switch(st.phase){
        case HOP_XFER_QUEUED:phase=@"排队中";break; case HOP_XFER_SCANNING:phase=@"扫描中";break;
        case HOP_XFER_SENDING:phase=@"发送中";break; case HOP_XFER_RECEIVING:phase=@"接收中";break;
        case HOP_XFER_VERIFYING:phase=@"校验中";break; case HOP_XFER_COMPLETED:phase=@"已完成";break;
        case HOP_XFER_PARTIAL:phase=@"部分失败";break; case HOP_XFER_FAILED:phase=@"失败";break;
        case HOP_XFER_CANCELLED:phase=@"已取消";break;
    }
    if(hasSelected && !hasTaskStatus)phase=task_state_name(&selectedSnapshot);
    NSString *direction=hasSelected?
        (selectedSnapshot.direction==HOP_XFER_DIR_SEND?@"发送":@"接收"):
        (st.direction==HOP_XFER_DIR_SEND?@"发送":st.direction==HOP_XFER_DIR_RECEIVE?@"接收":@"待机");
    uint64_t now_ms=hop_monotonic_ms(),speed=hasTaskStatus?st.speed_bytes_per_sec:0;
    if(!st.last_progress_ms || now_ms<st.last_progress_ms || now_ms-st.last_progress_ms>2000)speed=0;
    NSString *rate=speed?[NSString stringWithFormat:@"%@/s",byte_count(speed)]:@"速率 --";
    NSString *eta=speed?[NSString stringWithFormat:@"ETA %llus",(unsigned long long)st.eta_seconds]:@"ETA --";
    NSString *detail=hasTaskStatus?utf8(st.detail):(hasSelected?utf8(selectedSnapshot.detail):@"等待文件传输");
    if(!detail.length)detail=hasSelected&&!hasTaskStatus?@"实时进度将在任务开始后显示":@"";
    s_xferTitle.stringValue=name;
    s_xferDetail.stringValue=[NSString stringWithFormat:@"%@  ·  %@  ·  %@  ·  %@  ·  %@",
        direction,phase,detail,rate,eta];

    double progress=0;
    uint32_t filesDone=0,filesTotal=0,queued=0;
    uint64_t bytesDone=0,bytesTotal=0;
    if(hasTaskStatus){
        bytesDone=st.bytes_done;bytesTotal=st.bytes_total;
        filesDone=st.files_done;filesTotal=st.files_total;queued=st.queued_count;
    }else if(hasSelected){
        bytesDone=selectedSnapshot.bytes_done;bytesTotal=selectedSnapshot.bytes_total;
        filesDone=selectedSnapshot.files_done;filesTotal=selectedSnapshot.files_total;
    }else{
        bytesDone=st.bytes_done;bytesTotal=st.bytes_total;
        filesDone=st.files_done;filesTotal=st.files_total;queued=st.queued_count;
    }
    progress=bytesTotal?MIN(100.0,100.0*(double)bytesDone/(double)bytesTotal):
        (hasTaskStatus&&st.phase==HOP_XFER_COMPLETED?100.0:0.0);
    if(hasSelected && !hasTaskStatus && !bytesTotal && !filesTotal)
        s_xferCounts.stringValue=@"任务正在准备或排队，开始后显示实时进度";
    else{
        s_xferCounts.stringValue=[NSString stringWithFormat:@"%.0f%%  ·  文件 %u / %u    排队 %u    数据 %@ / %@",
            progress,filesDone,filesTotal,queued,byte_count(bytesDone),byte_count(bytesTotal)];
        if(!hasSelected && pair.send.active && pair.receive.active)
            s_xferCounts.stringValue=[NSString stringWithFormat:@"双向传输  ·  发送 %@/%@  ·  接收 %@/%@  ·  排队 %u",
                byte_count(pair.send.bytes_done),byte_count(pair.send.bytes_total),
                byte_count(pair.receive.bytes_done),byte_count(pair.receive.bytes_total),pair.send.queued_count];
    }
    s_xferProgress.doubleValue=progress;

    int fileDirection=hasSelected?selectedSnapshot.direction:st.direction;
    uint32_t fileBatch=hasSelected?selectedSnapshot.transfer_id:st.batch;
    s_fileBatchActive=hasSelected?(hasTaskStatus && st.active):st.active;
    if(!s_fileSnapshot)s_fileSnapshot=calloc(HOP_XFER_FILE_LIMIT,sizeof(*s_fileSnapshot));
    int count=s_fileSnapshot&&fileDirection!=HOP_XFER_DIR_NONE&&fileBatch?
        xfer_get_files(fileDirection,fileBatch,s_fileSnapshot,HOP_XFER_FILE_LIMIT):0;
    if(s_fileBatch!=fileBatch || s_fileDirection!=fileDirection){
        memset(s_collapsedFolders,0,sizeof(s_collapsedFolders));
        s_fileBatch=fileBatch;s_fileDirection=fileDirection;
    }
    NSMutableString *files=[NSMutableString string];s_visibleFileCount=0;
    for(int i=0;i<count;++i){
        NSString *path=utf8(s_fileSnapshot[i].relative_path);BOOL hidden=NO;
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
    NSString *nextFiles=files.length?files:
        (hasSelected?(selectedSnapshot.state<=HOP_XFER_TASK_QUEUED?@"任务正在准备或排队，开始后显示文件明细":@"此任务的实时文件明细暂不可用"):@"当前没有文件明细");
    BOOL sameFileContext=oldFileBatch==fileBatch&&oldFileDirection==fileDirection;
    if(![s_xferFiles.string isEqualToString:nextFiles]){
        s_xferFiles.string=nextFiles;
        s_fileSelectionValid=NO;
        if(sameFileContext&&oldSelectedFileIndex>=0){
            for(int line=0;line<s_visibleFileCount;++line)if(s_visibleFileIndices[line]==oldSelectedFileIndex){
                NSUInteger start=0;
                for(int row=0;row<line;++row){
                    NSRange newline=[nextFiles rangeOfString:@"\n" options:0
                        range:NSMakeRange(start,nextFiles.length-start)];
                    if(newline.location==NSNotFound){start=nextFiles.length;break;}
                    start=NSMaxRange(newline);
                }
                NSRange newline=[nextFiles rangeOfString:@"\n" options:0
                    range:NSMakeRange(start,nextFiles.length-start)];
                NSUInteger end=newline.location==NSNotFound?nextFiles.length:newline.location;
                if(end>start){[s_xferFiles setSelectedRange:NSMakeRange(start,end-start)];s_fileSelectionValid=YES;}
                break;
            }
        }
    }else if(!sameFileContext){
        [s_xferFiles setSelectedRange:NSMakeRange(0,0)];s_fileSelectionValid=NO;
    }
    if(fileScroll){
        NSClipView *clip=fileScroll.contentView;
        if(sameFileContext)[clip scrollToPoint:oldFileScrollOrigin];
        else [s_xferFiles scrollRangeToVisible:NSMakeRange(0,0)];
        [fileScroll reflectScrolledClipView:clip];
    }
    update_transfer_actions();
    s_refreshingTransfers=NO;
}
static BOOL log_result_is_error(NSString *text) {
    return !text || [text hasPrefix:@"读取"] || [text hasPrefix:@"无法"] ||
        [text hasPrefix:@"日志路径"] || [text hasPrefix:@"日志编码"] ||
        [text hasPrefix:@"内存不足"];
}
static void set_log_note(NSString *message, BOOL error) {
    if(!s_logNote)return;
    s_logNote.stringValue=message ?: @"";
    s_logNote.textColor=error?[NSColor systemRedColor]:[NSColor secondaryLabelColor];
}
static void refresh_logs(void) {
    if (!s_logView) return;
    if(s_logBusy)return;
    s_logBusy=YES;set_log_note(@"正在读取日志…",NO);
    NSString *path=utf8(g_mac.log_path);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        @autoreleasepool {
            NSString *text=@"暂无日志记录。";
            @try {
                if(!path.length)text=@"日志路径尚未初始化。";
                else {
                    NSFileHandle *file=[NSFileHandle fileHandleForReadingAtPath:path];
                    if(!file){
                        text=[[NSFileManager defaultManager] fileExistsAtPath:path]?
                            @"读取日志失败：无法打开日志文件。":@"暂无日志记录。";
                    } else {
                        @try {
                            unsigned long long size=[file seekToEndOfFile];
                            if(size>0){
                                [file seekToFileOffset:size>262144?size-262144:0];
                                NSData *data=[file readDataToEndOfFile];
                                /* A tail may begin in the middle of a UTF-8 sequence. */
                                if(size>262144){const unsigned char *p=data.bytes;NSUInteger i=0;
                                    while(i<data.length&&p[i]!='\n')++i;
                                    if(i<data.length)data=[data subdataWithRange:NSMakeRange(i+1,data.length-i-1)];}
                                if(data.length)
                                    text=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"日志编码无法识别。";
                            }
                        } @finally {[file closeFile];}
                    }
                }
            } @catch(NSException *exception){text=[NSString stringWithFormat:@"读取日志失败：%@",exception.reason ?: @"未知错误"];}
            dispatch_async(dispatch_get_main_queue(), ^{
                s_logBusy=NO;s_logView.string=text;
                if([text isEqualToString:@"暂无日志记录。"])set_log_note(@"暂无日志记录",NO);
                else if(log_result_is_error(text))set_log_note(@"读取失败，请重试",YES);
                else set_log_note(@"日志已更新",NO);
            });
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
    s_overviewControl.stringValue=s_connected?@"已连接":@"等待连接";
    s_overviewData.stringValue=xfer_connected()?@"已连接":@"等待连接";
    s_overviewSharing.stringValue=s_sharing?
        (g_mac.cfg.role==HOP_ROLE_SERVER?@"已移交客户端":@"正在接收输入"):
        s_connected?@"已就绪 · 等待边缘移交":@"等待连接";
    s_connectionRole.stringValue=g_mac.cfg.role==HOP_ROLE_SERVER?@"本机角色：服务器 · 提供键鼠":@"本机角色：客户端 · 接收键鼠";
    s_connectionStatus.stringValue=status;
    s_connectionPeer.stringValue=g_mac.cfg.role==HOP_ROLE_SERVER?[NSString stringWithFormat:@"本机 IP：%@",s_addresses]:[NSString stringWithFormat:@"服务器 IP：%@",utf8(g_mac.cfg.peer_host)];
    s_connectionPorts.stringValue=[NSString stringWithFormat:@"控制端口：%u    数据端口：%u",
        (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port];
    s_footer.stringValue=[NSString stringWithFormat:@"HopDesk  v%s    ·    %@",HOP_VERSION,status];
    ui_apply_status_badge(s_headerStatus,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewStatus,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewControl,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewData,xfer_connected(),NO);
    ui_apply_status_badge(s_overviewSharing,s_connected,s_sharing);
}
static void show_page(int page) {
    if(page<0||page>=UI_PAGE_COUNT)page=UI_PAGE_OVERVIEW;
    s_page=page;
    for(int i=0;i<UI_PAGE_COUNT;++i){
        s_hosts[i].hidden=i!=page;
        s_nav[i].layer.backgroundColor=(i==page?[NSColor colorWithSRGBRed:.90 green:.95 blue:.99 alpha:1]:[NSColor clearColor]).CGColor;
        s_nav[i].contentTintColor=i==page?[NSColor systemBlueColor]:[NSColor labelColor];
        s_nav[i].state=i==page?NSControlStateValueOn:NSControlStateValueOff;
    }
    s_pageTitle.stringValue=kPageNames[page];
    NSClipView *clip=s_hosts[page].contentView;[clip scrollToPoint:NSMakePoint(0,MAX(0,s_pages[page].frame.size.height-clip.bounds.size.height))];[s_hosts[page] reflectScrolledClipView:clip];
    if(page==UI_PAGE_TRANSFER)refresh_transfer_page();
    if(page==UI_PAGE_SETTINGS)refresh_logs();
}
static NSView *add_page(NSView *root) {
    NSView *v=ui_page(root);
    return v;
}
static void build_settings_page(NSView *p) {
    NSView *left=ui_card(p,NSMakeRect(20,180,400,470));
    NSView *right=ui_card(p,NSMakeRect(440,180,400,470));
    ui_label(left,@"设备与连接",NSMakeRect(18,424,360,24),15,YES);
    ui_label(left,@"设置本机身份、连接目标和两端共用的认证信息。",NSMakeRect(18,398,360,18),11,NO);
    const CGFloat y[]={360,320,280,240,200,160,120,80};
    const int ids[]={CFG_NAME,CFG_ROLE,CFG_HOST,CFG_PORT,CFG_DATA_PORT,CFG_TOKEN,CFG_SERVER_EDGE,CFG_CLIENT_EDGE};
    NSString *labels[]={@"设备名称",@"本机角色",@"服务器地址",@"控制端口",@"数据端口",@"共享 Token",@"服务端边缘",@"客户端边缘"};
    for(int i=0;i<8;++i){
        if(ids[i]==CFG_HOST){
            s_hostLabel=ui_label(left,labels[i],NSMakeRect(18,y[i]+7,112,22),12,NO);
            s_cfg[CFG_HOST]=ui_field(left,@"",NSMakeRect(136,y[i],242,30),NO);
            s_cfg[CFG_HOST].placeholderString=@"服务器模式无需填写";
            [[NSNotificationCenter defaultCenter] addObserver:s_uiController selector:@selector(settingsTextDidChange:)
                name:NSControlTextDidChangeNotification object:s_cfg[CFG_HOST]];
        } else {
            ui_label(left,labels[i],NSMakeRect(18,y[i]+7,112,22),12,NO);
            if(ids[i]==CFG_ROLE){
                s_rolePopup=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(136,y[i],242,30) pullsDown:NO];
                [s_rolePopup addItemsWithTitles:@[@"服务器 · 提供键鼠",@"客户端 · 接收键鼠"]];
                s_rolePopup.target=s_uiController;s_rolePopup.action=@selector(roleChanged:);
                [left addSubview:s_rolePopup];
            } else if(ids[i]==CFG_SERVER_EDGE || ids[i]==CFG_CLIENT_EDGE){
                int edge=ids[i]==CFG_SERVER_EDGE?0:1;
                NSPopUpButton *pop=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(136,y[i],242,30) pullsDown:NO];
                [pop addItemsWithTitles:@[@"左边缘",@"右边缘",@"上边缘",@"下边缘"]];
                pop.target=s_uiController;pop.action=@selector(settingsChanged:);
                [left addSubview:pop];s_edgePopups[edge]=pop;
            } else {
                s_cfg[ids[i]]=ui_field(left,@"",NSMakeRect(136,y[i],242,30),ids[i]==CFG_TOKEN);
                if(ids[i]==CFG_TOKEN)s_cfg[ids[i]].placeholderString=@"两台设备必须使用相同 Token";
                [[NSNotificationCenter defaultCenter] addObserver:s_uiController selector:@selector(settingsTextDidChange:)
                    name:NSControlTextDidChangeNotification object:s_cfg[ids[i]]];
            }
        }
    }
    s_hostHint=ui_label(left,@"服务器模式：无需填写地址，等待客户端连接。",NSMakeRect(18,22,360,34),11,NO);

    ui_label(right,@"交互与显示器",NSMakeRect(18,424,360,24),15,YES);
    ui_label(right,@"按需调整跨屏切换行为；输入源编号支持十六进制。",NSMakeRect(18,398,360,18),11,NO);
    const int settingIDs[]={CFG_SWITCH_DELAY,CFG_COOLDOWN,CFG_MARGIN,CFG_DISP_WIN,CFG_DISP_MAC,CFG_WAKE,CFG_CHUNK};
    NSString *names[]={@"边缘触发延迟（ms）",@"切换冷却（ms）",@"边缘内缩（px）",@"Windows 输入源",@"Mac 输入源",@"唤醒时长（秒）",@"传输分块（KB）"};
    const CGFloat ry[]={360,320,280,240,200,160,120};
    for(int i=0;i<7;++i){
        ui_label(right,names[i],NSMakeRect(18,ry[i]+7,170,22),12,NO);
        s_cfg[settingIDs[i]]=ui_field(right,@"",NSMakeRect(190,ry[i],190,30),NO);
        [[NSNotificationCenter defaultCenter] addObserver:s_uiController selector:@selector(settingsTextDidChange:)
            name:NSControlTextDidChangeNotification object:s_cfg[settingIDs[i]]];
    }
    ui_label(right,@"Mac 切源命令",NSMakeRect(18,96,160,20),12,NO);
    s_cfg[CFG_MAC_INPUT]=ui_field(right,@"",NSMakeRect(18,64,360,28),NO);
    [[NSNotificationCenter defaultCenter] addObserver:s_uiController selector:@selector(settingsTextDidChange:)
        name:NSControlTextDidChangeNotification object:s_cfg[CFG_MAC_INPUT]];
    ui_label(right,@"Mac 睡眠命令",NSMakeRect(18,36,160,20),12,NO);
    s_cfg[CFG_MAC_SLEEP]=ui_field(right,@"",NSMakeRect(18,6,360,28),NO);
    [[NSNotificationCenter defaultCenter] addObserver:s_uiController selector:@selector(settingsTextDidChange:)
        name:NSControlTextDidChangeNotification object:s_cfg[CFG_MAC_SLEEP]];

    NSView *actions=ui_card(p,NSMakeRect(20,22,840,104));
    ui_label(actions,@"应用设置",NSMakeRect(18,82,360,20),13,YES);
    ui_label(actions,@"保存会写入配置文件；网络监听和认证参数需要重启后生效。",NSMakeRect(18,56,560,18),11,NO);
    s_cfgNote=ui_label(actions,@"设置已加载。修改后保存。",NSMakeRect(18,28,560,18),11,NO);
    s_saveSettingsButton=ui_primary_button(actions,@"保存设置",NSMakeRect(610,38,112,34),s_uiController,@selector(saveSettings:));
    s_resetSettingsButton=ui_button(actions,@"恢复默认",NSMakeRect(738,38,92,34),s_uiController,@selector(resetSettings:));
}
static void build_pages(NSView *root) {
    for(int i=0;i<UI_PAGE_COUNT;++i)s_pages[i]=add_page(root);
    NSView *p=s_pages[UI_PAGE_OVERVIEW];
    NSView *card=ui_card(p,NSMakeRect(20,470,840,172));
    ui_label(card,@"连接概览",NSMakeRect(22,140,780,22),13,YES);
    s_overviewStatus=ui_label(card,@"等待连接",NSMakeRect(22,100,260,30),16,YES);
    s_overviewPeer=ui_label(card,@"设备与对端信息",NSMakeRect(22,64,600,22),12,NO);
    ui_primary_button(card,@"重新连接",NSMakeRect(680,88,130,36),s_uiController,@selector(reconnect:));

    card=ui_card(p,NSMakeRect(20,330,268,120));
    ui_label(card,@"控制通道",NSMakeRect(18,82,230,20),12,YES);
    s_overviewControl=ui_label(card,@"等待连接",NSMakeRect(18,42,230,26),14,YES);
    card=ui_card(p,NSMakeRect(306,330,268,120));
    ui_label(card,@"数据通道",NSMakeRect(18,82,230,20),12,YES);
    s_overviewData=ui_label(card,@"等待连接",NSMakeRect(18,42,230,26),14,YES);
    card=ui_card(p,NSMakeRect(592,330,268,120));
    ui_label(card,@"键鼠共享",NSMakeRect(18,82,230,20),12,YES);
    s_overviewSharing=ui_label(card,@"等待连接",NSMakeRect(18,42,230,26),13,YES);

    card=ui_card(p,NSMakeRect(20,142,840,164));
    ui_label(card,@"显示器控制",NSMakeRect(22,128,780,22),13,YES);
    ui_button(card,@"切换到 Mac 显示",NSMakeRect(22,54,148,36),s_uiController,@selector(switchMac:));
    ui_button(card,@"切回 Windows 显示",NSMakeRect(184,54,160,36),s_uiController,@selector(switchWin:));
    ui_button(card,@"唤醒显示",NSMakeRect(358,54,112,36),s_uiController,@selector(wakeDisplay:));
    ui_button(card,@"显示睡眠",NSMakeRect(484,54,112,36),s_uiController,@selector(sleepDisplay:));
    ui_primary_button(card,@"打开传输管理",NSMakeRect(610,54,170,36),s_uiController,@selector(selectPage:)).tag=UI_PAGE_TRANSFER;

    p=s_pages[UI_PAGE_DEVICE];
    card=ui_card(p,NSMakeRect(20,420,840,220));
    ui_label(card,@"连接状态",NSMakeRect(22,176,780,22),13,YES);
    s_connectionRole=ui_label(card,@"本机角色：macOS 客户端",NSMakeRect(22,138,780,22),12,NO);
    s_connectionStatus=ui_label(card,@"等待连接",NSMakeRect(22,104,780,22),14,YES);
    s_connectionPeer=ui_label(card,@"服务端地址：未配置",NSMakeRect(22,70,780,22),12,NO);
    s_connectionPorts=ui_label(card,@"控制端口：24817    数据端口：24818",NSMakeRect(22,38,780,22),12,NO);
    ui_primary_button(card,@"重新连接",NSMakeRect(22,0,130,32),s_uiController,@selector(reconnect:));
    ui_label(p,@"HopDesk 会自动重连。点击“重新连接”会关闭当前控制连接，并立即尝试重新建立连接。",
        NSMakeRect(22,360,810,42),11,NO);

    p=s_pages[UI_PAGE_TRANSFER];
    [p setFrameSize:NSMakeSize(880,960)];
    card=ui_card(p,NSMakeRect(20,760,840,180));
    ui_label(card,@"当前传输",NSMakeRect(22,146,780,20),13,YES);
    s_xferTitle=ui_label(card,@"暂无传输任务",NSMakeRect(22,112,780,26),17,YES);
    s_xferDetail=ui_label(card,@"等待文件传输",NSMakeRect(22,84,780,20),11,NO);
    s_xferCounts=ui_label(card,@"0%  ·  文件 0 / 0    排队 0    数据 0 B / 0 B",NSMakeRect(22,56,780,18),11,NO);
    s_xferProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(22,28,780,10)];
    s_xferProgress.minValue=0;s_xferProgress.maxValue=100;s_xferProgress.indeterminate=NO;
    [card addSubview:s_xferProgress];

    card=ui_card(p,NSMakeRect(20,530,840,210));
    ui_label(card,@"活动任务",NSMakeRect(22,180,780,20),13,YES);
    for(int d=0;d<2;++d){
        CGFloat x=d?430:22;
        ui_label(card,d?@"接收任务":@"发送任务",NSMakeRect(x,150,370,20),11,YES);
        NSScrollView *taskScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(x,48,370,96)];
        taskScroll.hasVerticalScroller=YES;taskScroll.borderType=NSBezelBorder;
        NSTableView *table=[[NSTableView alloc] initWithFrame:NSMakeRect(0,0,370,96)];
        NSTableColumn *column=[[NSTableColumn alloc] initWithIdentifier:@"task"];
        column.width=360;[table addTableColumn:column];table.headerView=nil;
        table.dataSource=(id<NSTableViewDataSource>)s_uiController;table.delegate=(id<NSTableViewDelegate>)s_uiController;table.allowsEmptySelection=YES;
        taskScroll.documentView=table;[card addSubview:taskScroll];s_taskTables[d]=table;
        s_cancelTaskButtons[d]=ui_danger_button(card,d?@"取消选中接收任务":@"取消选中发送任务",
            NSMakeRect(x,8,168,30),s_uiController,@selector(cancelTask:));
        s_cancelTaskButtons[d].tag=d;s_cancelTaskButtons[d].enabled=NO;
    }

    card=ui_card(p,NSMakeRect(20,270,840,240));
    ui_label(card,@"文件明细",NSMakeRect(22,204,780,22),13,YES);
    NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,48,804,142)];
    scroll.hasVerticalScroller=YES;scroll.hasHorizontalScroller=YES;
    s_xferFiles=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,136)];
    s_xferFiles.editable=NO;s_xferFiles.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    s_xferFiles.delegate=(id<NSTextViewDelegate>)s_uiController;
    scroll.documentView=s_xferFiles;[card addSubview:scroll];
    ui_button(card,@"展开/折叠目录",NSMakeRect(500,8,140,30),s_uiController,@selector(toggleFolder:));
    s_retryButton=ui_button(card,@"重试选中文件",NSMakeRect(654,8,150,30),s_uiController,@selector(retryFile:));

    card=ui_card(p,NSMakeRect(20,20,840,230));
    ui_label(card,@"传输历史 · 最多保留 256 条",NSMakeRect(22,198,780,20),13,YES);
    s_historyDirection=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(22,160,150,28) pullsDown:NO];
    [s_historyDirection addItemsWithTitles:@[@"全部方向",@"发送",@"接收"]];[card addSubview:s_historyDirection];
    s_historyState=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(185,160,160,28) pullsDown:NO];
    [s_historyState addItemsWithTitles:@[@"全部状态",@"已完成",@"部分完成",@"失败",@"已取消",@"已中断"]];[card addSubview:s_historyState];
    s_historyOrder=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(358,160,160,28) pullsDown:NO];
    [s_historyOrder addItemsWithTitles:@[@"最近完成优先",@"最早完成优先"]];[card addSubview:s_historyOrder];
    NSScrollView *taskHistoryScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,14,804,132)];
    taskHistoryScroll.hasVerticalScroller=YES;taskHistoryScroll.hasHorizontalScroller=YES;
    s_taskHistory=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,126)];
    s_taskHistory.editable=NO;s_taskHistory.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    taskHistoryScroll.documentView=s_taskHistory;[card addSubview:taskHistoryScroll];

    p=s_pages[UI_PAGE_SETTINGS];
    [p setFrameSize:NSMakeSize(880,1400)];
    NSView *settingsForm=[[NSView alloc] initWithFrame:NSMakeRect(0,700,880,680)];
    [p addSubview:settingsForm];
    build_settings_page(settingsForm);

    card=ui_card(p,NSMakeRect(20,240,840,430));
    ui_label(card,@"运行日志",NSMakeRect(22,390,780,22),13,YES);
    s_logNote=ui_label(card,@"尚未读取",NSMakeRect(620,390,198,22),11,NO);
    NSScrollView *logScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,58,804,310)];
    logScroll.hasVerticalScroller=YES;logScroll.hasHorizontalScroller=YES;
    s_logView=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,800,300)];
    s_logView.editable=NO;s_logView.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    logScroll.documentView=s_logView;[card addSubview:logScroll];
    ui_button(card,@"刷新日志",NSMakeRect(584,12,108,32),s_uiController,@selector(refreshLogs:));
    ui_button(card,@"复制日志",NSMakeRect(710,12,108,32),s_uiController,@selector(copyLogs:));

    card=ui_card(p,NSMakeRect(20,20,840,190));
    ui_label(card,@"软件更新",NSMakeRect(22,150,780,22),13,YES);
    s_updateVersion=ui_label(card,[NSString stringWithFormat:@"当前版本：v%s",HOP_VERSION],NSMakeRect(22,112,780,26),18,YES);
    s_updateSource=ui_label(card,@"更新源：GitHub Releases",NSMakeRect(22,80,780,20),12,NO);
    ui_primary_button(card,@"检查更新",NSMakeRect(22,18,130,34),s_uiController,@selector(checkUpdates:));
    ui_label(card,@"更新会校验清单与文件哈希；失败时会明确提示，不会误报为已是最新版本。",
        NSMakeRect(180,20,620,30),11,NO);

    for(int i=0;i<UI_PAGE_COUNT;++i)s_hosts[i].hidden=i!=UI_PAGE_OVERVIEW;
    populate_settings();
}

@implementation HopDeskMainController
- (void)selectPage:(id)sender {
    NSInteger page=[sender tag];
    if ([sender isKindOfClass:[NSButton class]] && page==0 && [sender title].length==0) page=0;
    show_page((int)page);
}
- (void)roleChanged:(id)sender {(void)sender;update_settings_role_fields();mark_settings_dirty();}
- (void)settingsChanged:(id)sender {(void)sender;mark_settings_dirty();}
- (void)settingsTextDidChange:(NSNotification *)notification {(void)notification;mark_settings_dirty();}
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
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    if(s_refreshingTransfers || s_syncTaskSelection)return;
    NSTableView *table=(NSTableView *)notification.object;
    if(table!=s_taskTables[0] && table!=s_taskTables[1])return;
    s_syncTaskSelection=YES;
    [s_taskTables[table==s_taskTables[0]?1:0] deselectAll:nil];
    s_syncTaskSelection=NO;
    refresh_transfer_page();
}
- (void)textViewDidChangeSelection:(NSNotification *)notification {
    if(notification.object==s_xferFiles && !s_refreshingTransfers){
        s_fileSelectionValid=YES;update_transfer_actions();
    }
}
- (void)cancelTask:(id)sender {
    int d=[sender tag]==1?1:0;hop_xfer_task_id id=selected_task(d);
    hop_xfer_task_snapshot task;
    if(!id || !hop_xfer_task_get_snapshot(id,&task))return;
    NSString *name=utf8(task.name);
    if(!name.length)name=[NSString stringWithFormat:@"任务 #%llu",(unsigned long long)id];
    NSAlert *alert=[NSAlert new];alert.alertStyle=NSAlertStyleWarning;
    alert.messageText=[NSString stringWithFormat:@"确定取消%@？",name];
    alert.informativeText=xfer_connected()?
        @"取消请求会立即发送到任务执行器；已经完成的文件不会回滚。":
        @"数据通道当前未连接。取消后，对端的临时文件可能要等连接恢复或断线超时后清理。已经完成的文件不会回滚。";
    [alert addButtonWithTitle:@"返回"];
    [alert addButtonWithTitle:@"取消任务"];
    if([alert runModal]!=NSAlertSecondButtonReturn)return;
    if(!xfer_cancel_task(id)){
        NSAlert *failed=[NSAlert new];failed.messageText=@"任务已结束或正在完成，无法取消。";[failed runModal];
    }
    refresh_transfer_page();
}
- (void)retryFile:(id)sender {
    (void)sender;
    int index=selected_visible_file_index();
    if(index>=0 && s_fileDirection==HOP_XFER_DIR_SEND &&
       !xfer_retry_file(s_fileBatch,index)){
        NSAlert *a=[NSAlert new];a.messageText=@"无法重试";
        a.informativeText=@"文件可能仍在进行、并非失败状态，或这是跨端粘贴任务。跨端粘贴请在目标设备确认剪贴板后重新发起粘贴。";[a runModal];
    }
}
- (void)toggleFolder:(id)sender {
    (void)sender;
    int index=selected_visible_file_index();
    if(index>=0 && s_fileSnapshot[index].is_directory){
        s_collapsedFolders[index]=!s_collapsedFolders[index];
        refresh_transfer_page();
    }
}
- (void)refreshLogs:(id)sender {(void)sender;refresh_logs();}
- (void)copyLogs:(id)sender {
    (void)sender;
    NSPasteboard *pb=[NSPasteboard generalPasteboard];[pb clearContents];
    BOOL copied=[pb setString:s_logView.string ?: @"" forType:NSPasteboardTypeString];
    set_log_note(copied?@"日志已复制到剪贴板":@"复制失败，请重试",!copied);
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
    for(int i=0;i<UI_PAGE_COUNT;++i){
        NSButton *b=[NSButton buttonWithTitle:kPageNames[i] target:s_uiController action:@selector(selectPage:)];
        b.frame=NSMakeRect(18,620-i*52,174,38);b.bordered=NO;b.wantsLayer=YES;b.layer.cornerRadius=8;b.autoresizingMask=NSViewMinYMargin;
        [b setButtonType:NSButtonTypePushOnPushOff];
        b.tag=i;b.alignment=NSTextAlignmentLeft;b.font=[NSFont systemFontOfSize:13];
        [sidebar addSubview:b];s_nav[i]=b;
    }
    s_pageTitle=ui_label(root,@"概览",NSMakeRect(246,736,360,34),23,YES);
    s_pageTitle.autoresizingMask=NSViewMinYMargin;
    s_headerStatus=ui_label(root,@"等待连接",NSMakeRect(830,742,270,24),12,YES);
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
    if(s_page==UI_PAGE_TRANSFER)refresh_transfer_page();
}
void main_ui_shutdown(void) {
    [s_mainTimer invalidate];s_mainTimer=nil;
    [s_mainWindow orderOut:nil];
}
