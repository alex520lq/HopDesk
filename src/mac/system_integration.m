/* =====================================================================
 *  HopDesk - macOS Autostart (LaunchAgent) & Permission Deep-Links
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mac_app.h"
#include "../ui/hopdesk_ui.h"

static NSString *autostart_plist_path(void) {
    NSString *home = NSHomeDirectory();
    if (!home.length) return nil;
    return [home stringByAppendingPathComponent:@"Library/LaunchAgents/com.hopdesk.agent.plist"];
}

void mac_get_app_path(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    NSString *path = NSBundle.mainBundle.bundlePath;
    if (!path.length) path = NSBundle.mainBundle.executablePath;
    if (path.length) snprintf(out, cap, "%s", path.UTF8String);
}

int mac_autostart_is_enabled(void) {
    NSString *plist = autostart_plist_path();
    if (!plist.length) return 0;
    NSDictionary *dict = [NSDictionary dictionaryWithContentsOfFile:plist];
    if (![dict isKindOfClass:[NSDictionary class]]) return 0;
    NSNumber *runAtLoad = dict[@"RunAtLoad"];
    NSArray *args = dict[@"ProgramArguments"];
    return ([runAtLoad isKindOfClass:[NSNumber class]] && runAtLoad.boolValue &&
            [args isKindOfClass:[NSArray class]] && args.count > 0) ? 1 : 0;
}

int mac_autostart_set_enabled(int enabled) {
    NSString *plist = autostart_plist_path();
    if (!plist.length) return 0;

    if (!enabled) {
        if (unlink(plist.fileSystemRepresentation) == 0 || errno == ENOENT) {
            hop_log(HOP_LOG_INFO, "autostart: removed LaunchAgent plist %s", plist.UTF8String);
            return 1;
        }
        hop_log(HOP_LOG_ERROR, "autostart: failed to remove %s: %s", plist.UTF8String, strerror(errno));
        return 0;
    }

    NSString *execPath = [NSBundle.mainBundle.executablePath stringByResolvingSymlinksInPath];
    if (!execPath.length) {
        hop_log(HOP_LOG_ERROR, "autostart: unable to resolve current executable path");
        return 0;
    }

    NSString *dir = [plist stringByDeletingLastPathComponent];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&err]) {
        hop_log(HOP_LOG_ERROR, "autostart: failed to create LaunchAgents dir: %s",
                err.localizedDescription.UTF8String);
        return 0;
    }

    NSDictionary *payload = @{
        @"Label": @"com.hopdesk.agent",
        @"ProgramArguments": @[execPath, @"--agent"],
        @"RunAtLoad": @YES,
        @"KeepAlive": @{ @"SuccessfulExit": @NO },
        @"ThrottleInterval": @5
    };
    NSData *xml = [NSPropertyListSerialization dataWithPropertyList:payload
                                                             format:NSPropertyListXMLFormat_v1_0
                                                            options:0
                                                              error:&err];
    if (!xml.length) {
        hop_log(HOP_LOG_ERROR, "autostart: failed to serialize LaunchAgent plist: %s",
                err.localizedDescription.UTF8String);
        return 0;
    }

    NSString *tmpPath = [plist stringByAppendingString:@".tmp"];
    if (![xml writeToFile:tmpPath options:0 error:&err]) {
        hop_log(HOP_LOG_ERROR, "autostart: failed to write temp plist: %s",
                err.localizedDescription.UTF8String);
        return 0;
    }
    (void)chmod(tmpPath.fileSystemRepresentation, 0600);
    if (rename(tmpPath.fileSystemRepresentation, plist.fileSystemRepresentation) != 0) {
        hop_log(HOP_LOG_ERROR, "autostart: failed to commit plist: %s", strerror(errno));
        (void)unlink(tmpPath.fileSystemRepresentation);
        return 0;
    }
    hop_log(HOP_LOG_INFO, "autostart: enabled LaunchAgent plist for %s", execPath.UTF8String);
    return 1;
}

static BOOL open_privacy_pane(NSString *pane) {
    NSString *base = @"x-apple.systempreferences:com.apple.preference.security?";
    if (@available(macOS 13.0, *))
        base = @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?";
    BOOL opened = [NSWorkspace.sharedWorkspace openURL:
        [NSURL URLWithString:[base stringByAppendingString:pane]]];
    if (!opened) {
        NSString *legacy = [@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane];
        opened = [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:legacy]];
    }
    return opened;
}

int mac_permission_action(int action) {
    if (action == HOPDESK_UI_PERM_DIALOG) {
        mac_request_accessibility_permission();
        return 1;
    }
    if (action == HOPDESK_UI_PERM_ACCESSIBILITY) {
        NSDictionary *prompt = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)prompt);
        (void)CGRequestPostEventAccess();
        BOOL ok = open_privacy_pane(@"Privacy_Accessibility");
        hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR, "permission: %s Accessibility settings",
                ok ? "opened" : "failed to open");
        return ok ? 1 : 0;
    }
    if (action == HOPDESK_UI_PERM_INPUT_MONITORING) {
        (void)CGRequestListenEventAccess();
        BOOL ok = open_privacy_pane(@"Privacy_ListenEvent");
        hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR, "permission: %s Input Monitoring settings",
                ok ? "opened" : "failed to open");
        return ok ? 1 : 0;
    }
    if (action == HOPDESK_UI_PERM_SCREEN_AUDIO) {
        BOOL ok = open_privacy_pane(@"Privacy_ScreenCapture");
        hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR, "permission: %s Screen & System Audio Recording settings",
                ok ? "opened" : "failed to open");
        return ok ? 1 : 0;
    }
    return 0;
}
