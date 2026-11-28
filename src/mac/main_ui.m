/* HopDesk macOS UI host: native status item, shared CUI main window. */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include "mac_app.h"
#include "split_agent.h"
#include "split_ui.h"
#include "../ui/hopdesk_ui.h"
#include "../platform/audio_link.h"
#include <cui/cui_app.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_running;
/* Main-thread owned; keep the UI process alive until a requested save and
 * its IPC completion have finished, even if its window has been closed. */
static int s_save_pending;
static int s_ui_initialized;
static int s_config_loaded;
static int s_connected;
static int s_sharing;
static hop_config s_ui_config;
static BOOL s_startupPermissionsChecked;

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


static const hop_xfer_task_snapshot *find_task(const hopdesk_ui_model *model,
                                                hop_xfer_task_id id) {
    int i;
    if (!model || !id) return NULL;
    for (i = 0; i < model->task_count; ++i)
        if (model->tasks[i].id == id) return &model->tasks[i];
    return NULL;
}

static void snapshot_model(hopdesk_ui_model *model,
                           hop_xfer_task_id selected_task,
                           uint32_t file_offset) {
    int connected = 0, sharing = 0, post = 0, listen = 0, accessibility = 0;
    int total = 0, n = 0;
    uint64_t revision = 0;
    const hop_xfer_task_snapshot *selected;
    split_ipc_transfer_state state = {0};

    if (!model) return;
    if (!s_config_loaded) {
        s_ui_config = g_mac.cfg;
        (void)hop_config_load(&s_ui_config, g_mac.config_path);
        s_config_loaded = 1;
    }
    model->config = s_ui_config;
    local_addresses(model->local_addresses, sizeof(model->local_addresses));

    if (split_ui_is_active()) {
        split_ui_refresh_connection(&connected, &sharing);
        s_connected = connected;
        s_sharing = sharing;
    }
    split_ui_display_status(model->display_status, sizeof(model->display_status));
    model->connected = s_connected;
    model->sharing = s_sharing;
    model->data_connected = split_ui_data_connected();
    model->manually_disconnected = split_ui_is_disconnected();
    {
        unsigned local_input = model->config.input_mode;
        unsigned peer_input = 0, peer_audio = 0;
        int peer_known = 0;
        char peer_name[HOP_NAME_MAX] = {0};
        split_ui_get_capabilities(&local_input, &peer_input, &peer_audio, &peer_known,
                                  peer_name, sizeof(peer_name));
        s_ui_config.input_mode = local_input;
        model->config.input_mode = local_input;
        model->peer_input_mode = peer_input;
        model->peer_audio_mode = peer_audio;
        model->peer_capabilities_known = peer_known;
        if (model->connected && peer_name[0])
            snprintf(model->peer_label, sizeof(model->peer_label), "%s", peer_name);
        else if (model->config.role == HOP_ROLE_CLIENT)
            snprintf(model->peer_label, sizeof(model->peer_label), "%s",
                model->config.peer_host[0] ? model->config.peer_host : "对端地址未配置");
        else
            snprintf(model->peer_label, sizeof(model->peer_label), "%s",
                model->connected ? "已连接的对端设备" : (model->manually_disconnected ? "已手动断开" : "等待对端连接"));
    }
    model->permissions_known = input_permissions(&post, &listen, &accessibility);
    model->permission_post = post;
    model->permission_listen = listen;
    model->permission_accessibility = accessibility;

    {
        hop_audio_status audio = {0};
        split_ui_get_audio_status(&audio);
        model->audio_requested = audio.requested;
        model->audio_active = audio.active;
        model->audio_unavailable = audio.unavailable;
    }

    if (split_ui_get_transfer_state(&state)) {
        model->transfer = state.status;
        if (!model->task_snapshot_valid || model->task_revision != state.task_revision) {
            n = split_ui_get_tasks(model->tasks, HOPDESK_UI_TASK_CAP,
                                   &total, &revision);
            model->task_count = n > 0 ? n : 0;
            model->task_total = total;
            model->task_revision = revision;
            model->task_snapshot_valid = 1;
        }
        if (!model->history_snapshot_valid || model->history_revision != state.history_revision) {
            n = split_ui_get_history(model->history, HOPDESK_UI_HISTORY_CAP,
                HOP_XFER_DIR_NONE, HOP_XFER_TASK_EMPTY, 1, &total, &revision);
            model->history_count = n > 0 ? n : 0;
            model->history_total = total;
            model->history_revision = revision;
            model->history_snapshot_valid = 1;
        }
    } else {
        memset(&model->transfer, 0, sizeof(model->transfer));
        memset(model->tasks, 0, sizeof(model->tasks));
        memset(model->history, 0, sizeof(model->history));
        model->task_count = model->task_total = 0;
        model->history_count = model->history_total = 0;
        model->task_revision = model->history_revision = 0;
        model->task_snapshot_valid = 0;
        model->history_snapshot_valid = 0;
        model->file_count = model->file_total = 0;
        model->file_batch = 0;
        model->file_direction = HOP_XFER_DIR_NONE;
        model->file_offset = file_offset;
        model->file_revision = 0;
    }

    selected = find_task(model, selected_task);
    if (!selected || !selected->transfer_id ||
        (selected->direction != HOP_XFER_DIR_SEND &&
         selected->direction != HOP_XFER_DIR_RECEIVE)) {
        model->file_count = 0;
        model->file_total = 0;
        model->file_batch = 0;
        model->file_offset = file_offset;
        model->file_revision = 0;
        model->file_direction = HOP_XFER_DIR_NONE;
        return;
    }

    {
        int file_total = 0;
        uint64_t file_revision = 0;
        (void)split_ui_get_files(selected->direction, selected->transfer_id,
            file_offset, NULL, 0, &file_total, &file_revision);
        if (model->file_batch != selected->transfer_id ||
            model->file_direction != selected->direction ||
            model->file_offset != file_offset ||
            model->file_revision != file_revision) {
            n = split_ui_get_files(selected->direction, selected->transfer_id,
                file_offset, model->files, HOPDESK_UI_FILE_PAGE,
                &file_total, &file_revision);
            model->file_count = n > 0 ? n : 0;
        }
        model->file_total = file_total;
        model->file_batch = selected->transfer_id;
        model->file_direction = selected->direction;
        model->file_offset = file_offset;
        model->file_revision = file_revision;
    }
}

