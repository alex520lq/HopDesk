/* =====================================================================
 * HopDesk - macOS native main window
 * Cocoa implementation of the shared six-page application UI.
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ApplicationServices/ApplicationServices.h>
#import <dispatch/dispatch.h>
#include "mac_app.h"
#include "split_agent.h"
#include "split_ui.h"
#include "xfer.h"
#include "../core/xfer_status.h"
#include "../core/hop.h"
#include "../core/xfer_task_history.h"
#include "../platform/audio_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc/malloc.h>

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
static NSButton *s_audioSend, *s_audioPlay;
static NSTextField *s_audioStatus;
static NSPopUpButton *s_edgePopups[2];
static NSTextField *s_xferTitle, *s_xferDetail, *s_xferCounts;
static NSProgressIndicator *s_xferProgress;
static NSTextView *s_xferFiles, *s_logView, *s_taskHistory;
static NSTextField *s_logNote, *s_filePageLabel;
static NSButton *s_filePreviousButton, *s_fileNextButton;
static BOOL s_fileSelectionValid;
static NSPopUpButton *s_historyDirection, *s_historyState, *s_historyOrder;
static NSButton *s_retryButton, *s_cancelTaskButtons[2];
static BOOL s_refreshingTransfers, s_syncTaskSelection, s_fileBatchActive;
static NSTableView *s_taskTables[2];
static NSArray<NSDictionary *> *s_taskRows[2];
static hop_xfer_task_snapshot *s_tasksSnapshot;
static int s_tasksSnapshotCapacity, s_tasksSnapshotCount;
static uint64_t s_taskRevisionSeen, s_taskSendRevisionSeen, s_taskReceiveRevisionSeen;
static uint64_t s_historyRevisionSeen;
static NSInteger s_historyDirectionSeen=-1, s_historyStateSeen=-1, s_historyOrderSeen=-1;
static NSTextField *s_updateVersion, *s_updateSource;
static NSTimer *s_mainTimer;
static NSObject *s_uiController;
static int s_page;
static int s_connected, s_sharing;
static hop_xfer_file_status *s_fileSnapshot;
enum { UI_FILE_PAGE_SIZE = 128 };
static int s_fileSnapshotCapacity, s_fileSnapshotCount;
static int s_visibleFileIndices[UI_FILE_PAGE_SIZE];
static int s_visibleFileCount;
static NSMutableSet<NSString *> *s_collapsedFolderPaths;
static int s_filePageIndex, s_filePageOffset, s_fileTotalCount;
static uint64_t s_fileRevisionSeen;
static BOOL s_fileSnapshotFrozen, s_filePresentationDirty;
static uint32_t s_fileBatch;
static int s_fileDirection;
static BOOL s_startupPermissionsChecked;
static NSTextField *s_inputPermissionsStatus, *s_overviewPermissions;

/* Destroying the window must also drop every static strong reference below.
 * Otherwise closing the window only hides it and the full 4-page UI stays
 * resident for the rest of the process lifetime. */
static void main_ui_destroy(void);
static void refresh_audio_status(void);

/* Permissions belong to the process doing input work. The split UI must use
 * the agent's existing status snapshot, not assume its own TCC grants suffice. */
static BOOL input_permissions(int *post, int *listen, int *accessibility) {
    if (split_ui_is_active())
        return split_ui_get_input_permissions(post, listen, accessibility);
    *post = CGPreflightPostEventAccess();
    *listen = CGPreflightListenEventAccess();
    *accessibility = AXIsProcessTrusted();
    return YES;
}

static NSString *input_permissions_status(void) {
    int post = 0, listen = 0, accessibility = 0;
    if (!input_permissions(&post, &listen, &accessibility))
        return @"后台未连接，无法确认键鼠权限";
    return [NSString stringWithFormat:@"设备控制：%@    输入监控：%@",
        post && accessibility ? @"已授权" : @"未授权", listen ? @"已授权" : @"未授权"];
}

static void open_input_permission_settings(BOOL listen) {
    /* Explicit user actions remain available after the once-per-launch prompt,
     * including when Settings shows a stale enabled entry after an update. */
    if (listen) {
        (void)CGRequestListenEventAccess();
    } else {
        NSDictionary *prompt = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)prompt);
        (void)CGRequestPostEventAccess();
    }
    NSString *pane = listen ? @"Privacy_ListenEvent" : @"Privacy_Accessibility";
    NSString *base = @"x-apple.systempreferences:com.apple.preference.security?";
    if (@available(macOS 13.0, *))
        base = @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?";
    BOOL opened = [NSWorkspace.sharedWorkspace openURL:
        [NSURL URLWithString:[base stringByAppendingString:pane]]];
    if (!opened) {
        NSString *legacy = [@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane];
        opened = [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:legacy]];
    }
    hop_log(opened ? HOP_LOG_INFO : HOP_LOG_ERROR, "permission: %s %s settings for %s",
        opened ? "opened" : "unable to open", listen ? "Input Monitoring" : "Accessibility",
        NSBundle.mainBundle.bundlePath.UTF8String);
    if (!opened) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"无法打开系统权限设置";
        alert.informativeText = @"请打开“系统设置 → 隐私与安全性”，选择设备控制（辅助功能）或输入监控，为 HopDesk 开启权限。";
        [alert runModal];
    }
}

void mac_request_accessibility_permission(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (split_agent_is_active()) return; /* Background agents never display prompts. */
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"HopDesk 键鼠权限";
        alert.informativeText = [NSString stringWithFormat:
            @"%@\n\n请在两个设置页为这份应用授权：\n%@\n\n授权后请退出并重新启动 HopDesk（双进程模式还需停止并重启后台）。如果更新后开关已开但仍未授权，请移除旧条目，再添加上面路径中的 HopDesk。",
            input_permissions_status(), NSBundle.mainBundle.bundlePath];
        [alert addButtonWithTitle:@"打开设备控制设置"];
        [alert addButtonWithTitle:@"打开输入监控设置"];
        [alert addButtonWithTitle:@"稍后"];
        [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse response = [alert runModal];
        if (response == NSAlertFirstButtonReturn) open_input_permission_settings(NO);
        if (response == NSAlertSecondButtonReturn) open_input_permission_settings(YES);
    });
}

void mac_check_input_permissions(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_startupPermissionsChecked || split_agent_is_active()) return;
        s_startupPermissionsChecked = YES;
        int post = 0, listen = 0, accessibility = 0;
        if (!input_permissions(&post, &listen, &accessibility) || !post || !listen || !accessibility)
            mac_request_accessibility_permission();
    });
}

