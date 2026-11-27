#!/usr/bin/env python3
"""Exercise extracted production permission UI with OS/dialog doubles.
Does not change TCC settings or establish real desktop permission coverage.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
source = (ROOT / 'src/mac/main_ui.m').read_text()
start = source.index('static BOOL input_permissions')
end = source.index('static const hop_xfer_task_snapshot *find_task', start)
production = source[start:end]
fixture = r'''
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <objc/runtime.h>
#include <assert.h>
#include <stdio.h>
static BOOL s_startupPermissionsChecked;
static int localPost=1,localListen=1,localAccess=1,split,agent,available=1;
static int peerPost=1,peerListen=1,peerAccess=1,alerts,opens,requests,failOpen;
static NSModalResponse response=NSAlertThirdButtonReturn;
static NSString *lastURL;
static int split_ui_is_active(void){return split;}
static int split_agent_is_active(void){return agent;}
static int split_ui_get_input_permissions(int *p,int *l,int *a){*p=peerPost;*l=peerListen;*a=peerAccess;return available;}
static BOOL fakePost(void){return localPost;}
static BOOL fakeListen(void){return localListen;}
static BOOL fakeAccess(void){return localAccess;}
static BOOL requestAccess(CFDictionaryRef options){(void)options;requests++;return localAccess;}
static BOOL requestPost(void){requests++;return localPost;}
static BOOL requestListen(void){requests++;return localListen;}
#define CGPreflightPostEventAccess fakePost
#define CGPreflightListenEventAccess fakeListen
#define AXIsProcessTrusted fakeAccess
#define AXIsProcessTrustedWithOptions requestAccess
#define CGRequestPostEventAccess requestPost
#define CGRequestListenEventAccess requestListen
#define HOP_LOG_INFO 0
#define HOP_LOG_ERROR 1
#define hop_log(...) ((void)0)
static NSModalResponse fakeModal(id self,SEL cmd){(void)self;(void)cmd;alerts++;return response;}
static BOOL fakeOpen(id self,SEL cmd,NSURL *url){(void)self;(void)cmd;opens++;lastURL=url.absoluteString;return !failOpen;}
void mac_request_accessibility_permission(void);
@PRODUCTION@
static void drain(void){
    __block BOOL done=NO;
    dispatch_async(dispatch_get_main_queue(), ^{
        dispatch_async(dispatch_get_main_queue(), ^{done=YES;});
    });
    while(!done) CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,false);
}
int main(void){ @autoreleasepool {
    method_setImplementation(class_getInstanceMethod(NSAlert.class,@selector(runModal)),(IMP)fakeModal);
    method_setImplementation(class_getInstanceMethod(NSWorkspace.class,@selector(openURL:)),(IMP)fakeOpen);
    mac_check_input_permissions();drain();assert(alerts==0); /* no prompt when granted */
    s_startupPermissionsChecked=NO;localPost=0;
    mac_check_input_permissions();mac_check_input_permissions();drain();assert(alerts==1);
    mac_request_accessibility_permission();drain();assert(alerts==2); /* explicit action is never suppressed */
    s_startupPermissionsChecked=NO;agent=1;
    mac_check_input_permissions();mac_request_accessibility_permission();drain();assert(alerts==2);
    agent=0;split=1;localPost=localListen=localAccess=1;peerPost=0;
    s_startupPermissionsChecked=NO;mac_check_input_permissions();drain();assert(alerts==3);
    assert([input_permissions_status() containsString:@"未授权"]); /* agent, not UI process */
    available=0;assert([input_permissions_status() containsString:@"无法确认"]);
    available=1;peerPost=1;peerListen=0;
    assert([input_permissions_status() containsString:@"输入监控：未授权"]);
    failOpen=1;open_input_permission_settings(YES);
    assert(opens==2 && [lastURL hasSuffix:@"Privacy_ListenEvent"]); /* fallback keeps the chosen pane */
    assert(requests==1);
    failOpen=0;open_input_permission_settings(NO);
    assert([lastURL hasSuffix:@"Privacy_Accessibility"] && requests==3);
    split=0;
    for(int mask=0;mask<8;mask++){
        localPost=mask&1;localListen=mask&2;localAccess=mask&4;
        s_startupPermissionsChecked=NO;int before=alerts;
        mac_check_input_permissions();drain();assert(alerts==before+(mask!=7));
    }
    puts("PASS: permission startup/manual guidance, agent status and pane fallback (API doubles)");
} return 0; }
'''.replace('@PRODUCTION@', production)
with tempfile.TemporaryDirectory(prefix='hopdesk-permissions-') as directory:
    path = pathlib.Path(directory) / 'permissions.m'
    path.write_text(fixture)
    binary = pathlib.Path(directory) / 'permissions'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', str(path),
                    '-framework', 'Cocoa', '-framework', 'ApplicationServices', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