static void save_config(const hop_config *config) {
    hop_config copy;
    if (!config) {
        hopdesk_ui_save_complete(0, "没有可保存的设置。");
        return;
    }
    copy = *config;
    s_save_pending = 1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int saved = 0;
        if (split_ui_is_active()) {
            saved = split_ui_save_config(&copy);
        } else if (hop_config_save_atomic(&copy, g_mac.config_path)) {
            saved = mac_apply_hot_config(&copy) ? 1 : 2;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (saved > 0) {
                s_ui_config = copy;
                if (saved == 1)
                    hopdesk_ui_save_complete(1,
                        "设置已保存。支持热生效的参数已即时应用；连接方式与端口参数重启后生效。");
                else
                    hopdesk_ui_save_complete(1,
                        "设置已保存，但后台未能应用声音能力。请确认后台运行；重启后会按已保存配置重试。");
            } else {
                hopdesk_ui_save_complete(0,
                    "保存失败；原配置未改变。请检查文件权限和磁盘空间。");
            }
            s_save_pending = 0;
        });
    });
}

static int cancel_task(hop_xfer_task_id id) {
    return split_ui_cancel_task(id);
}

static int retry_file(uint32_t batch, int index) {
    return split_ui_retry_file(batch, index);
}

static void reconnect_peer(void) {
    split_ui_reconnect();
}

static void disconnect_peer(void) {
    split_ui_disconnect();
}