static NSString *const kPageNames[UI_PAGE_COUNT] = {@"概览",@"设备",@"传输",@"设置"};

@interface HopDeskLogView : NSTextView
@end

@implementation HopDeskLogView
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    // The read-only log needs only selection and copy. The default text menu warms system service caches.
    NSMenu *menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@""].target = self;
    return menu;
}
@end

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
- (void)previousFilePage:(id)sender;
- (void)nextFilePage:(id)sender;
- (void)refreshLogs:(id)sender;
- (void)copyLogs:(id)sender;
- (void)checkUpdates:(id)sender;
- (void)openInputPermissionSettings:(id)sender;
- (void)checkInputPermissions:(id)sender;
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
    set_settings_note(@"有未保存的更改。音频设置可立即生效；网络、认证和角色设置需重启。",NO);
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
        @"客户端：接收服务器键鼠；音频发送/播放独立配置。":
        @"服务器：向客户端提供键鼠；音频发送/播放独立配置。";
}
static void set_settings_editors_enabled(BOOL enabled) {
    for(int i=0;i<CFG_COUNT;++i)if(s_cfg[i])s_cfg[i].enabled=enabled;
    if(s_rolePopup)s_rolePopup.enabled=enabled;
    for(int i=0;i<2;++i)if(s_edgePopups[i])s_edgePopups[i].enabled=enabled;
    if(s_audioSend)s_audioSend.enabled=enabled;
    if(s_audioPlay)s_audioPlay.enabled=enabled;
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
    s_audioSend.state=(c->audio_mode & HOP_AUDIO_SEND)?NSControlStateValueOn:NSControlStateValueOff;
    s_audioPlay.state=(c->audio_mode & HOP_AUDIO_PLAY)?NSControlStateValueOn:NSControlStateValueOff;
    refresh_audio_status();
    update_settings_role_fields();
    s_populatingSettings=NO;
    s_settingsDirty=NO;
    set_settings_note(@"设置已加载。音频保存后立即应用；网络、认证和角色参数重启后生效。",NO);
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
    next.audio_mode=(s_audioSend.state==NSControlStateValueOn?HOP_AUDIO_SEND:0u) |
                    (s_audioPlay.state==NSControlStateValueOn?HOP_AUDIO_PLAY:0u);
    if(next.audio_mode && port==HOP_AUDIO_PORT){
        settings_validation_error(CFG_PORT,@"控制端口 24819 已保留给系统音频，请选择其他端口。");return;
    }
    if(next.audio_mode && dataPort==HOP_AUDIO_PORT){
        settings_validation_error(CFG_DATA_PORT,@"数据端口 24819 已保留给系统音频，请选择其他端口。");return;
    }
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
                if(split_ui_set_audio_mode(next.audio_mode))
                    set_settings_note(@"设置已保存。音频设置立即生效；网络、认证和角色参数重启后生效。",NO);
                else
                    set_settings_note(@"设置已保存，但后台未能应用音频设置。请确认后台运行；重启后会按已保存配置重试。",YES);
                refresh_audio_status();
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
static BOOL resize_task_snapshot(int capacity) {
    if (capacity < 0 || capacity > HOP_TASK_MANAGER_MAX_LIVE) return NO;
    if (capacity == s_tasksSnapshotCapacity) return YES;
    if (capacity == 0) {
        free(s_tasksSnapshot);
        s_tasksSnapshot = NULL;
        s_tasksSnapshotCapacity = 0;
        return YES;
    }
    hop_xfer_task_snapshot *next = realloc(s_tasksSnapshot,
        (size_t)capacity * sizeof(*s_tasksSnapshot));
    if (!next) return NO;
    s_tasksSnapshot = next;
    s_tasksSnapshotCapacity = capacity;
    return YES;
}

static void refresh_task_lists(const hop_xfer_status_pair *pair, uint64_t taskRevision) {
    if (!s_taskTables[0] || !s_taskTables[1] || !pair) return;
    if (taskRevision == s_taskRevisionSeen &&
        pair->send_revision == s_taskSendRevisionSeen &&
        pair->receive_revision == s_taskReceiveRevisionSeen)
        return;

    hop_xfer_task_id selected[2] = {selected_task(0), selected_task(1)};
    int total = 0, n = -1;
    uint64_t revision = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (split_ui_get_tasks(NULL, 0, &total, &revision) < 0 ||
            total < 0 || total > HOP_TASK_MANAGER_MAX_LIVE)
            return;
        if (!resize_task_snapshot(total)) return;
        int actualTotal = 0;
        uint64_t actualRevision = 0;
        n = split_ui_get_tasks(s_tasksSnapshot, total, &actualTotal, &actualRevision);
        if (n < 0) return;
        revision = actualRevision;
        if (n == actualTotal) {
            total = actualTotal;
            if (actualTotal < s_tasksSnapshotCapacity && !resize_task_snapshot(actualTotal)) return;
            break;
        }
        total = actualTotal;
        n = -1;
    }
    if (n < 0 || n != total) return;
    s_tasksSnapshotCount = n;

    NSMutableArray<NSDictionary *> *rows[2] = {[NSMutableArray array], [NSMutableArray array]};
    for (int i = 0; i < n; ++i) {
        hop_xfer_task_snapshot *t = &s_tasksSnapshot[i];
        int d = t->direction == HOP_XFER_DIR_SEND ? 0 :
                t->direction == HOP_XFER_DIR_RECEIVE ? 1 : -1;
        if (d < 0 || t->state >= HOP_XFER_TASK_COMPLETED ||
            t->kind == HOP_TASK_KIND_IMAGE_WORK) continue;
        hop_xfer_status current = d == 0 ? pair->send : pair->receive;
        BOOL currentBatch = t->transfer_id && current.batch == t->transfer_id;
        NSString *name = utf8(t->name);
        if (!name.length && currentBatch && current.name[0]) name = utf8(current.name);
        if (!name.length) name = [NSString stringWithFormat:@"任务 #%llu", (unsigned long long)t->id];
        uint32_t done = currentBatch ? current.files_done : t->files_done;
        uint32_t count = currentBatch ? current.files_total : t->files_total;
        uint64_t bytesDone = currentBatch ? current.bytes_done : t->bytes_done;
        uint64_t bytesTotal = currentBatch ? current.bytes_total : t->bytes_total;
        double progress = bytesTotal ? MIN(100.0, 100.0 * (double)bytesDone / (double)bytesTotal) : 0.0;
        NSString *progressText = currentBatch || bytesTotal ?
            [NSString stringWithFormat:@"%.0f%%  ·  文件 %u/%u  ·  %@/%@",
                progress, done, count, byte_count(bytesDone), byte_count(bytesTotal)] :
            (t->state == HOP_XFER_TASK_SCANNING ? @"正在准备" : @"等待开始");
        NSString *text = [NSString stringWithFormat:@"%@  ·  %@  ·  %@", name, task_state_name(t), progressText];
        [rows[d] addObject:@{@"id": @(t->id), @"text": text}];
    }
    for (int d = 0; d < 2; ++d) if (rows[d].count == 0)
        [rows[d] addObject:@{@"id": @0, @"text": d ? @"暂无活动的接收任务" : @"暂无活动的发送任务"}];

    for (int d = 0; d < 2; ++d) {
        NSArray<NSDictionary *> *nextRows = [rows[d] copy];
        if ([s_taskRows[d] isEqualToArray:nextRows]) continue;
        NSScrollView *scroll = s_taskTables[d].enclosingScrollView;
        NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        s_taskRows[d] = nextRows;
        [s_taskTables[d] reloadData];
        [s_taskTables[d] deselectAll:nil];
        if (selected[d]) for (NSUInteger i = 0; i < s_taskRows[d].count; ++i) {
            if ([s_taskRows[d][i][@"id"] unsignedLongLongValue] == selected[d]) {
                [s_taskTables[d] selectRowIndexes:[NSIndexSet indexSetWithIndex:i]
                                byExtendingSelection:NO];
                break;
            }
        }
        if (scroll) {
            NSClipView *clip = scroll.contentView;
            [clip scrollToPoint:origin];
            [scroll reflectScrolledClipView:clip];
        }
    }

    s_taskRevisionSeen = revision;
    s_taskSendRevisionSeen = pair->send_revision;
    s_taskReceiveRevisionSeen = pair->receive_revision;
}

static void refresh_task_history(uint64_t historyRevision) {
    if (!s_taskHistory || !s_historyDirection || !s_historyState || !s_historyOrder) return;
    static const int directions[] = {HOP_XFER_DIR_NONE, HOP_XFER_DIR_SEND, HOP_XFER_DIR_RECEIVE};
    static const hop_xfer_task_state states[] = {HOP_XFER_TASK_EMPTY, HOP_XFER_TASK_COMPLETED,
        HOP_XFER_TASK_PARTIAL, HOP_XFER_TASK_FAILED, HOP_XFER_TASK_CANCELLED, HOP_XFER_TASK_INTERRUPTED};
    NSInteger d = s_historyDirection.indexOfSelectedItem;
    NSInteger st = s_historyState.indexOfSelectedItem;
    NSInteger order = s_historyOrder.indexOfSelectedItem;
    if (d < 0 || d > 2 || st < 0 || st > 5 || order < 0 || order > 1) return;
    if (historyRevision == s_historyRevisionSeen &&
        d == s_historyDirectionSeen && st == s_historyStateSeen && order == s_historyOrderSeen)
        return;

    int total = 0;
    uint64_t revision = 0;
    if (split_ui_get_history(NULL, 0, directions[d], states[st], order == 0,
                             &total, &revision) < 0 || total < 0 ||
        total > HOP_TASK_HISTORY_CAPACITY) return;
    hop_xfer_task_history_record *history = total ?
        calloc((size_t)total, sizeof(*history)) : NULL;
    if (total && !history) return;
    int actualTotal = 0;
    uint64_t actualRevision = 0;
    int n = split_ui_get_history(history, total, directions[d], states[st], order == 0,
                                 &actualTotal, &actualRevision);
    if (n < 0) { free(history); return; }
    if (n != actualTotal) {
        free(history);
        total = actualTotal;
        history = total ? calloc((size_t)total, sizeof(*history)) : NULL;
        if (total && !history) return;
        n = split_ui_get_history(history, total, directions[d], states[st], order == 0,
                                 &actualTotal, &actualRevision);
        if (n < 0 || n != actualTotal) { free(history); return; }
    }
    revision = actualRevision;

    NSMutableString *rows = [NSMutableString string];
    for (int i = 0; i < n; ++i) {
        const hop_xfer_task_history_record *t = &history[i];
        hop_xfer_task_snapshot display = {0};
        display.state = (hop_xfer_task_state)t->state;
        NSString *name = utf8(t->name);
        if (!name.length) name = [NSString stringWithFormat:@"任务 #%llu", (unsigned long long)t->record_id];
        [rows appendFormat:@"%@  ·  %@  ·  %@  ·  文件 %u/%u  ·  %@/%@\n",
            t->direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收", name, task_state_name(&display),
            t->files_done, t->files_total, byte_count(t->bytes_done), byte_count(t->bytes_total)];
    }
    free(history);
    NSString *next = rows.length ? rows : @"暂无符合条件的任务";
    if (![s_taskHistory.string isEqualToString:next]) {
        NSScrollView *scroll = s_taskHistory.enclosingScrollView;
        NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        s_taskHistory.string = next;
        if (scroll) {
            NSClipView *clip = scroll.contentView;
            [clip scrollToPoint:origin];
            [scroll reflectScrolledClipView:clip];
        }
    }
    s_historyRevisionSeen = revision;
    s_historyDirectionSeen = d;
    s_historyStateSeen = st;
    s_historyOrderSeen = order;
}
static int selected_visible_file_index(void) {
    if(!s_fileSelectionValid || !s_xferFiles || !s_fileSnapshot)return -1;
    NSString *text=s_xferFiles.string ?: @"";
    NSUInteger location=MIN(s_xferFiles.selectedRange.location,text.length),line=0;
    for(NSUInteger i=0;i<location;++i)if([text characterAtIndex:i]=='\n')line++;
    if(line>=(NSUInteger)s_visibleFileCount)return -1;
    return s_visibleFileIndices[line];
}
static const hop_xfer_file_status *selected_file_status(void) {
    int index = selected_visible_file_index();
    int local = index - s_filePageOffset;
    return index >= 0 && local >= 0 && local < s_fileSnapshotCount ? &s_fileSnapshot[local] : NULL;
}
static BOOL resize_file_snapshot(int capacity) {
    if (capacity < 0 || capacity > UI_FILE_PAGE_SIZE) return NO;
    if (capacity == s_fileSnapshotCapacity) return YES;
    if (capacity == 0) {
        free(s_fileSnapshot); s_fileSnapshot = NULL; s_fileSnapshotCapacity = 0;
        return YES;
    }
    hop_xfer_file_status *next = realloc(s_fileSnapshot, (size_t)capacity * sizeof(*s_fileSnapshot));
    if (!next) return NO;
    s_fileSnapshot = next; s_fileSnapshotCapacity = capacity;
    return YES;
}
static const hop_xfer_task_snapshot *find_task_snapshot(hop_xfer_task_id id) {
    if (!id || !s_tasksSnapshot) return NULL;
    for (int i = 0; i < s_tasksSnapshotCount; ++i)
        if (s_tasksSnapshot[i].id == id) return &s_tasksSnapshot[i];
    return NULL;
}
static void set_label_if_changed(NSTextField *field, NSString *value) {
    if (field && ![field.stringValue isEqualToString:value ?: @""])
        field.stringValue = value ?: @"";
}
static void update_transfer_actions(void) {
    const hop_xfer_file_status *file=selected_file_status();
    BOOL canRetry=s_fileDirection==HOP_XFER_DIR_SEND && !s_fileBatchActive &&
        !(s_fileBatch & HOP_XFER_PASTE_BATCH_FLAG) && file &&
        file->phase==HOP_XFER_FILE_FAILED && !file->is_directory;
    if(s_retryButton)s_retryButton.enabled=canRetry;
    for(int d=0;d<2;++d){
        BOOL canCancel=NO;
        hop_xfer_task_id id=selected_task(d);
        const hop_xfer_task_snapshot *task=find_task_snapshot(id);
        if(task) canCancel=task->state<HOP_XFER_TASK_COMPLETED && !task->cancel_requested && !task->completing;
        if(s_cancelTaskButtons[d])s_cancelTaskButtons[d].enabled=canCancel;
    }
}
static void refresh_transfer_page(void) {
    if (!s_xferTitle || s_refreshingTransfers) return;
    int oldSelectedFileIndex = selected_visible_file_index();
    uint32_t oldFileBatch = s_fileBatch;
    int oldFileDirection = s_fileDirection;
    int oldFilePageIndex = s_filePageOffset >= 0 ? s_filePageOffset / UI_FILE_PAGE_SIZE : s_filePageIndex;
    NSScrollView *fileScroll = s_xferFiles.enclosingScrollView;
    NSPoint oldFileScrollOrigin = fileScroll ? fileScroll.contentView.bounds.origin : NSZeroPoint;
    s_refreshingTransfers = YES;

    split_ipc_transfer_state transfer = {0};
    (void)split_ui_get_transfer_state(&transfer);
    hop_xfer_status_pair pair = transfer.status;
    refresh_task_lists(&pair, transfer.task_revision);
    refresh_task_history(transfer.history_revision);

    hop_xfer_task_snapshot selectedSnapshot = {0};
    hop_xfer_task_id selectedId = selected_task(0);
    if (!selectedId) selectedId = selected_task(1);
    const hop_xfer_task_snapshot *selected = find_task_snapshot(selectedId);
    BOOL hasSelected = selected != NULL;
    if (selected) selectedSnapshot = *selected;

    BOOL hasTaskStatus = NO;
    hop_xfer_status st = pair.primary_direction == HOP_XFER_DIR_SEND ? pair.send : pair.receive;
    if (hasSelected) {
        hop_xfer_status candidate = selectedSnapshot.direction == HOP_XFER_DIR_SEND ? pair.send : pair.receive;
        if (selectedSnapshot.transfer_id && candidate.batch == selectedSnapshot.transfer_id) {
            st = candidate;
            hasTaskStatus = YES;
        } else {
            memset(&st, 0, sizeof(st));
            st.direction = selectedSnapshot.direction;
        }
    }

    NSString *name = hasSelected ? utf8(selectedSnapshot.name) : utf8(st.name);
    if (!name.length && hasTaskStatus && st.name[0]) name = utf8(st.name);
    if (!name.length) name = hasSelected ?
        [NSString stringWithFormat:@"%@任务 #%llu",
            selectedSnapshot.direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收",
            (unsigned long long)selectedId] :
        (st.name[0] ? utf8(st.name) : @"暂无传输任务");

    NSString *phase = @"空闲";
    switch (st.phase) {
        case HOP_XFER_QUEUED: phase = @"排队中"; break;
        case HOP_XFER_SCANNING: phase = @"扫描中"; break;
        case HOP_XFER_SENDING: phase = @"发送中"; break;
        case HOP_XFER_RECEIVING: phase = @"接收中"; break;
        case HOP_XFER_VERIFYING: phase = @"校验中"; break;
        case HOP_XFER_COMPLETED: phase = @"已完成"; break;
        case HOP_XFER_PARTIAL: phase = @"部分失败"; break;
        case HOP_XFER_FAILED: phase = @"失败"; break;
        case HOP_XFER_CANCELLED: phase = @"已取消"; break;
    }
    if (hasSelected && !hasTaskStatus) phase = task_state_name(&selectedSnapshot);
    NSString *direction = hasSelected ?
        (selectedSnapshot.direction == HOP_XFER_DIR_SEND ? @"发送" : @"接收") :
        (st.direction == HOP_XFER_DIR_SEND ? @"发送" :
         st.direction == HOP_XFER_DIR_RECEIVE ? @"接收" : @"待机");

    uint64_t now_ms = hop_monotonic_ms();
    uint64_t speed = hasTaskStatus ? st.speed_bytes_per_sec : 0;
    if (!st.last_progress_ms || now_ms < st.last_progress_ms || now_ms - st.last_progress_ms > 2000)
        speed = 0;
    NSString *rate = speed ? [NSString stringWithFormat:@"%@/s", byte_count(speed)] : @"速率 --";
    NSString *eta = speed ? [NSString stringWithFormat:@"ETA %llus", (unsigned long long)st.eta_seconds] : @"ETA --";
    NSString *detail = hasTaskStatus ? utf8(st.detail) :
        (hasSelected ? utf8(selectedSnapshot.detail) : @"等待文件传输");
    if (!detail.length) detail = hasSelected && !hasTaskStatus ? @"实时进度将在任务开始后显示" : @"";
    set_label_if_changed(s_xferTitle, name);
    set_label_if_changed(s_xferDetail,
        [NSString stringWithFormat:@"%@  ·  %@  ·  %@  ·  %@  ·  %@", direction, phase, detail, rate, eta]);

    uint32_t filesDone = 0, filesTotal = 0, queued = 0;
    uint64_t bytesDone = 0, bytesTotal = 0;
    if (hasTaskStatus) {
        bytesDone = st.bytes_done; bytesTotal = st.bytes_total;
        filesDone = st.files_done; filesTotal = st.files_total; queued = st.queued_count;
    } else if (hasSelected) {
        bytesDone = selectedSnapshot.bytes_done; bytesTotal = selectedSnapshot.bytes_total;
        filesDone = selectedSnapshot.files_done; filesTotal = selectedSnapshot.files_total;
    } else {
        bytesDone = st.bytes_done; bytesTotal = st.bytes_total;
        filesDone = st.files_done; filesTotal = st.files_total; queued = st.queued_count;
    }
    double progress = bytesTotal ? MIN(100.0, 100.0 * (double)bytesDone / (double)bytesTotal) :
        (hasTaskStatus && st.phase == HOP_XFER_COMPLETED ? 100.0 : 0.0);
    NSString *counts = nil;
    if (hasSelected && !hasTaskStatus && !bytesTotal && !filesTotal) {
        counts = @"任务正在准备或排队，开始后显示实时进度";
    } else {
        counts = [NSString stringWithFormat:@"%.0f%%  ·  文件 %u / %u    排队 %u    数据 %@ / %@",
            progress, filesDone, filesTotal, queued, byte_count(bytesDone), byte_count(bytesTotal)];
        if (!hasSelected && pair.send.active && pair.receive.active)
            counts = [NSString stringWithFormat:@"双向传输  ·  发送 %@/%@  ·  接收 %@/%@  ·  排队 %u",
                byte_count(pair.send.bytes_done), byte_count(pair.send.bytes_total),
                byte_count(pair.receive.bytes_done), byte_count(pair.receive.bytes_total), pair.send.queued_count];
    }
    set_label_if_changed(s_xferCounts, counts);
    if (s_xferProgress.doubleValue != progress) s_xferProgress.doubleValue = progress;

    int fileDirection = hasSelected ? selectedSnapshot.direction : st.direction;
    uint32_t fileBatch = hasSelected ? selectedSnapshot.transfer_id : st.batch;
    s_fileBatchActive = hasSelected ? (hasTaskStatus && st.active) : st.active;
    BOOL contextChanged = oldFileBatch != fileBatch || oldFileDirection != fileDirection;
    if (contextChanged) {
        s_fileBatch = fileBatch;
        s_fileDirection = fileDirection;
        s_filePageIndex = 0;
        s_filePageOffset = -1;
        s_fileTotalCount = 0;
        s_fileSnapshotCount = 0;
        (void)resize_file_snapshot(0);
        s_fileRevisionSeen = 0;
        s_fileSnapshotFrozen = NO;
        s_filePresentationDirty = YES;
        s_fileSelectionValid = NO;
        [s_collapsedFolderPaths removeAllObjects];
    }

    hop_xfer_status fileStatus = fileDirection == HOP_XFER_DIR_SEND ? pair.send : pair.receive;
    uint64_t fileRevision = fileStatus.batch == fileBatch ?
        (fileDirection == HOP_XFER_DIR_SEND ? pair.send_revision : pair.receive_revision) : 0;
    BOOL fileTerminal = !s_fileBatchActive && fileStatus.batch == fileBatch &&
        (fileStatus.phase == HOP_XFER_COMPLETED || fileStatus.phase == HOP_XFER_PARTIAL ||
         fileStatus.phase == HOP_XFER_FAILED || fileStatus.phase == HOP_XFER_CANCELLED);

    if (fileBatch && fileDirection != HOP_XFER_DIR_NONE) {
        int pages = s_fileTotalCount > 0 ?
            (s_fileTotalCount + UI_FILE_PAGE_SIZE - 1) / UI_FILE_PAGE_SIZE : 1;
        if (s_filePageIndex >= pages) s_filePageIndex = pages - 1;
        if (s_filePageIndex < 0) s_filePageIndex = 0;
    } else {
        s_filePageIndex = 0;
        s_fileTotalCount = 0;
        s_fileSnapshotCount = 0;
        (void)resize_file_snapshot(0);
    }

    int requestedOffset = s_filePageIndex * UI_FILE_PAGE_SIZE;
    BOOL pageChanged = requestedOffset != s_filePageOffset;
    BOOL needFetch = contextChanged || pageChanged || fileRevision != s_fileRevisionSeen;
    if (s_fileSnapshotFrozen && !contextChanged && !pageChanged) needFetch = NO;

    if (needFetch && fileBatch && fileDirection != HOP_XFER_DIR_NONE) {
        int total = 0, count = 0;
        uint64_t revision = 0;
        BOOL fetched = NO;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (split_ui_get_files(fileDirection, fileBatch, (uint32_t)requestedOffset,
                                   NULL, 0, &total, &revision) < 0 || total < 0) break;
            int desired = requestedOffset < total ? MIN(UI_FILE_PAGE_SIZE, total - requestedOffset) : 0;
            if (!resize_file_snapshot(desired)) break;
            int actualTotal = 0;
            uint64_t actualRevision = 0;
            count = split_ui_get_files(fileDirection, fileBatch, (uint32_t)requestedOffset,
                s_fileSnapshot, desired, &actualTotal, &actualRevision);
            if (count < 0) break;
            total = actualTotal;
            revision = actualRevision;
            if (count == (requestedOffset < total ? MIN(UI_FILE_PAGE_SIZE, total - requestedOffset) : 0))
                { fetched = YES; break; }
        }
        if (fetched) {
            s_fileSnapshotCount = count;
            s_fileTotalCount = total;
            s_filePageOffset = requestedOffset;
            s_fileRevisionSeen = revision;
            if (fileTerminal) s_fileSnapshotFrozen = YES;
        }
    } else if (needFetch && (!fileBatch || fileDirection == HOP_XFER_DIR_NONE)) {
        s_filePageOffset = requestedOffset;
        s_fileRevisionSeen = 0;
        s_fileSnapshotCount = 0;
        s_fileTotalCount = 0;
    }

    int pageCount = s_fileTotalCount > 0 ?
        (s_fileTotalCount + UI_FILE_PAGE_SIZE - 1) / UI_FILE_PAGE_SIZE : 1;
    if (s_filePageLabel) {
        NSString *label = s_fileTotalCount ?
            [NSString stringWithFormat:@"第 %d / %d 页  ·  共 %d 项", s_filePageIndex + 1, pageCount, s_fileTotalCount] :
            @"暂无文件明细";
        set_label_if_changed(s_filePageLabel, label);
    }
    if (s_filePreviousButton) s_filePreviousButton.enabled = s_filePageIndex > 0;
    if (s_fileNextButton) s_fileNextButton.enabled = s_fileTotalCount > requestedOffset + UI_FILE_PAGE_SIZE;

    BOOL sameFileContext = oldFileBatch == fileBatch && oldFileDirection == fileDirection;
    BOOL sameFilePage = sameFileContext && oldFilePageIndex == s_filePageIndex;
    BOOL renderFiles = needFetch || s_filePresentationDirty;
    if (renderFiles && s_filePageOffset == requestedOffset) {
        NSMutableString *files = [NSMutableString string];
        s_visibleFileCount = 0;
        for (int i = 0; i < s_fileSnapshotCount; ++i) {
            NSString *path = utf8(s_fileSnapshot[i].relative_path);
            BOOL hidden = NO;
            for (NSString *folder in s_collapsedFolderPaths) {
                if ([path hasPrefix:[folder stringByAppendingString:@"/"]]) { hidden = YES; break; }
            }
            if (hidden) continue;
            int globalIndex = requestedOffset + i;
            s_visibleFileIndices[s_visibleFileCount++] = globalIndex;
            NSUInteger depth = [[path componentsSeparatedByString:@"/"] count] - 1;
            NSString *indent = [@"" stringByPaddingToLength:MIN(depth * 2, 24)
                withString:@" " startingAtIndex:0];
            int filePhase = s_fileSnapshot[i].phase;
            NSString *state = filePhase == HOP_XFER_FILE_COMPLETED ? @"完成" :
                filePhase == HOP_XFER_FILE_FAILED ? @"失败" :
                filePhase == HOP_XFER_FILE_SENDING ? @"发送中" :
                filePhase == HOP_XFER_FILE_RECEIVING ? @"接收中" :
                filePhase == HOP_XFER_FILE_VERIFYING ? @"校验中" : @"排队中";
            NSString *kind = s_fileSnapshot[i].is_directory ?
                ([s_collapsedFolderPaths containsObject:path] ? @"▸ 目录" : @"▾ 目录") : @"文件";
            [files appendFormat:@"%@%@ %@    %@ / %@    %@\n", indent, kind, path,
                byte_count(s_fileSnapshot[i].bytes_done), byte_count(s_fileSnapshot[i].bytes_total), state];
        }
        NSString *nextFiles = files.length ? files :
            (s_fileTotalCount ? @"当前页中的目录内容已折叠" :
             (hasSelected ? (selectedSnapshot.state <= HOP_XFER_TASK_QUEUED ?
                @"任务正在准备或排队，开始后显示文件明细" : @"此任务暂无文件明细") : @"当前没有文件明细"));
        if (![s_xferFiles.string isEqualToString:nextFiles]) {
            s_xferFiles.string = nextFiles;
            s_fileSelectionValid = NO;
            if (sameFilePage && oldSelectedFileIndex >= 0) {
                for (int line = 0; line < s_visibleFileCount; ++line)
                    if (s_visibleFileIndices[line] == oldSelectedFileIndex) {
                        NSUInteger start = 0;
                        for (int row = 0; row < line; ++row) {
                            NSRange newline = [nextFiles rangeOfString:@"\n" options:0
                                range:NSMakeRange(start, nextFiles.length - start)];
                            if (newline.location == NSNotFound) { start = nextFiles.length; break; }
                            start = NSMaxRange(newline);
                        }
                        NSRange newline = [nextFiles rangeOfString:@"\n" options:0
                            range:NSMakeRange(start, nextFiles.length - start)];
                        NSUInteger end = newline.location == NSNotFound ? nextFiles.length : newline.location;
                        if (end > start) {
                            [s_xferFiles setSelectedRange:NSMakeRange(start, end - start)];
                            s_fileSelectionValid = YES;
                        }
                        break;
                    }
            }
        } else if (!sameFilePage) {
            [s_xferFiles setSelectedRange:NSMakeRange(0, 0)];
            s_fileSelectionValid = NO;
        }
        if (fileScroll) {
            NSClipView *clip = fileScroll.contentView;
            if (sameFilePage) [clip scrollToPoint:oldFileScrollOrigin];
            else [s_xferFiles scrollRangeToVisible:NSMakeRange(0, 0)];
            [fileScroll reflectScrolledClipView:clip];
        }
        s_filePresentationDirty = NO;
    }

    update_transfer_actions();
    s_refreshingTransfers = NO;
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
static void refresh_audio_status(void) {
    if(!s_audioStatus)return;
    hop_audio_status audio={0};
    split_ui_get_audio_status(&audio);
    NSMutableArray<NSString *> *parts=[NSMutableArray array];
    if(audio.requested & HOP_AUDIO_SEND){
        NSString *state=(audio.active & HOP_AUDIO_SEND)?@"已连接":
            (audio.unavailable & HOP_AUDIO_SEND)?@"不可用":@"等待对端播放";
        [parts addObject:[NSString stringWithFormat:@"发送：%@",state]];
    }
    if(audio.requested & HOP_AUDIO_PLAY){
        NSString *state=(audio.active & HOP_AUDIO_PLAY)?@"已连接":
            (audio.unavailable & HOP_AUDIO_PLAY)?@"不可用":@"等待对端发送";
        [parts addObject:[NSString stringWithFormat:@"播放：%@",state]];
    }
    set_label_if_changed(s_audioStatus, parts.count ? [parts componentsJoinedByString:@"    ·    "] : @"音频已关闭");
}
static void refresh_connection_labels(void) {
    NSString *status=link_status();
    set_label_if_changed(s_headerStatus,status);
    set_label_if_changed(s_overviewStatus,status);
    set_label_if_changed(s_overviewPeer,[NSString stringWithFormat:@"%@  ·  %@  ·  控制端口 %u  ·  数据端口 %u",
        g_mac.cfg.role==HOP_ROLE_SERVER?@"服务器 · 提供键鼠":@"客户端 · 接收键鼠",g_mac.cfg.role==HOP_ROLE_SERVER?s_addresses:utf8(g_mac.cfg.peer_host), (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port]);
    set_label_if_changed(s_overviewControl,s_connected?@"已连接":@"等待连接");
    set_label_if_changed(s_overviewData,split_ui_data_connected()?@"已连接":@"等待连接");
    set_label_if_changed(s_overviewSharing,s_sharing?
        (g_mac.cfg.role==HOP_ROLE_SERVER?@"已移交客户端":@"正在接收输入"):
        s_connected?@"已就绪 · 等待边缘移交":@"等待连接");
    set_label_if_changed(s_connectionRole,g_mac.cfg.role==HOP_ROLE_SERVER?@"本机角色：服务器 · 提供键鼠":@"本机角色：客户端 · 接收键鼠");
    set_label_if_changed(s_connectionStatus,status);
    set_label_if_changed(s_connectionPeer,g_mac.cfg.role==HOP_ROLE_SERVER?[NSString stringWithFormat:@"本机 IP：%@",s_addresses]:[NSString stringWithFormat:@"服务器 IP：%@",utf8(g_mac.cfg.peer_host)]);
    set_label_if_changed(s_connectionPorts,[NSString stringWithFormat:@"控制端口：%u    数据端口：%u",
        (unsigned)g_mac.cfg.port,(unsigned)g_mac.cfg.data_port]);
    set_label_if_changed(s_footer,[NSString stringWithFormat:@"HopDesk  v%s    ·    %@",HOP_VERSION,status]);
    ui_apply_status_badge(s_headerStatus,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewStatus,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewControl,s_connected,s_sharing);
    ui_apply_status_badge(s_overviewData,split_ui_data_connected(),NO);
    ui_apply_status_badge(s_overviewSharing,s_connected,s_sharing);
    NSString *permissions = input_permissions_status();
    set_label_if_changed(s_inputPermissionsStatus, permissions);
    set_label_if_changed(s_overviewPermissions, permissions);
    int post = 0, listen = 0, accessibility = 0;
    BOOL known = input_permissions(&post, &listen, &accessibility);
    if (!known || !post || !listen || !accessibility) {
        set_label_if_changed(s_overviewSharing, known ? @"权限不足 · 请检查键鼠权限" : @"后台未连接 · 权限未知");
        s_overviewSharing.textColor = NSColor.systemOrangeColor;
    }
    refresh_audio_status();
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
    s_hostHint=ui_label(left,@"服务器：向客户端提供键鼠；音频能力独立配置。",NSMakeRect(18,398,360,18),11,NO);
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
    ui_label(left,@"音频能力（独立于本机角色）",NSMakeRect(18,50,360,18),12,YES);
    s_audioSend=[NSButton checkboxWithTitle:@"发送本机系统声音" target:s_uiController action:@selector(settingsChanged:)];
    s_audioSend.frame=NSMakeRect(18,24,178,22);
    s_audioSend.toolTip=@"发送本机系统声音；需要对端启用播放；需要 macOS 14.2 或更高版本";
    [left addSubview:s_audioSend];
    s_audioPlay=[NSButton checkboxWithTitle:@"播放对端系统声音" target:s_uiController action:@selector(settingsChanged:)];
    s_audioPlay.frame=NSMakeRect(198,24,180,22);
    s_audioPlay.toolTip=@"播放对端系统声音；需要对端启用发送";
    [left addSubview:s_audioPlay];
    s_audioStatus=ui_label(left,@"音频已关闭",NSMakeRect(18,2,360,16),10,NO);

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
    ui_label(actions,@"音频保存后立即应用；网络监听、认证和角色参数重启后生效。",NSMakeRect(18,56,560,18),11,NO);
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

    s_overviewPermissions=ui_label(card,@"正在检查键鼠权限…",NSMakeRect(22,22,580,22),11,NO);
    ui_button(card,@"检查键鼠权限",NSMakeRect(680,18,130,30),s_uiController,@selector(checkInputPermissions:));

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
    s_filePreviousButton=ui_button(card,@"上一页",NSMakeRect(18,8,78,30),s_uiController,@selector(previousFilePage:));
    s_filePageLabel=ui_label(card,@"暂无文件明细",NSMakeRect(104,12,210,22),11,NO);
    s_filePreviousButton.enabled=NO;
    s_fileNextButton=ui_button(card,@"下一页",NSMakeRect(322,8,78,30),s_uiController,@selector(nextFilePage:));
    s_fileNextButton.enabled=NO;
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
    [p setFrameSize:NSMakeSize(880,1580)];
    card=ui_card(p,NSMakeRect(20,1400,840,160));
    ui_label(card,@"键鼠权限",NSMakeRect(22,126,780,22),15,YES);
    s_inputPermissionsStatus=ui_label(card,@"正在检查…",NSMakeRect(22,98,780,22),12,NO);
    ui_label(card,@"授权后请重启 HopDesk 和后台；更新后开关已开却仍未授权时，请移除旧条目并重新添加。",
        NSMakeRect(22,70,796,22),11,NO);
    ui_button(card,@"设备控制（辅助功能）…",NSMakeRect(22,22,206,34),s_uiController,@selector(openInputPermissionSettings:)).tag=0;
    ui_button(card,@"输入监控…",NSMakeRect(244,22,140,34),s_uiController,@selector(openInputPermissionSettings:)).tag=1;
    NSView *settingsForm=[[NSView alloc] initWithFrame:NSMakeRect(0,700,880,680)];
    [p addSubview:settingsForm];
    build_settings_page(settingsForm);

    card=ui_card(p,NSMakeRect(20,240,840,430));
    ui_label(card,@"运行日志",NSMakeRect(22,390,780,22),13,YES);
    s_logNote=ui_label(card,@"尚未读取",NSMakeRect(620,390,198,22),11,NO);
    NSScrollView *logScroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(18,58,804,310)];
    logScroll.hasVerticalScroller=YES;logScroll.hasHorizontalScroller=YES;
    s_logView=[[HopDeskLogView alloc] initWithFrame:NSMakeRect(0,0,800,300)];
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
- (void)openInputPermissionSettings:(id)sender { open_input_permission_settings([sender tag] != 0); }
- (void)checkInputPermissions:(id)sender { (void)sender; mac_request_accessibility_permission(); }
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
- (void)reconnect:(id)sender {(void)sender;split_ui_reconnect();}
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
    if(!id || !split_ui_get_task(id,&task))return;
    NSString *name=utf8(task.name);
    if(!name.length)name=[NSString stringWithFormat:@"任务 #%llu",(unsigned long long)id];
    NSAlert *alert=[NSAlert new];alert.alertStyle=NSAlertStyleWarning;
    alert.messageText=[NSString stringWithFormat:@"确定取消%@？",name];
    alert.informativeText=split_ui_data_connected()?
        @"取消请求会立即发送到任务执行器；已经完成的文件不会回滚。":
        @"数据通道当前未连接。取消后，对端的临时文件可能要等连接恢复或断线超时后清理。已经完成的文件不会回滚。";
    [alert addButtonWithTitle:@"返回"];
    [alert addButtonWithTitle:@"取消任务"];
    if([alert runModal]!=NSAlertSecondButtonReturn)return;
    if(!split_ui_cancel_task(id)){
        NSAlert *failed=[NSAlert new];failed.messageText=@"任务已结束或正在完成，无法取消。";[failed runModal];
    }
    refresh_transfer_page();
}
- (void)retryFile:(id)sender {
    (void)sender;
    int index=selected_visible_file_index();
    if(index>=0 && s_fileDirection==HOP_XFER_DIR_SEND &&
       !split_ui_retry_file(s_fileBatch,index)){
        NSAlert *a=[NSAlert new];a.messageText=@"无法重试";
        a.informativeText=@"文件可能仍在进行、并非失败状态，或这是跨端粘贴任务。跨端粘贴请在目标设备确认剪贴板后重新发起粘贴。";[a runModal];
    }
}
- (void)toggleFolder:(id)sender {
    (void)sender;
    const hop_xfer_file_status *file=selected_file_status();
    if(file && file->is_directory){
        NSString *path=utf8(file->relative_path);
        if([s_collapsedFolderPaths containsObject:path]) [s_collapsedFolderPaths removeObject:path];
        else [s_collapsedFolderPaths addObject:path];
        s_filePresentationDirty=YES;
        refresh_transfer_page();
    }
}
- (void)previousFilePage:(id)sender {
    (void)sender;
    if(s_filePageIndex>0){--s_filePageIndex;refresh_transfer_page();}
}
- (void)nextFilePage:(id)sender {
    (void)sender;
    if((s_filePageIndex+1)*UI_FILE_PAGE_SIZE<s_fileTotalCount){++s_filePageIndex;refresh_transfer_page();}
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
    if (split_ui_is_active()) {
        if (s_saveBusy) return NO;
        if (s_settingsDirty) {
            NSAlert *alert = [NSAlert new];
            alert.messageText = @"设置尚未保存";
            alert.informativeText = @"关闭窗口将丢弃本次尚未保存的设置。";
            [alert addButtonWithTitle:@"返回"];
            [alert addButtonWithTitle:@"丢弃并关闭"];
            if ([alert runModal] != NSAlertSecondButtonReturn) return NO;
        }
    }
    [s_mainTimer invalidate];s_mainTimer=nil;
    /* Let AppKit finish the close first, then drop our references on the next
     * main-queue turn. Releasing inside the delegate callback is re-entrant. */
    if (!split_ui_is_active())
        dispatch_async(dispatch_get_main_queue(), ^{ main_ui_destroy(); });
    return YES;
}
@end