static void set_input_mode(unsigned input_mode) {
    s_ui_config.input_mode = input_mode & HOP_INPUT_MODES;
    (void)split_ui_set_input_mode(s_ui_config.input_mode);
}

static void display_action(int action) {
    switch (action) {
        case HOPDESK_UI_DISPLAY_TO_MAC:
            if (split_ui_is_active()) (void)mac_cli_agent_action(NULL, HOP_CLI_ACTION_DISPLAY_MAC);
            else (void)disp_switch_target(1);
            break;
        case HOPDESK_UI_DISPLAY_TO_WINDOWS:
            if (split_ui_is_active()) (void)mac_cli_agent_action(NULL, HOP_CLI_ACTION_DISPLAY_WIN);
            else (void)disp_switch_target(0);
            break;
        case HOPDESK_UI_DISPLAY_TOGGLE:
            if (split_ui_is_active()) (void)mac_cli_agent_action(NULL, HOP_CLI_ACTION_DISPLAY_TOGGLE);
            else (void)disp_switch_target(2);
            break;
        case HOPDESK_UI_DISPLAY_WAKE:
            if (split_ui_is_active()) (void)mac_cli_agent_action(NULL, HOP_CLI_ACTION_DISPLAY_WAKE);
            else (void)disp_wake(s_ui_config.disp_wake_sec);
            break;
        case HOPDESK_UI_DISPLAY_SLEEP:
            if (split_ui_is_active()) (void)mac_cli_agent_action(NULL, HOP_CLI_ACTION_DISPLAY_SLEEP);
            else disp_sleep();
            break;
    }
}

static void check_updates(void) {
    statusbar_check_updates();
}

static int load_logs(char *out, size_t capacity) {
    FILE *file;
    long size, start;
    size_t got;
    if (!out || capacity < 2) return 0;
    out[0] = '\0';
    file = fopen(g_mac.log_path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        fclose(file);
        return 0;
    }
    start = size > (long)(capacity - 1) ? size - (long)(capacity - 1) : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    got = fread(out, 1, capacity - 1, file);
    fclose(file);
    out[got] = '\0';
    if (start > 0) {
        char *newline = strchr(out, '\n');
        if (newline) memmove(out, newline + 1, strlen(newline + 1) + 1);
    }
    return 1;
}

static int copy_text(const char *utf8) {
    if (!utf8) return 0;
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard setString:[NSString stringWithUTF8String:utf8] ?: @"" forType:NSPasteboardTypeString];
}

static void default_config(hop_config *config) {
    char name[HOP_NAME_MAX];
    if (!config) return;
    snprintf(name, sizeof(name), "%s",
        s_ui_config.device_name[0] ? s_ui_config.device_name : "Mac");
    hop_config_defaults(config);
    snprintf(config->device_name, sizeof(config->device_name), "%s", name);
}

static int s_cjk_font_fd = -1;
static void *s_cjk_font_mapping;
static size_t s_cjk_font_size;

static void release_cjk_font(void) {
    if (s_cjk_font_mapping) munmap(s_cjk_font_mapping, s_cjk_font_size);
    if (s_cjk_font_fd >= 0) close(s_cjk_font_fd);
    s_cjk_font_mapping = NULL;
    s_cjk_font_size = 0;
    s_cjk_font_fd = -1;
}

static cui_font load_cjk_font(cui_ctx *ui) {
    static const char *const candidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Supplemental/Songti.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
    };
    size_t i;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int fd = open(candidates[i], O_RDONLY);
        struct stat st;
        void *mapping;
        cui_font font;
        if (fd < 0) continue;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            close(fd);
            continue;
        }
        mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            close(fd);
            continue;
        }
        s_cjk_font_fd = fd;
        s_cjk_font_mapping = mapping;
        s_cjk_font_size = (size_t)st.st_size;
        font = cui_font_load_memory(ui, mapping, s_cjk_font_size, false);
        if (font >= 0) return font;
        release_cjk_font();
    }
    return -1;
}