static void main_ui_destroy(void) {
    [s_mainTimer invalidate]; s_mainTimer=nil;
    if(s_mainWindow){
        s_mainWindow.delegate=nil;
        /* The window is retained by NSApp; detach its content view first so
         * the whole page tree is released even before the window object goes. */
        [s_mainWindow setContentView:[[NSView alloc] initWithFrame:NSZeroRect]];
        [s_mainWindow orderOut:nil];
        [s_mainWindow close];
        s_mainWindow=nil;
    }
    for(int i=0;i<UI_PAGE_COUNT;++i){s_pages[i]=nil;s_hosts[i]=nil;s_nav[i]=nil;}
    s_hostCount=0;
    s_rolePopup=nil;s_addresses=nil;
    s_pageTitle=nil;s_headerStatus=nil;s_footer=nil;
    s_overviewStatus=nil;s_overviewPeer=nil;s_overviewControl=nil;s_overviewData=nil;s_overviewSharing=nil;
    s_connectionRole=nil;s_connectionStatus=nil;s_connectionPeer=nil;s_connectionPorts=nil;
    for(int i=0;i<CFG_COUNT;++i)s_cfg[i]=nil;
    s_cfgNote=nil;s_hostLabel=nil;s_hostHint=nil;
    s_saveSettingsButton=nil;s_resetSettingsButton=nil;
    s_audioSend=nil;s_audioPlay=nil;s_audioStatus=nil;
    s_edgePopups[0]=nil;s_edgePopups[1]=nil;
    s_xferTitle=nil;s_xferDetail=nil;s_xferCounts=nil;s_xferProgress=nil;
    s_xferFiles=nil;s_logView=nil;s_taskHistory=nil;s_logNote=nil;s_filePageLabel=nil;
    s_filePreviousButton=nil;s_fileNextButton=nil;
    s_historyDirection=nil;s_historyState=nil;s_historyOrder=nil;
    s_retryButton=nil;s_cancelTaskButtons[0]=nil;s_cancelTaskButtons[1]=nil;
    s_taskTables[0]=nil;s_taskTables[1]=nil;
    s_taskRows[0]=nil;s_taskRows[1]=nil;
    s_updateVersion=nil;s_updateSource=nil;
    s_inputPermissionsStatus=nil;s_overviewPermissions=nil;
    s_uiController=nil;
    free(s_fileSnapshot);s_fileSnapshot=NULL;s_fileSnapshotCapacity=0;s_fileSnapshotCount=0;
    free(s_tasksSnapshot);s_tasksSnapshot=NULL;s_tasksSnapshotCapacity=0;s_tasksSnapshotCount=0;
    s_saveBusy=NO;s_logBusy=NO;s_settingsDirty=NO;s_populatingSettings=NO;
    s_fileSelectionValid=NO;s_refreshingTransfers=NO;s_syncTaskSelection=NO;s_fileBatchActive=NO;
    s_visibleFileCount=0;s_fileBatch=0;s_fileDirection=HOP_XFER_DIR_NONE;s_page=0;
    s_filePageIndex=0;s_filePageOffset=-1;s_fileTotalCount=0;s_fileRevisionSeen=0;s_fileSnapshotFrozen=NO;s_filePresentationDirty=NO;
    s_taskRevisionSeen=0;s_taskSendRevisionSeen=0;s_taskReceiveRevisionSeen=0;s_historyRevisionSeen=0;
    s_historyDirectionSeen=-1;s_historyStateSeen=-1;s_historyOrderSeen=-1;
    [s_collapsedFolderPaths removeAllObjects];s_collapsedFolderPaths=nil;
    memset(s_visibleFileIndices,0,sizeof(s_visibleFileIndices));
    (void)malloc_zone_pressure_relief(NULL,0);
}

static void create_main_window(void) {
    if(s_mainWindow)return;
    s_draft=g_mac.cfg;
    if (split_ui_is_active()) (void)hop_config_load(&s_draft, g_mac.config_path);
    char addresses[512];local_addresses(addresses,sizeof(addresses));s_addresses=utf8(addresses);
    NSRect frame=NSMakeRect(0,0,1140,800);
    s_mainWindow=[[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    s_mainWindow.releasedWhenClosed=NO;
    s_mainWindow.title=@"HopDesk";
    s_mainWindow.minSize=NSMakeSize(1140,620);
    s_uiController=[[HopDeskMainController alloc] init];
    s_collapsedFolderPaths=[NSMutableSet set];
    s_filePageOffset=-1;
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
    split_ui_refresh_connection(&s_connected, &s_sharing);
    refresh_connection_labels();
    if(s_page==UI_PAGE_TRANSFER)refresh_transfer_page();
}
void main_ui_shutdown(void) {
    main_ui_destroy();
}