static void apply_theme(int ui_theme, int persist) {
    cui_ctx *ui = cui_app_ui(NULL);
    if (ui_theme < HOP_THEME_SYSTEM || ui_theme > HOP_THEME_DARK)
        ui_theme = HOP_THEME_SYSTEM;
    if (ui_theme == HOP_THEME_SYSTEM) {
        cui_app_follow_system_theme(NULL, true);
    } else {
        cui_app_follow_system_theme(NULL, false);
        if (ui) cui_set_theme(ui, ui_theme == HOP_THEME_DARK ? CUI_THEME_DARK : CUI_THEME_LIGHT);
    }
    s_ui_config.ui_theme = ui_theme;
    if (persist) {
        hop_config copy = s_ui_config;
        s_save_pending = 1;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            (void)hop_config_save_atomic(&copy, g_mac.config_path);
            dispatch_async(dispatch_get_main_queue(), ^{
                s_save_pending = 0;
            });
        });
    }
}

static const hopdesk_ui_ops k_ui_ops = {
    snapshot_model,
    save_config,
    cancel_task,
    retry_file,
    reconnect_peer,
    disconnect_peer,
    display_action,
    check_updates,
    mac_request_accessibility_permission,
    load_logs,
    copy_text,
    default_config,
    load_cjk_font,
    release_cjk_font,
    apply_theme,
    set_input_mode
};

static void draw_frame(cui_ctx *ui, void *user) {
    (void)user;
    if (!s_running) return;
    if (!s_ui_initialized) {
        hopdesk_ui_init(ui);
        s_ui_initialized = 1;
    }
    hopdesk_ui_frame(ui);
}

void statusbar_show_main(void) {
    main_ui_show_page(HOPDESK_UI_OVERVIEW);
}

void main_ui_show_page(int page) {
    cui_app_desc desc;
    /* Defense in depth: the agent must never instantiate CUI/Metal. */
    if (split_agent_is_active()) return;
    NSApplicationActivationPolicy previousPolicy = [NSApp activationPolicy];
    if (s_running) {
        hopdesk_ui_show_page(page);
        return;
    }
    if (!s_config_loaded) {
        s_ui_config = g_mac.cfg;
        (void)hop_config_load(&s_ui_config, g_mac.config_path);
        s_config_loaded = 1;
    }
    hopdesk_ui_set_ops(&k_ui_ops);
    hopdesk_ui_show_page(page);
    memset(&desc, 0, sizeof(desc));
    desc.title = "HopDesk";
    desc.width = 1180;
    desc.height = 820;
    desc.min_width = 960;
    desc.min_height = 640;
    desc.renderer = CUI_RENDERER_GPU;
    desc.follow_system_theme = s_ui_config.ui_theme == HOP_THEME_SYSTEM;
    desc.theme = s_ui_config.ui_theme == HOP_THEME_DARK ? CUI_THEME_DARK : CUI_THEME_LIGHT;
    desc.accent = CUI_ACCENT_BLUE;
    s_running = 1;
    (void)cui_app_run(&desc, draw_frame, NULL);
    s_ui_initialized = 0;
    hopdesk_ui_shutdown();
    /* The CUI context is gone; completion callbacks must not touch it.
     * Drain the save completion before returning to the process entry point. */
    while (s_save_pending) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
    s_running = 0;
    /* CUI temporarily promotes an accessory app while its window is visible.
     * Restore the caller's policy; split UI processes entered as Regular and
     * stay Regular, while the menu-bar host returns to Accessory. */
    if ([NSApp activationPolicy] != previousPolicy)
        [NSApp setActivationPolicy:previousPolicy];
}

void main_ui_set_status(int connected, int sharing) {
    s_connected = connected != 0;
    s_sharing = sharing != 0;
    main_ui_refresh();
}

void main_ui_refresh(void) {
    if (s_running) hopdesk_ui_refresh();
}

void main_ui_shutdown(void) {
    if (s_running) cui_app_quit(NULL);
}
