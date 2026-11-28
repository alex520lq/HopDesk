#include "hopdesk_ui.h"
#include "../core/config_schema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const hopdesk_ui_ops *s_ops;
static hopdesk_ui_model s_model;
static cui_ctx *s_ctx;
static int s_page = HOPDESK_UI_OVERVIEW;
static hop_xfer_task_id s_selected_task;
static int s_selected_file = -1;
static hop_xfer_task_id s_cancel_confirm_task;
static uint32_t s_file_offset;

typedef struct {
    hop_config cfg;
    double port, data_port, audio_port, switch_delay, cooldown, margin, wake_sec;
    double wake_delay_ms, wake_settle_ms, wake_timeout_ms;
    double ddc_retries, verify_timeout_ms, poll_ms, monitor_index;
    double audio_keepalive_ms, audio_peer_timeout_ms, audio_send_limit_ms;
    double chunk_kb, rate_limit, mouse_scale, wheel_scale;
    char disp_win[16], disp_mac[16];
    int role, server_edge, client_edge, ui_theme, log_level, update_channel;
    int win_input_preset, mac_input_preset, audio_dir;
    int tab;
    bool mac_cmd_open, audio_adv_open, shared_display, sleep_fallback;
    bool provide_mouse, provide_keyboard;
    bool wheel_invert, clip_text, clip_image, clip_file;
    int loaded, dirty, saving;
    char note[256];
} hopdesk_ui_settings;

static hopdesk_ui_settings s_settings;
static int s_scroll_reset_pending;

static const char *const k_pages[HOPDESK_UI_PAGE_COUNT] = {
    "概览", "设备", "传输", "设置"
};
static const char *const k_page_subtitles[HOPDESK_UI_PAGE_COUNT] = {
    "连接状态、键鼠能力归属、文件通道和声音通道的实时概览。",
    "查看本机与对端设备、连接方式及双方提供的能力。连接方式、端口和认证参数修改后需重启生效。",
    "活动任务、文件分页明细、取消、失败文件重试和历史记录。",
    "键鼠提供方、外观与声音即时生效；连接方式、端口和认证参数保存后需重启生效。"
};
static const char *const k_connect_modes[] = {
    "等待对端连接（本机监听）", "主动连接对端（填写地址）"
};
static const char *const k_edges[] = {
    "左边缘", "右边缘", "上边缘", "下边缘"
};
static const char *const k_themes[] = {
    "跟随系统", "浅色", "深色"
};
static const char *const k_log_levels[] = {
    "Info（详细）", "Warn（警告）", "Error（仅错误）", "Off（关闭）"
};
static const char *const k_theme_icons[] = {
    CUI_ICON_MONITOR, CUI_ICON_SUN, CUI_ICON_MOON
};
static const char *const k_settings_tabs[] = {
    "连接与外观", "键鼠与显示", "声音", "文件传输", "更新与日志"
};
static const char *const k_audio_directions[] = {
    "关闭", "发送到对端", "本机播放对端"
};
static const char *const k_audio_direction_icons[] = {
    CUI_ICON_VOLUME_X, CUI_ICON_SEND, CUI_ICON_VOLUME_2
};
static const char *const k_update_channels[] = {
    "稳定版 (stable)", "开发版 (dev)"
};
static const char *const k_settings_tab_icons[] = {
    CUI_ICON_WIFI, CUI_ICON_KEYBOARD, CUI_ICON_VOLUME_2, CUI_ICON_REPEAT, CUI_ICON_TERMINAL
};

static int audio_mode_to_dir(unsigned mode) {
    if ((mode & HOP_AUDIO_SEND) && !(mode & HOP_AUDIO_PLAY)) return 1;
    if ((mode & HOP_AUDIO_PLAY) && !(mode & HOP_AUDIO_SEND)) return 2;
    if (mode & HOP_AUDIO_SEND) return 1;
    return 0;
}

static unsigned audio_dir_to_mode(int dir) {
    if (dir == 1) return HOP_AUDIO_SEND;
    if (dir == 2) return HOP_AUDIO_PLAY;
    return 0u;
}

static const char *audio_direction_summary(void) {
    unsigned req = s_model.audio_requested & HOP_AUDIO_MODES;
    unsigned act = s_model.audio_active & HOP_AUDIO_MODES;
    unsigned peer = s_model.peer_audio_mode & HOP_AUDIO_MODES;
    if (s_model.audio_unavailable) return "音频设备或权限不可用";
    if (act & HOP_AUDIO_SEND) return "本机 → 对端 正在发送";
    if (act & HOP_AUDIO_PLAY) return "对端 → 本机 正在播放";
    if (req == 0u) return "已关闭";
    if (!s_model.connected) return req == HOP_AUDIO_SEND ? "本机待发送（未连接）" : "本机待播放（未连接）";
    if (s_model.peer_capabilities_known && req == HOP_AUDIO_PLAY && peer == HOP_AUDIO_PLAY)
        return "两端均设为播放（请在一端切换）";
    if (s_model.peer_capabilities_known && req == HOP_AUDIO_SEND && peer == HOP_AUDIO_SEND)
        return "两端均设为发送（正在协调）";
    if (req == HOP_AUDIO_SEND) return "本机已开启发送（等待对端就绪）";
    if (req == HOP_AUDIO_PLAY) return "本机已开启播放（等待对端发送）";
    return "待机";
}

static cui_tone audio_direction_tone(void) {
    if (s_model.audio_unavailable) return CUI_TONE_WARNING;
    if (s_model.audio_active) return CUI_TONE_SUCCESS;
    if (s_model.connected && s_model.peer_capabilities_known &&
        ((s_model.audio_requested == HOP_AUDIO_PLAY && s_model.peer_audio_mode == HOP_AUDIO_PLAY) ||
         (s_model.audio_requested == HOP_AUDIO_SEND && s_model.peer_audio_mode == HOP_AUDIO_SEND)))
        return CUI_TONE_WARNING;
    if (s_model.audio_requested) return CUI_TONE_INFO;
    return CUI_TONE_NEUTRAL;
}

static void ui_toast(const char *title, const char *detail, cui_tone tone) {
    if (!s_ctx) return;
    cui_toast_ex(s_ctx, title, &(cui_toast_opts){
        .description = detail,
        .tone = tone,
        .duration = 4.0f
    });
}

static const char *link_status_text(void) {
    if (s_model.connected) return "已连接";
    if (s_model.manually_disconnected) return "已手动断开";
    return "未连接";
}

static cui_tone link_status_tone(void) {
    if (s_model.connected) return CUI_TONE_SUCCESS;
    if (s_model.manually_disconnected) return CUI_TONE_WARNING;
    return CUI_TONE_NEUTRAL;
}

static const char *capability_owner_text(unsigned bit) {
    if (s_model.config.input_mode & bit) return "本机提供";
    if (s_model.connected && (s_model.peer_input_mode & bit)) return "对端提供";
    return "未启用";
}

static cui_tone capability_owner_tone(unsigned bit) {
    if (s_model.config.input_mode & bit) return CUI_TONE_INFO;
    if (s_model.connected && (s_model.peer_input_mode & bit)) return CUI_TONE_SUCCESS;
    return CUI_TONE_NEUTRAL;
}

static const char *input_summary_text(void) {
    int lm = (s_model.config.input_mode & HOP_INPUT_MOUSE) != 0;
    int lk = (s_model.config.input_mode & HOP_INPUT_KEYBOARD) != 0;
    int pm = s_model.connected && (s_model.peer_input_mode & HOP_INPUT_MOUSE) != 0;
    int pk = s_model.connected && (s_model.peer_input_mode & HOP_INPUT_KEYBOARD) != 0;
    if (lm && lk) return "本机提供键鼠";
    if (pm && pk) return "对端提供键鼠";
    if (lm && pk) return "鼠本机 · 键对端";
    if (pm && lk) return "鼠对端 · 键本机";
    if (lm) return "本机提供鼠标";
    if (lk) return "本机提供键盘";
    if (pm) return "对端提供鼠标";
    if (pk) return "对端提供键盘";
    return "未启用";
}

static cui_tone input_summary_tone(void) {
    if (s_model.sharing) return CUI_TONE_INFO;
    if (s_model.config.input_mode & HOP_INPUT_MODES) return CUI_TONE_INFO;
    if (s_model.connected && (s_model.peer_input_mode & HOP_INPUT_MODES)) return CUI_TONE_SUCCESS;
    return CUI_TONE_NEUTRAL;
}

static void apply_input_mode(unsigned next_mode, const char *changed_label, int enabled) {
    next_mode &= HOP_INPUT_MODES;
    s_model.config.input_mode = next_mode;
    s_settings.cfg.input_mode = next_mode;
    s_settings.provide_mouse = (next_mode & HOP_INPUT_MOUSE) != 0;
    s_settings.provide_keyboard = (next_mode & HOP_INPUT_KEYBOARD) != 0;
    if (enabled) s_model.peer_input_mode &= ~next_mode;
    if (s_ops && s_ops->set_input_mode) {
        s_ops->set_input_mode(next_mode);
        if (changed_label) {
            if (enabled)
                ui_toast("已接管能力提供", "已切换为本机提供，对端将自动转为接收方。", CUI_TONE_INFO);
            else
                ui_toast("已关闭本机提供", changed_label, CUI_TONE_NEUTRAL);
        }
    }
}

static void open_settings_tab(int tab) {
    s_settings.tab = tab;
    hopdesk_ui_show_page(HOPDESK_UI_SETTINGS);
}

static void draw_capability_badges(cui_ctx *ui, unsigned input_mode, unsigned audio_mode, int show_empty) {
    int any = 0;
    cui_row(ui, .width = CUI_FILL, .gap = 6, .wrap = true) {
        if (input_mode & HOP_INPUT_MOUSE) {
            cui_badge_ex(ui, "提供鼠标", &(cui_badge_opts){.tone = CUI_TONE_INFO, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (input_mode & HOP_INPUT_KEYBOARD) {
            cui_badge_ex(ui, "提供键盘", &(cui_badge_opts){.tone = CUI_TONE_INFO, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (audio_mode & HOP_AUDIO_SEND) {
            cui_badge_ex(ui, "发送声音", &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (audio_mode & HOP_AUDIO_PLAY) {
            cui_badge_ex(ui, "播放声音", &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (!any && show_empty) {
            cui_badge_ex(ui, "暂未提供外设能力", &(cui_badge_opts){.tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM});
        }
    }
}

static void action_disconnect(void) {
    if (!s_ops || !s_ops->disconnect) return;
    s_ops->disconnect();
    ui_toast("已断开连接", "当前连接已切断并暂停自动重连，点击“重新连接”可恢复。", CUI_TONE_WARNING);
}

static void action_reconnect(void) {
    int was_paused;
    if (!s_ops || !s_ops->reconnect) return;
    was_paused = s_model.manually_disconnected;
    s_ops->reconnect();
    if (was_paused)
        ui_toast("正在恢复连接", "已重新启用连接监听与自动重连。", CUI_TONE_INFO);
}

static void format_bytes(uint64_t bytes, char *out, size_t cap) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else snprintf(out, cap, "%.1f %s", value, units[unit]);
}

static const char *task_state_text(int state) {
    switch (state) {
        case HOP_XFER_TASK_CREATED: return "已创建";
        case HOP_XFER_TASK_SCANNING: return "扫描中";
        case HOP_XFER_TASK_QUEUED: return "排队中";
        case HOP_XFER_TASK_ACTIVE: return "传输中";
        case HOP_XFER_TASK_COMPLETED: return "已完成";
        case HOP_XFER_TASK_PARTIAL: return "部分失败";
        case HOP_XFER_TASK_FAILED: return "失败";
        case HOP_XFER_TASK_CANCELLED: return "已取消";
        case HOP_XFER_TASK_INTERRUPTED: return "已中断";
        default: return "等待中";
    }
}

static cui_tone task_tone(int state) {
    switch (state) {
        case HOP_XFER_TASK_COMPLETED: return CUI_TONE_SUCCESS;
        case HOP_XFER_TASK_PARTIAL:
        case HOP_XFER_TASK_FAILED:
        case HOP_XFER_TASK_INTERRUPTED: return CUI_TONE_DANGER;
        case HOP_XFER_TASK_CANCELLED: return CUI_TONE_NEUTRAL;
        case HOP_XFER_TASK_ACTIVE: return CUI_TONE_INFO;
        default: return CUI_TONE_WARNING;
    }
}

static const char *file_state_text(int state) {
    switch (state) {
        case HOP_XFER_FILE_SENDING: return "发送中";
        case HOP_XFER_FILE_RECEIVING: return "接收中";
        case HOP_XFER_FILE_VERIFYING: return "校验中";
        case HOP_XFER_FILE_COMPLETED: return "完成";
        case HOP_XFER_FILE_FAILED: return "失败";
        default: return "排队中";
    }
}

static float task_progress(const hop_xfer_task_snapshot *task) {
    if (!task) return 0.0f;
    if (task->bytes_total)
        return (float)((double)task->bytes_done / (double)task->bytes_total);
    if (task->files_total)
        return (float)((double)task->files_done / (double)task->files_total);
    return task->state == HOP_XFER_TASK_COMPLETED ? 1.0f : 0.0f;
}

static const hop_xfer_task_snapshot *selected_task(void) {
    int i;
    for (i = 0; i < s_model.task_count; ++i)
        if (s_model.tasks[i].id == s_selected_task) return &s_model.tasks[i];
    if (s_model.task_count > 0) return &s_model.tasks[0];
    return NULL;
}

static const hop_xfer_file_status *selected_file(void) {
    int i;
    for (i = 0; i < s_model.file_count; ++i)
        if ((int)s_model.files[i].index == s_selected_file) return &s_model.files[i];
    return NULL;
}

static const char *update_channel_zh(int ch) {
    return ch == HOP_UPDATE_CHANNEL_DEV ? "开发版" : "稳定版";
}

static const char *current_channel_zh(void) {
    return strcmp(HOP_CHANNEL, "dev") == 0 ? "开发版" : "稳定版";
}

static const char *update_phase_text(int phase) {
    switch (phase) {
        case HOP_UPDATE_CHECKING:    return "检查中…";
        case HOP_UPDATE_UP_TO_DATE:  return "已是最新";
        case HOP_UPDATE_AVAILABLE:   return "有可用更新";
        case HOP_UPDATE_DOWNLOADING: return "下载中…";
        case HOP_UPDATE_READY:       return "待重启安装";
        case HOP_UPDATE_APPLYING:    return "正在重启安装…";
        case HOP_UPDATE_ERROR:       return "更新异常";
        default:                     return "未检查";
    }
}

static cui_tone update_phase_tone(int phase) {
    switch (phase) {
        case HOP_UPDATE_UP_TO_DATE:  return CUI_TONE_SUCCESS;
        case HOP_UPDATE_AVAILABLE:   return CUI_TONE_INFO;
        case HOP_UPDATE_DOWNLOADING: return CUI_TONE_INFO;
        case HOP_UPDATE_READY:       return CUI_TONE_SUCCESS;
        case HOP_UPDATE_APPLYING:    return CUI_TONE_WARNING;
        case HOP_UPDATE_ERROR:       return CUI_TONE_DANGER;
        default:                     return CUI_TONE_NEUTRAL;
    }
}

static void draw_stat_card(cui_ctx *ui, const char *title, const char *value,
                           const char *detail, cui_tone tone) {
    cui_card(ui, .width = CUI_GROW(1), .min_width = 190, .pad = 16, .gap = 8) {
        cui_secondary(ui, title);
        cui_heading(ui, value, 3);
        cui_badge_ex(ui, detail, &(cui_badge_opts){.tone = tone, .dot = true});
    }
}

static void draw_overview(cui_ctx *ui) {
    char peer[160], control[96], data[96], audio[96];
    snprintf(peer, sizeof(peer), "%s",
        s_model.peer_label[0] ? s_model.peer_label :
        (s_model.config.role == HOP_ROLE_CLIENT ? "尚未配置对端地址" : "等待对端连接"));
    snprintf(control, sizeof(control), "控制端口 %u", (unsigned)s_model.config.port);
    snprintf(data, sizeof(data), "数据端口 %u", (unsigned)s_model.config.data_port);
    snprintf(audio, sizeof(audio), "请求 %s · 活跃 %s",
        s_model.audio_requested ? "已配置" : "关闭",
        s_model.audio_active ? "是" : "否");

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        if (s_model.protocol_mismatch) {
            char proto_detail[192];
            snprintf(proto_detail, sizeof(proto_detail),
                "对端握手因协议版本不兼容被拒绝。本机版本 v%s（%s · 控制 v%d / 数据 v%d），请将两端更新至相同协议版本。",
                HOP_VERSION, current_channel_zh(), HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                    cui_heading(ui, "协议版本不匹配", 3);
                    cui_badge_ex(ui, "需要同步更新", &(cui_badge_opts){.tone = CUI_TONE_WARNING, .dot = true});
                }
                cui_text(ui, proto_detail);
                if (cui_button(ui, "前往检查更新", .variant = CUI_BUTTON_SECONDARY))
                    open_settings_tab(4);
            }
        }
        if (s_model.update.phase == HOP_UPDATE_READY) {
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                    cui_heading(ui, "更新已下载就绪", 3);
                    cui_badge_ex(ui, "待重启安装", &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .dot = true});
                }
                cui_text(ui, s_model.update.message[0] ? s_model.update.message : "已通过 SHA-256 校验，可随时重启安装。");
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, "重启并安装更新", .variant = CUI_BUTTON_PRIMARY,
                        .disabled = !s_ops || !s_ops->update_action)) {
                        if (s_ops && s_ops->update_action)
                            s_ops->update_action(HOP_UPDATE_ACTION_APPLY, s_model.update.target_channel);
                    }
                    if (cui_button(ui, "查看更新详情", .variant = CUI_BUTTON_SECONDARY))
                        open_settings_tab(4);
                }
            }
        }
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            draw_stat_card(ui, "控制连接",
                link_status_text(),
                peer, link_status_tone());
            draw_stat_card(ui, "文件通道",
                s_model.data_connected ? "已连接" : "未连接",
                data, s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL);
            draw_stat_card(ui, "键鼠提供方",
                input_summary_text(),
                s_model.sharing ? "正在控制对端" : (s_model.connected ? "已同步能力" : "等待连接"),
                input_summary_tone());
            draw_stat_card(ui, "声音共享",
                audio_direction_summary(),
                audio, audio_direction_tone());
        }

        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "本机与对端", 3);
                cui_badge_ex(ui,
                    s_model.connected ? "在线" : (s_model.manually_disconnected ? "已手动断开" : "离线"),
                    &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 18, .wrap = true, .align = CUI_ALIGN_START) {
                cui_column(ui, .width = CUI_GROW(1), .gap = 6) {
                    cui_muted(ui, "本机设备");
                    cui_label(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk");
                    cui_secondary(ui, s_model.config.role == HOP_ROLE_SERVER
                        ? "连接方式：等待对端连接（本机监听）" : "连接方式：主动连接对端");
                    cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : "未获取到本机地址");
                    draw_capability_badges(ui, s_model.config.input_mode, s_model.audio_requested, 1);
                }
                cui_column(ui, .width = CUI_GROW(1), .gap = 6) {
                    cui_muted(ui, "对端设备");
                    cui_label(ui, peer);
                    cui_secondary(ui, control);
                    cui_text(ui, data);
                    if (s_model.connected && s_model.peer_capabilities_known)
                        draw_capability_badges(ui, s_model.peer_input_mode, s_model.peer_audio_mode, 1);
                    else
                        cui_muted(ui, s_model.connected ? "正在同步对端能力…" : "连接后自动同步对端提供的能力");
                }
            }
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button(ui, "打开传输管理", .variant = CUI_BUTTON_PRIMARY))
                    hopdesk_ui_show_page(HOPDESK_UI_TRANSFER);
                if (cui_button_ex(ui, s_model.manually_disconnected ? "恢复连接" : "重新连接", &(cui_button_opts){
                        .variant = s_model.manually_disconnected ? CUI_BUTTON_SECONDARY : CUI_BUTTON_OUTLINE,
                        .icon = s_model.manually_disconnected ? CUI_ICON_LINK : CUI_ICON_REFRESH_CW,
                        .disabled = !s_ops || !s_ops->reconnect
                    }))
                    action_reconnect();
                if (cui_button_ex(ui, "断开连接", &(cui_button_opts){
                        .variant = CUI_BUTTON_DANGER,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = s_model.manually_disconnected || !s_ops || !s_ops->disconnect
                    }))
                    action_disconnect();
                if (cui_button_ex(ui, "一键切换显示器", &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_MONITOR,
                        .disabled = !s_ops || !s_ops->display_action
                    }))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TOGGLE);
                if (cui_button(ui, "显示器交给 Mac", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_MAC);
                if (cui_button(ui, "显示器交给 Windows", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_WINDOWS);
                if (cui_button(ui, "唤醒显示器", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_WAKE);
            }
        }

        if (s_model.display_status[0]) cui_text(ui, s_model.display_status);
        if (s_model.permissions_known) {
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_heading(ui, "键鼠权限", 3);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    cui_badge_ex(ui, s_model.permission_post ? "设备控制已授权" : "设备控制未授权",
                        &(cui_badge_opts){.tone = s_model.permission_post ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                    cui_badge_ex(ui, s_model.permission_listen ? "输入监控已授权" : "输入监控未授权",
                        &(cui_badge_opts){.tone = s_model.permission_listen ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                    cui_badge_ex(ui, s_model.permission_accessibility ? "辅助功能已授权" : "辅助功能未授权",
                        &(cui_badge_opts){.tone = s_model.permission_accessibility ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                }
                if (!s_model.permission_post || !s_model.permission_listen || !s_model.permission_accessibility) {
                    if (cui_button(ui, "检查键鼠权限", .variant = CUI_BUTTON_SECONDARY) &&
                        s_ops && s_ops->request_permissions)
                        s_ops->request_permissions();
                }
            }
        }
    }
}

static void draw_device(cui_ctx *ui) {
    char ports[128], connect_mode[64], dimensions[96], mouse_badge[64], kbd_badge[64];
    snprintf(ports, sizeof(ports), "控制 %u  ·  数据 %u",
        (unsigned)s_model.config.port, (unsigned)s_model.config.data_port);
    snprintf(connect_mode, sizeof(connect_mode), "%s",
        s_model.config.role == HOP_ROLE_SERVER ? "等待对端连接（本机监听）" : "主动连接对端（填写地址）");
    snprintf(dimensions, sizeof(dimensions), "%d × %d",
        s_model.config.screen_w, s_model.config.screen_h);
    snprintf(mouse_badge, sizeof(mouse_badge), "鼠标：%s", capability_owner_text(HOP_INPUT_MOUSE));
    snprintf(kbd_badge, sizeof(kbd_badge), "键盘：%s", capability_owner_text(HOP_INPUT_KEYBOARD));

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, "本机设备");
                cui_heading(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk", 3);
                cui_secondary(ui, connect_mode);
                cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : "未获取到本机地址");
                cui_secondary(ui, dimensions);
                draw_capability_badges(ui, s_model.config.input_mode, s_model.audio_requested, 1);
            }
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, "对端设备");
                cui_heading(ui, s_model.peer_label[0] ? s_model.peer_label : "尚未连接", 3);
                cui_secondary(ui, s_model.connected ? "控制连接已建立" :
                    (s_model.manually_disconnected ? "已手动断开（暂停自动重连）" : "等待连接"));
                cui_text(ui, ports);
                if (s_model.config.role == HOP_ROLE_CLIENT)
                    cui_text(ui, s_model.config.peer_host[0] ? s_model.config.peer_host : "尚未配置对端地址");
                else
                    cui_text(ui, s_model.manually_disconnected ? "本机监听已暂停，点击恢复连接即可继续" : "本机正在等待对端连入");
                if (s_model.connected && s_model.peer_capabilities_known)
                    draw_capability_badges(ui, s_model.peer_input_mode, s_model.peer_audio_mode, 1);
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 10) {
            cui_heading(ui, "连接与键鼠能力", 3);
            cui_text(ui, "两端对等协作，鼠标、键盘与声音均可按需指定由哪一端提供，开启后自动同步对端。");
            cui_row(ui, .width = CUI_FILL, .gap = 10, .wrap = true) {
                cui_badge_ex(ui,
                    s_model.connected ? "控制通道在线" :
                    (s_model.manually_disconnected ? "控制通道已手动断开" : "控制通道离线"),
                    &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
                cui_badge_ex(ui, s_model.data_connected ? "数据通道在线" : "数据通道离线",
                    &(cui_badge_opts){.tone = s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
                cui_badge_ex(ui, mouse_badge,
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_MOUSE)});
                cui_badge_ex(ui, kbd_badge,
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_KEYBOARD)});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, s_model.manually_disconnected ? "恢复连接" : "重新连接", &(cui_button_opts){
                        .variant = CUI_BUTTON_PRIMARY,
                        .icon = s_model.manually_disconnected ? CUI_ICON_LINK : CUI_ICON_REFRESH_CW,
                        .disabled = !s_ops || !s_ops->reconnect
                    }))
                    action_reconnect();
                if (cui_button_ex(ui, "断开连接", &(cui_button_opts){
                        .variant = CUI_BUTTON_DANGER,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = s_model.manually_disconnected || !s_ops || !s_ops->disconnect
                    }))
                    action_disconnect();
                if (cui_button(ui, "配置键鼠能力", .variant = CUI_BUTTON_SECONDARY))
                    open_settings_tab(1);
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 8) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_heading(ui, "声音共享", 3);
                cui_badge_ex(ui, audio_direction_summary(),
                    &(cui_badge_opts){.tone = audio_direction_tone(), .dot = true});
            }
            cui_text(ui, "声音流向为单向三选一（关闭 / 发送到对端 / 本机播放对端），切换后两端自动协同。");
            if (cui_button(ui, "配置声音设置", .variant = CUI_BUTTON_SECONDARY))
                open_settings_tab(2);
        }
    }
}

static void draw_transfer_summary(cui_ctx *ui, const hop_xfer_status *status, const char *title) {
    char bytes[64], counts[96];
    format_bytes(status->bytes_done, bytes, sizeof(bytes));
    snprintf(counts, sizeof(counts), "文件 %u / %u  ·  已传输 %s",
        status->files_done, status->files_total, bytes);
    cui_card(ui, .width = CUI_GROW(1), .min_width = 250, .pad = 14, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
            cui_label(ui, title);
            cui_badge_ex(ui, status->active ? "进行中" : "空闲",
                &(cui_badge_opts){.tone = status->active ? CUI_TONE_INFO : CUI_TONE_NEUTRAL, .dot = true});
        }
        cui_text(ui, status->name[0] ? status->name : "暂无任务");
        cui_secondary(ui, status->detail[0] ? status->detail : "等待传输");
        cui_progress_ex(ui, status->bytes_total ?
            (float)((double)status->bytes_done / (double)status->bytes_total) : 0.0f,
            &(cui_progress_opts){.width = CUI_FILL, .height = 7, .show_value = true});
        cui_muted(ui, counts);
    }
}

static void draw_transfer(cui_ctx *ui) {
    const cui_table_column task_columns[] = {
        {"任务", CUI_GROW(2), CUI_TEXT_START, false},
        {"方向", CUI_PX(72), CUI_TEXT_START, false},
        {"状态", CUI_PX(92), CUI_TEXT_START, false},
        {"进度", CUI_PX(170), CUI_TEXT_START, false}
    };
    const cui_table_column file_columns[] = {
        {"文件", CUI_GROW(2), CUI_TEXT_START, false},
        {"类型", CUI_PX(70), CUI_TEXT_START, false},
        {"状态", CUI_PX(82), CUI_TEXT_START, false},
        {"进度", CUI_PX(150), CUI_TEXT_START, false}
    };
    const cui_table_column history_columns[] = {
        {"任务", CUI_GROW(2), CUI_TEXT_START, false},
        {"方向", CUI_PX(70), CUI_TEXT_START, false},
        {"结果", CUI_PX(92), CUI_TEXT_START, false},
        {"文件", CUI_PX(90), CUI_TEXT_START, false},
        {"数据量", CUI_PX(100), CUI_TEXT_END, false}
    };
    int i;
    const hop_xfer_task_snapshot *chosen = selected_task();

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            draw_transfer_summary(ui, &s_model.transfer.send, "发送");
            draw_transfer_summary(ui, &s_model.transfer.receive, "接收");
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "活动任务", 3);
                cui_muted(ui, s_model.task_total > s_model.task_count ?
                    "仅显示最近 32 条活动任务" : "实时任务");
            }
            if (s_model.task_count == 0) {
                cui_secondary(ui, "当前没有活动任务。");
            } else if (cui_table_begin_ex(ui, "hopdesk-active-tasks", task_columns, 4,
                       &(cui_table_opts){.height = CUI_PX(250), .striped = true, .compact = true, .selectable = true})) {
                for (i = 0; i < s_model.task_count; ++i) {
                    const hop_xfer_task_snapshot *task = &s_model.tasks[i];
                    char progress[32];
                    snprintf(progress, sizeof(progress), "%.0f%%", task_progress(task) * 100.0f);
                    if (cui_table_row(ui, task->id == s_selected_task)) {
                        s_selected_task = task->id;
                        s_cancel_confirm_task = 0;
                        s_file_offset = 0;
                        s_selected_file = -1;
                    }
                    cui_table_cell_text(ui, task->name[0] ? task->name : "文件任务");
                    cui_table_cell_text(ui, task->direction == HOP_XFER_DIR_SEND ? "发送" : "接收");
                    cui_table_cell(ui);
                    cui_badge_ex(ui, task_state_text(task->state),
                        &(cui_badge_opts){.tone = task_tone(task->state), .size = CUI_SIZE_SM});
                    cui_table_cell(ui);
                    cui_progress_ex(ui, task_progress(task),
                        &(cui_progress_opts){.width = CUI_FILL, .height = 5});
                    (void)progress;
                }
                cui_table_end(ui);
            }
            cui_column(ui, .width = CUI_FILL, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, "取消选中任务", .variant = CUI_BUTTON_DANGER,
                        .disabled = !chosen || chosen->state >= HOP_XFER_TASK_COMPLETED ||
                                    chosen->cancel_requested || !s_ops || !s_ops->cancel_task))
                        s_cancel_confirm_task = chosen ? chosen->id : 0;
                    if (chosen)
                        cui_muted(ui, chosen->detail[0] ? chosen->detail : task_state_text(chosen->state));
                }
                if (s_cancel_confirm_task) {
                    if (!chosen || chosen->id != s_cancel_confirm_task ||
                        chosen->state >= HOP_XFER_TASK_COMPLETED || chosen->cancel_requested) {
                        s_cancel_confirm_task = 0;
                    } else {
                        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                            cui_text(ui, "确认取消这个传输任务？");
                            /* Keep the safe action first in keyboard/focus order. */
                            if (cui_button(ui, "保留任务", .variant = CUI_BUTTON_SECONDARY))
                                s_cancel_confirm_task = 0;
                            if (cui_button(ui, "确认取消", .variant = CUI_BUTTON_DANGER)) {
                                if (s_ops && s_ops->cancel_task &&
                                    s_ops->cancel_task(s_cancel_confirm_task))
                                    ui_toast("已请求取消", "任务会在安全的生命周期节点停止。", CUI_TONE_INFO);
                                else
                                    ui_toast("无法取消", "任务可能已经结束或正在提交结果。", CUI_TONE_WARNING);
                                s_cancel_confirm_task = 0;
                            }
                        }
                    }
                }
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "文件明细", 3);
                cui_muted(ui, s_model.file_total > 0 ?
                    "按页加载，不读取完整文件清单" : "选择一个活动任务查看文件");
            }
            if (s_model.file_total > 0) {
                cui_text(ui, chosen && chosen->name[0] ? chosen->name :
                    (s_model.file_direction == HOP_XFER_DIR_SEND ? "发送任务" : "接收任务"));
                if (cui_table_begin_ex(ui, "hopdesk-transfer-files", file_columns, 4,
                    &(cui_table_opts){.height = CUI_PX(300), .striped = true, .compact = true, .selectable = true})) {
                    for (i = 0; i < s_model.file_count; ++i) {
                        const hop_xfer_file_status *file = &s_model.files[i];
                        char percent[32];
                        float ratio = file->bytes_total ?
                            (float)((double)file->bytes_done / (double)file->bytes_total) :
                            (file->phase == HOP_XFER_FILE_COMPLETED ? 1.0f : 0.0f);
                        snprintf(percent, sizeof(percent), "%.0f%%", ratio * 100.0f);
                        if (cui_table_row(ui, (int)file->index == s_selected_file))
                            s_selected_file = (int)file->index;
                        cui_table_cell_text(ui, file->relative_path[0] ? file->relative_path : "(未知文件)");
                        cui_table_cell_text(ui, file->is_directory ? "目录" : "文件");
                        cui_table_cell_text(ui, file_state_text(file->phase));
                        cui_table_cell(ui);
                        cui_progress_ex(ui, ratio, &(cui_progress_opts){.width = CUI_FILL, .height = 5});
                        (void)percent;
                    }
                    cui_table_end(ui);
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, "上一页", .disabled = s_file_offset == 0)) {
                        s_file_offset = s_file_offset >= HOPDESK_UI_FILE_PAGE ?
                            s_file_offset - HOPDESK_UI_FILE_PAGE : 0;
                        s_selected_file = -1;
                    }
                    if (cui_button(ui, "下一页", .disabled =
                        s_file_offset + HOPDESK_UI_FILE_PAGE >= (uint32_t)s_model.file_total)) {
                        s_file_offset += HOPDESK_UI_FILE_PAGE;
                        s_selected_file = -1;
                    }
                    cui_muted(ui, "第 ");
                    cui_textf(ui, "%u", (unsigned)(s_file_offset / HOPDESK_UI_FILE_PAGE + 1));
                    cui_muted(ui, " 页");
                    if (cui_button(ui, "重试选中文件", .variant = CUI_BUTTON_SECONDARY,
                        .disabled = !selected_file() || selected_file()->phase != HOP_XFER_FILE_FAILED ||
                                    s_model.file_direction != HOP_XFER_DIR_SEND ||
                                    !s_ops || !s_ops->retry_file)) {
                        if (selected_file() && s_ops && s_ops->retry_file &&
                            s_ops->retry_file(s_model.file_batch, s_selected_file))
                            ui_toast("已提交重试", "失败文件已重新加入发送队列。", CUI_TONE_INFO);
                        else
                            ui_toast("无法重试", "仅支持已结束的普通发送任务中的失败文件。", CUI_TONE_WARNING);
                    }
                }
            } else {
                cui_secondary(ui, "选择一个活动任务后，这里会按 24 条一页显示文件状态。");
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "历史记录", 3);
                cui_muted(ui, s_model.history_total > s_model.history_count ?
                    "显示最近 24 条" : "已完成与终止的任务");
            }
            if (s_model.history_count == 0) {
                cui_secondary(ui, "暂无历史任务。");
            } else if (cui_table_begin_ex(ui, "hopdesk-transfer-history", history_columns, 5,
                       &(cui_table_opts){.height = CUI_PX(220), .striped = true, .compact = true})) {
                for (i = 0; i < s_model.history_count; ++i) {
                    const hop_xfer_task_history_record *entry = &s_model.history[i];
                    char files[48], bytes[64];
                    snprintf(files, sizeof(files), "%u / %u", entry->files_done, entry->files_total);
                    format_bytes(entry->bytes_total, bytes, sizeof(bytes));
                    (void)cui_table_row(ui, false);
                    cui_table_cell_text(ui, entry->name[0] ? entry->name : "文件任务");
                    cui_table_cell_text(ui, entry->direction == HOP_XFER_DIR_SEND ? "发送" : "接收");
                    cui_table_cell(ui);
                    cui_badge_ex(ui, task_state_text(entry->state),
                        &(cui_badge_opts){.tone = task_tone(entry->state), .size = CUI_SIZE_SM});
                    cui_table_cell_text(ui, files);
                    cui_table_cell_text(ui, bytes);
                }
                cui_table_end(ui);
            }
        }
    }
}

static int parse_u16(const char *text, uint16_t *out) {
    char *end = NULL;
    unsigned long value;
    if (!text || !text[0] || !out) return 0;
    value = strtoul(text, &end, 0);
    if (end == text || !end || *end || value > 65535ul) return 0;
    *out = (uint16_t)value;
    return 1;
}

static void apply_ui_theme(cui_ctx *ui, int ui_theme, int persist) {
    if (ui_theme < HOP_THEME_SYSTEM || ui_theme > HOP_THEME_DARK)
        ui_theme = HOP_THEME_SYSTEM;
    s_model.config.ui_theme = ui_theme;
    s_settings.cfg.ui_theme = ui_theme;
    s_settings.ui_theme = ui_theme;
    if (s_ops && s_ops->apply_theme) {
        s_ops->apply_theme(ui_theme, persist);
    } else if (ui && ui_theme != HOP_THEME_SYSTEM) {
        cui_set_theme(ui, ui_theme == HOP_THEME_DARK ? CUI_THEME_DARK : CUI_THEME_LIGHT);
    }
}

static void settings_load(void) {
    s_settings.cfg = s_model.config;
    s_settings.port = s_model.config.port;
    s_settings.data_port = s_model.config.data_port;
    s_settings.audio_port = s_model.config.audio_port ? s_model.config.audio_port : 24819;
    s_settings.switch_delay = s_model.config.switch_delay_ms;
    s_settings.cooldown = s_model.config.switch_cooldown_ms;
    s_settings.margin = s_model.config.edge_margin_px;
    s_settings.mouse_scale = s_model.config.mouse_scale_pct ? s_model.config.mouse_scale_pct : 100;
    s_settings.wheel_scale = s_model.config.wheel_scale_pct ? s_model.config.wheel_scale_pct : 100;
    s_settings.wheel_invert = s_model.config.wheel_invert != 0;
    s_settings.clip_text = s_model.config.clip_text != 0;
    s_settings.clip_image = s_model.config.clip_image != 0;
    s_settings.clip_file = s_model.config.clip_file != 0;
    s_settings.wake_sec = s_model.config.disp_wake_sec;
    s_settings.wake_delay_ms = s_model.config.disp_wake_delay_ms;
    s_settings.wake_settle_ms = s_model.config.disp_wake_settle_ms;
    s_settings.wake_timeout_ms = s_model.config.disp_wake_timeout_ms;
    s_settings.ddc_retries = s_model.config.disp_ddc_retries ? s_model.config.disp_ddc_retries : 3;
    s_settings.verify_timeout_ms = s_model.config.disp_verify_timeout_ms ? s_model.config.disp_verify_timeout_ms : 8000;
    s_settings.poll_ms = s_model.config.disp_poll_ms ? s_model.config.disp_poll_ms : 200;
    s_settings.monitor_index = s_model.config.disp_monitor_index;
    s_settings.audio_keepalive_ms = s_model.config.audio_keepalive_ms ? s_model.config.audio_keepalive_ms : 1000;
    s_settings.audio_peer_timeout_ms = s_model.config.audio_peer_timeout_ms ? s_model.config.audio_peer_timeout_ms : 3000;
    s_settings.audio_send_limit_ms = s_model.config.audio_send_limit_ms ? s_model.config.audio_send_limit_ms : 120;
    s_settings.shared_display = s_model.config.disp_shared != 0;
    s_settings.sleep_fallback = s_model.config.disp_sleep_fallback != 0;
    s_settings.chunk_kb = s_model.config.xfer_chunk_kb;
    s_settings.rate_limit = s_model.config.xfer_rate_limit_mib;
    snprintf(s_settings.disp_win, sizeof(s_settings.disp_win), "0x%02X", (unsigned)s_model.config.disp_win_input);
    snprintf(s_settings.disp_mac, sizeof(s_settings.disp_mac), "0x%02X", (unsigned)s_model.config.disp_mac_input);
    {
        int pcount = (int)hop_display_input_preset_count();
        int wp = hop_display_input_find_preset(s_model.config.disp_win_input);
        int mp = hop_display_input_find_preset(s_model.config.disp_mac_input);
        s_settings.win_input_preset = wp >= 0 ? wp : pcount;
        s_settings.mac_input_preset = mp >= 0 ? mp : pcount;
    }
    s_settings.role = (int)s_model.config.role;
    s_settings.server_edge = s_model.config.server_edge;
    s_settings.client_edge = s_model.config.client_edge;
    s_settings.ui_theme = (s_model.config.ui_theme >= HOP_THEME_SYSTEM &&
                           s_model.config.ui_theme <= HOP_THEME_DARK)
        ? s_model.config.ui_theme : HOP_THEME_SYSTEM;
    s_settings.log_level = (s_model.config.log_level >= HOP_LOG_INFO &&
                            s_model.config.log_level <= HOP_LOG_OFF)
        ? s_model.config.log_level : HOP_LOG_INFO;
    s_settings.update_channel = (s_model.config.update_channel == HOP_UPDATE_CHANNEL_DEV)
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
    s_settings.provide_mouse = (s_model.config.input_mode & HOP_INPUT_MOUSE) != 0;
    s_settings.provide_keyboard = (s_model.config.input_mode & HOP_INPUT_KEYBOARD) != 0;
    s_settings.audio_dir = audio_mode_to_dir(s_model.config.audio_mode);
    s_settings.loaded = 1;
}

static void settings_mark_dirty(void) {
    s_settings.dirty = 1;
    if (!s_settings.saving) snprintf(s_settings.note, sizeof(s_settings.note), "%s", "设置尚未保存");
}

static void settings_reset(void) {
    if (s_ops && s_ops->default_config) s_ops->default_config(&s_settings.cfg);
    else hop_config_defaults(&s_settings.cfg);
    s_model.config = s_settings.cfg;
    s_settings.loaded = 0;
    settings_load();
    apply_ui_theme(s_ctx, s_settings.ui_theme, 0);
    settings_mark_dirty();
}

static int settings_build_config(hop_config *out) {
    uint16_t win_input, mac_input;
    char err[256] = {0};
    if (!out) return 0;
    *out = s_settings.cfg;
    if (s_settings.port < 1 || s_settings.port > 65535 ||
        s_settings.data_port < 1 || s_settings.data_port > 65535 ||
        s_settings.audio_port < 1 || s_settings.audio_port > 65535) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "控制、数据和音频端口必须在 1–65535 之间。");
        return 0;
    }
    if (!parse_u16(s_settings.disp_win, &win_input) ||
        !parse_u16(s_settings.disp_mac, &mac_input)) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "显示器输入源必须是 0–65535 的十进制或 0x 十六进制值。");
        return 0;
    }

    out->role = (hop_role)s_settings.role;
    out->port = (uint16_t)s_settings.port;
    out->data_port = (uint16_t)s_settings.data_port;
    out->audio_port = (uint16_t)s_settings.audio_port;
    out->server_edge = s_settings.server_edge;
    out->client_edge = s_settings.client_edge;
    out->switch_delay_ms = (int)s_settings.switch_delay;
    out->switch_cooldown_ms = (int)s_settings.cooldown;
    out->edge_margin_px = (int)s_settings.margin;
    out->mouse_scale_pct = (int)s_settings.mouse_scale;
    out->wheel_scale_pct = (int)s_settings.wheel_scale;
    out->wheel_invert = s_settings.wheel_invert ? 1 : 0;
    out->clip_text = s_settings.clip_text ? 1 : 0;
    out->clip_image = s_settings.clip_image ? 1 : 0;
    out->clip_file = s_settings.clip_file ? 1 : 0;
    out->disp_win_input = win_input;
    out->disp_mac_input = mac_input;
    out->disp_wake_sec = (int)s_settings.wake_sec;
    out->disp_wake_delay_ms = (int)s_settings.wake_delay_ms;
    out->disp_wake_settle_ms = (int)s_settings.wake_settle_ms;
    out->disp_wake_timeout_ms = (int)s_settings.wake_timeout_ms;
    out->disp_ddc_retries = (int)s_settings.ddc_retries;
    out->disp_verify_timeout_ms = (int)s_settings.verify_timeout_ms;
    out->disp_poll_ms = (int)s_settings.poll_ms;
    out->disp_monitor_index = (int)s_settings.monitor_index;
    out->disp_shared = s_settings.shared_display ? 1 : 0;
    out->disp_sleep_fallback = s_settings.sleep_fallback ? 1 : 0;
    out->xfer_chunk_kb = (int)s_settings.chunk_kb;
    out->xfer_rate_limit_mib = (int)s_settings.rate_limit;
    out->input_mode = (s_settings.provide_mouse ? HOP_INPUT_MOUSE : 0u) |
                      (s_settings.provide_keyboard ? HOP_INPUT_KEYBOARD : 0u);
    out->audio_mode = audio_dir_to_mode(s_settings.audio_dir);
    out->audio_keepalive_ms = (int)s_settings.audio_keepalive_ms;
    out->audio_peer_timeout_ms = (int)s_settings.audio_peer_timeout_ms;
    out->audio_send_limit_ms = (int)s_settings.audio_send_limit_ms;
    out->ui_theme = (s_settings.ui_theme >= HOP_THEME_SYSTEM &&
                     s_settings.ui_theme <= HOP_THEME_DARK)
        ? s_settings.ui_theme : HOP_THEME_SYSTEM;
    out->log_level = (s_settings.log_level >= HOP_LOG_INFO &&
                      s_settings.log_level <= HOP_LOG_OFF)
        ? s_settings.log_level : HOP_LOG_INFO;
    out->update_channel = s_settings.update_channel == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;

    if (!hop_config_validate(out, 1, err, sizeof(err))) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", err[0] ? err : "配置校验失败。");
        return 0;
    }
    return 1;
}

static void settings_save(void) {
    hop_config config;
    if (s_settings.saving) return;
    if (!settings_build_config(&config)) {
        ui_toast("设置无效", s_settings.note, CUI_TONE_WARNING);
        return;
    }
    s_settings.cfg = config;
    s_settings.saving = 1;
    snprintf(s_settings.note, sizeof(s_settings.note), "%s", "正在保存设置…");
    if (s_ops && s_ops->save_config_async) s_ops->save_config_async(&config);
    else hopdesk_ui_save_complete(0, "当前平台没有配置保存服务。");
}

static void draw_settings_bar(cui_ctx *ui) {
    if (!s_settings.loaded) settings_load();
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S) && s_settings.dirty && !s_settings.saving)
        settings_save();

    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10, .wrap = true) {
        if (cui_tabs_ex(ui, "hopdesk-settings-tabs", k_settings_tabs, 5, &s_settings.tab,
                &(cui_tabs_opts){.variant = CUI_TABS_PILL, .icons = k_settings_tab_icons})) {
            s_scroll_reset_pending = 1;
        }
        cui_spacer(ui);
        if (s_settings.dirty && !s_settings.saving) {
            cui_badge_ex(ui, "未保存", &(cui_badge_opts){.tone = CUI_TONE_WARNING, .dot = true});
        }
        if (cui_button_ex(ui, "恢复默认", &(cui_button_opts){
                .variant = CUI_BUTTON_OUTLINE,
                .size = CUI_SIZE_SM,
                .icon = CUI_ICON_ROTATE_CCW,
                .disabled = s_settings.saving
            })) {
            settings_reset();
        }
        if (cui_button_ex(ui, s_settings.saving ? "保存中…" : "保存设置", &(cui_button_opts){
                .variant = CUI_BUTTON_PRIMARY,
                .size = CUI_SIZE_SM,
                .icon = CUI_ICON_SAVE,
                .loading = s_settings.saving,
                .disabled = !s_settings.dirty || s_settings.saving,
                .shortcut = "Ctrl S"
            })) {
            settings_save();
        }
    }
    if (s_settings.note[0]) {
        cui_muted(ui, s_settings.note);
    }
}

static void draw_settings_connection_and_appearance(cui_ctx *ui) {
    cui_row(ui, .width = CUI_FILL, .gap = 14, .wrap = true, .align = CUI_ALIGN_START) {
        cui_card(ui, .width = CUI_GROW(3), .min_width = 360, .pad = 16, .gap = 12) {
            cui_heading(ui, "设备与连接", 3);
            cui_secondary(ui, "连接方式仅决定哪一端监听端口、哪一端主动发起连接，不限定键鼠或声音由谁提供。");
            cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_START) {
                if (cui_input(ui, "setting-device-name", s_settings.cfg.device_name,
                    sizeof(s_settings.cfg.device_name), .label = "设备名称",
                    .placeholder = "例如：Mac / Windows", .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
                if (cui_select_ex(ui, "setting-connect-mode", k_connect_modes, 2, &s_settings.role,
                    &(cui_select_opts){.label = "连接方式", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            if (cui_input(ui, "setting-peer-host", s_settings.cfg.peer_host,
                sizeof(s_settings.cfg.peer_host), .label = "对端地址",
                .placeholder = "主动连接对端时填写对方的 IP 或主机名",
                .disabled = s_settings.role != HOP_ROLE_CLIENT, .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
            cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-control-port", &s_settings.port, 1, 65535,
                    &(cui_number_opts){.label = "控制端口", .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-data-port", &s_settings.data_port, 1, 65535,
                    &(cui_number_opts){.label = "数据端口", .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-audio-port", &s_settings.audio_port, 1, 65535,
                    &(cui_number_opts){.label = "音频端口", .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            if (cui_input(ui, "setting-token", s_settings.cfg.token,
                sizeof(s_settings.cfg.token), .label = "共享 Token", .password = true,
                .placeholder = "两台设备必须一致", .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_label(ui, "当前连接控制");
                cui_badge_ex(ui, link_status_text(),
                    &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
            }
            cui_secondary(ui, s_model.manually_disconnected
                ? "连接已手动切断并暂停自动重连；点击“恢复连接”即可重新建立连接。"
                : "需要临时切断键鼠、剪贴板和文件传输通道时，可立即断开并暂停自动重连。");
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (s_model.manually_disconnected) {
                    if (cui_button_ex(ui, "恢复连接", &(cui_button_opts){
                            .variant = CUI_BUTTON_PRIMARY,
                            .icon = CUI_ICON_LINK,
                            .disabled = !s_ops || !s_ops->reconnect
                        }))
                        action_reconnect();
                    (void)cui_button_ex(ui, "已断开", &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = true
                    });
                } else {
                    if (cui_button_ex(ui, "断开连接", &(cui_button_opts){
                            .variant = CUI_BUTTON_DANGER,
                            .icon = CUI_ICON_PAUSE,
                            .disabled = !s_ops || !s_ops->disconnect
                        }))
                        action_disconnect();
                    if (cui_button_ex(ui, "重新连接", &(cui_button_opts){
                            .variant = CUI_BUTTON_SECONDARY,
                            .icon = CUI_ICON_REFRESH_CW,
                            .disabled = !s_ops || !s_ops->reconnect
                        }))
                        action_reconnect();
                }
            }
        }

        cui_column(ui, .width = CUI_GROW(2), .min_width = 300, .gap = 14) {
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                cui_heading(ui, "外观主题", 3);
                cui_secondary(ui, "选择颜色模式，修改会立即应用并保存。");
                if (cui_segmented_ex(ui, "setting-ui-theme", k_themes, 3, &s_settings.ui_theme,
                        &(cui_segmented_opts){.full_width = true, .icons = k_theme_icons})) {
                    apply_ui_theme(ui, s_settings.ui_theme, 1);
                }
            }
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                cui_heading(ui, "快速入口", 3);
                cui_secondary(ui, "声音共享已独立为「声音」标签页，支持三选一单向流向与双端自动联动。");
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button_ex(ui, "前往声音设置", &(cui_button_opts){
                            .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM, .icon = CUI_ICON_VOLUME_2 }))
                        s_settings.tab = 2;
                    if (cui_button_ex(ui, "前往键鼠与显示", &(cui_button_opts){
                            .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM, .icon = CUI_ICON_MONITOR }))
                        s_settings.tab = 1;
                }
            }
        }
    }
}

static void apply_audio_direction(int dir) {
    unsigned next = audio_dir_to_mode(dir);
    s_settings.audio_dir = dir;
    s_settings.cfg.audio_mode = next;
    s_model.config.audio_mode = next;
    if (s_ops && s_ops->set_audio_mode) {
        s_ops->set_audio_mode(next);
        if (next == HOP_AUDIO_SEND) {
            ui_toast("声音流向已切换",
                s_model.connected ? "本机开始发送系统声音，对端已自动切换为接收播放。"
                                  : "已设为本机发送系统声音，连接建立后生效。",
                CUI_TONE_SUCCESS);
        } else if (next == HOP_AUDIO_PLAY) {
            ui_toast("声音流向已切换",
                s_model.connected ? "本机开始播放对端声音，对端已自动切换为发送声音。"
                                  : "已设为本机播放对端声音，连接建立后生效。",
                CUI_TONE_SUCCESS);
        } else {
            ui_toast("声音共享已关闭",
                s_model.connected ? "本机与对端均已关闭声音转发。" : "已关闭本机声音转发。",
                CUI_TONE_INFO);
        }
    } else {
        settings_mark_dirty();
    }
}

static void draw_settings_audio(cui_ctx *ui) {
    const char *peer_audio_text = "对端状态未知";
    cui_tone peer_audio_tone = CUI_TONE_NEUTRAL;
    if (s_model.connected && s_model.peer_capabilities_known) {
        if (s_model.peer_audio_mode == HOP_AUDIO_SEND) {
            peer_audio_text = "对端：发送声音";
            peer_audio_tone = CUI_TONE_INFO;
        } else if (s_model.peer_audio_mode == HOP_AUDIO_PLAY) {
            peer_audio_text = "对端：接收播放";
            peer_audio_tone = CUI_TONE_INFO;
        } else if (s_model.peer_audio_mode == HOP_AUDIO_MODES) {
            peer_audio_text = "对端：发送 + 播放（旧版）";
            peer_audio_tone = CUI_TONE_WARNING;
        } else {
            peer_audio_text = "对端：已关闭";
            peer_audio_tone = CUI_TONE_NEUTRAL;
        }
    } else if (!s_model.connected) {
        peer_audio_text = "对端：未连接";
    }

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_heading(ui, "声音共享流向", 3);
                cui_badge_ex(ui, audio_direction_summary(),
                    &(cui_badge_opts){.tone = audio_direction_tone(), .dot = true});
            }
            cui_secondary(ui, "选择声音传输方向后即时生效并自动保存；已连接时对端会自动切换到互补角色，避免两边同时发送或回声循环。");
            if (cui_segmented_ex(ui, "setting-audio-direction", k_audio_directions, 3,
                    &s_settings.audio_dir,
                    &(cui_segmented_opts){.full_width = true, .icons = k_audio_direction_icons})) {
                apply_audio_direction(s_settings.audio_dir);
            }

            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                cui_badge_ex(ui,
                    s_settings.audio_dir == 1 ? "本机：发送到对端" :
                    (s_settings.audio_dir == 2 ? "本机：播放对端声音" : "本机：关闭"),
                    &(cui_badge_opts){.tone = s_settings.audio_dir ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
                cui_badge_ex(ui, peer_audio_text, &(cui_badge_opts){.tone = peer_audio_tone});
                cui_badge_ex(ui,
                    s_model.audio_active ? "音频通道运行中" : "音频通道空闲",
                    &(cui_badge_opts){.tone = s_model.audio_active ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL});
                if (s_model.audio_unavailable) {
                    cui_badge_ex(ui, "音频设备或权限不可用", &(cui_badge_opts){.tone = CUI_TONE_WARNING});
                }
            }

            cui_separator(ui);
#ifdef _WIN32
            cui_muted(ui, "Windows 使用系统进程回环采集系统声音（自动排除 HopDesk 自身播放），端口配置位于「连接与外观」。");
#else
            cui_muted(ui, "macOS 发送本机系统声音需要 macOS 14.2+ 并在「系统设置 → 隐私与安全性 → 录屏与系统录音」中允许 HopDesk；仅播放对端声音无需额外权限。");
#endif
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            if (cui_accordion_begin(ui, "高级音频参数（心跳间隔 / 断流超时 / 发送阻塞上限）", &s_settings.audio_adv_open)) {
                cui_secondary(ui, "修改后点击顶部「保存设置」生效（断流超时需大于心跳间隔）。");
                cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                    if (cui_number_ex(ui, "setting-audio-keepalive", &s_settings.audio_keepalive_ms, 200, 10000,
                        &(cui_number_opts){.label = "音频心跳间隔", .step = 100, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-audio-timeout", &s_settings.audio_peer_timeout_ms, 500, 30000,
                        &(cui_number_opts){.label = "音频断流超时", .step = 200, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-audio-send-limit", &s_settings.audio_send_limit_ms, 20, 2000,
                        &(cui_number_opts){.label = "发送阻塞上限", .step = 20, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                }
                cui_accordion_end(ui);
            }
        }
    }
}

static void apply_detected_input_source(int for_windows, uint16_t code) {
    int pcount = (int)hop_display_input_preset_count();
    int preset = hop_display_input_find_preset(code);
    char label[64];
    hop_display_input_format_label(code, label, sizeof(label));
    if (for_windows) {
        snprintf(s_settings.disp_win, sizeof(s_settings.disp_win), "0x%02X", (unsigned)code);
        s_settings.win_input_preset = preset >= 0 ? preset : pcount;
        ui_toast("已填入 Windows 输入源", label, CUI_TONE_SUCCESS);
    } else {
        snprintf(s_settings.disp_mac, sizeof(s_settings.disp_mac), "0x%02X", (unsigned)code);
        s_settings.mac_input_preset = preset >= 0 ? preset : pcount;
        ui_toast("已填入 Mac 输入源", label, CUI_TONE_SUCCESS);
    }
    settings_mark_dirty();
}

static void draw_display_settings_card(cui_ctx *ui) {
    const hop_display_inventory *inv = &s_model.display_inventory;
    int pcount = (int)hop_display_input_preset_count();
    const char *input_opts[16];
    int opt_count = 0;
    for (int i = 0; i < pcount && opt_count < 15; ++i) {
        const hop_display_input_preset *p = hop_display_input_preset_at((size_t)i);
        if (p) input_opts[opt_count++] = p->label;
    }
    input_opts[opt_count++] = "自定义输入源…";
    int custom_idx = opt_count - 1;
    if (s_settings.win_input_preset < 0 || s_settings.win_input_preset > custom_idx)
        s_settings.win_input_preset = custom_idx;
    if (s_settings.mac_input_preset < 0 || s_settings.mac_input_preset > custom_idx)
        s_settings.mac_input_preset = custom_idx;

    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_heading(ui, "显示器切换", 3);
#ifdef _WIN32
            cui_badge_ex(ui, s_settings.shared_display ? "共屏切换已开启" : "普通切源模式",
                &(cui_badge_opts){.tone = s_settings.shared_display ? CUI_TONE_INFO : CUI_TONE_NEUTRAL, .dot = true});
#else
            cui_badge_ex(ui, inv->ddc_tool_ready ? "m1ddc 已就绪" : "未检测到 m1ddc",
                &(cui_badge_opts){.tone = inv->ddc_tool_ready ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .dot = true});
#endif
        }
        cui_secondary(ui, "点击“识别显示器”可在各屏幕悬浮显示 3 秒编号与型号；点击“探测输入源”可自动读取当前画面对应的显示器接口。");

        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            if (cui_button_ex(ui, "识别显示器", &(cui_button_opts){
                    .variant = CUI_BUTTON_SECONDARY,
                    .icon = CUI_ICON_MONITOR,
                    .disabled = !s_ops || !s_ops->display_action
                })) {
                if (s_ops && s_ops->display_action)
                    s_ops->display_action(HOPDESK_UI_DISPLAY_IDENTIFY);
            }
            if (cui_button_ex(ui, inv->probing ? "探测中…" : "探测显示器与输入源", &(cui_button_opts){
                    .variant = CUI_BUTTON_SECONDARY,
                    .icon = CUI_ICON_REFRESH_CW,
                    .loading = inv->probing != 0,
                    .disabled = inv->probing != 0 || !s_ops || !s_ops->display_action
                })) {
                if (s_ops && s_ops->display_action)
                    s_ops->display_action(HOPDESK_UI_DISPLAY_PROBE);
            }
#ifndef _WIN32
            if (!inv->ddc_tool_ready) {
                if (cui_button_ex(ui, "复制 m1ddc 安装命令", &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .icon = CUI_ICON_COPY,
                        .disabled = !s_ops || !s_ops->copy_text
                    }) && s_ops && s_ops->copy_text) {
                    if (s_ops->copy_text("brew install m1ddc"))
                        ui_toast("已复制安装命令", "在终端运行 brew install m1ddc 后再点击“探测显示器与输入源”。", CUI_TONE_INFO);
                }
            }
#endif
        }
        if (inv->probe_note[0]) cui_muted(ui, inv->probe_note);

        if (inv->count > 0) {
            cui_column(ui, .width = CUI_FILL, .gap = 6) {
                for (int i = 0; i < inv->count && i < HOP_DISPLAY_MAX_DEVICES; ++i) {
                    char mon_label[192], row_id[40];
                    hop_display_format_monitor_label(&inv->items[i], mon_label, sizeof(mon_label));
                    snprintf(row_id, sizeof(row_id), "##disp-inv-%d", i);
                    cui_push_id(ui, row_id);
                    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 8) {
                        cui_text(ui, mon_label);
                        if (inv->items[i].current_input >= 0 && inv->items[i].current_input <= 65535) {
#ifdef _WIN32
                            if (cui_button_ex(ui, "设为 Windows 输入源", &(cui_button_opts){
                                    .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM })) {
                                apply_detected_input_source(1, (uint16_t)inv->items[i].current_input);
                            }
#else
                            if (cui_button_ex(ui, "设为 Mac 输入源", &(cui_button_opts){
                                    .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM })) {
                                apply_detected_input_source(0, (uint16_t)inv->items[i].current_input);
                            }
#endif
                        }
                    }
                    cui_pop_id(ui);
                }
            }
        }

        cui_separator(ui);

#ifdef _WIN32
        if (cui_switch(ui, "Windows 与 Mac 共用一块外接显示器", &s_settings.shared_display,
            .description = "切到 Mac 时自动释放所选扩展屏，切回 Windows 时恢复扩展屏与输入源（主屏不可选）"))
            settings_mark_dirty();
        if (s_settings.shared_display) {
            char mon_bufs[HOP_DISPLAY_MAX_DEVICES + 1][192];
            char mon_devs[HOP_DISPLAY_MAX_DEVICES + 1][32];
            const char *mon_ptrs[HOP_DISPLAY_MAX_DEVICES + 1];
            int mon_count = 0;
            int mon_sel = 0;
            int matched_saved = 0;

            for (int i = 0; i < inv->count && i < HOP_DISPLAY_MAX_DEVICES; ++i) {
                if (inv->items[i].is_primary) continue;
                hop_display_format_monitor_label(&inv->items[i], mon_bufs[mon_count], sizeof(mon_bufs[mon_count]));
                snprintf(mon_devs[mon_count], sizeof(mon_devs[mon_count]), "%s", inv->items[i].device_id);
                mon_ptrs[mon_count] = mon_bufs[mon_count];
                if (strcmp(mon_devs[mon_count], s_settings.cfg.disp_device) == 0) {
                    mon_sel = mon_count;
                    matched_saved = 1;
                }
                ++mon_count;
            }
            if (s_settings.cfg.disp_device[0] && !matched_saved) {
                snprintf(mon_bufs[mon_count], sizeof(mon_bufs[mon_count]),
                         "已保存的扩展屏（%s · 当前未连接或已切至 Mac）", s_settings.cfg.disp_device);
                snprintf(mon_devs[mon_count], sizeof(mon_devs[mon_count]), "%s", s_settings.cfg.disp_device);
                mon_ptrs[mon_count] = mon_bufs[mon_count];
                mon_sel = mon_count;
                ++mon_count;
            }
            if (mon_count > 0) {
                if (!s_settings.cfg.disp_device[0]) {
                    snprintf(s_settings.cfg.disp_device, sizeof(s_settings.cfg.disp_device),
                             "%s", mon_devs[0]);
                }
                if (cui_select_ex(ui, "setting-shared-monitor", mon_ptrs, mon_count, &mon_sel,
                    &(cui_select_opts){.label = "选择共用外接显示器", .width = CUI_FILL})) {
                    snprintf(s_settings.cfg.disp_device, sizeof(s_settings.cfg.disp_device),
                             "%s", mon_devs[mon_sel]);
                    settings_mark_dirty();
                }
            } else {
                cui_secondary(ui, "暂未检测到可选择的扩展屏。请先在 Windows 按 Win+P 选择「扩展」启用共用屏，再点击上方「探测显示器与输入源」。");
            }
        }
#else
        {
            char mac_bufs[HOP_DISPLAY_MAX_DEVICES + 2][192];
            int mac_indices[HOP_DISPLAY_MAX_DEVICES + 2];
            const char *mac_ptrs[HOP_DISPLAY_MAX_DEVICES + 2];
            int mac_count = 0;
            int parsed_idx = hop_display_mac_parse_input_cmd_index(s_settings.cfg.mac_input_cmd);
            int mac_sel = -1;

            for (int i = 0; i < inv->count && i < HOP_DISPLAY_MAX_DEVICES; ++i) {
                if (inv->items[i].is_builtin) continue;
                int m1_idx = inv->items[i].device_id[0] ? atoi(inv->items[i].device_id) : 0;
                if (m1_idx < 1 || m1_idx > 16) m1_idx = mac_count + 1;
                hop_display_format_monitor_label(&inv->items[i], mac_bufs[mac_count], sizeof(mac_bufs[mac_count]));
                mac_indices[mac_count] = m1_idx;
                mac_ptrs[mac_count] = mac_bufs[mac_count];
                if (parsed_idx > 0 && mac_indices[mac_count] == parsed_idx) mac_sel = mac_count;
                ++mac_count;
            }
            if (mac_count == 0) {
                for (int n = 1; n <= 4; ++n) {
                    snprintf(mac_bufs[mac_count], sizeof(mac_bufs[mac_count]), "外接显示器 %d（m1ddc display %d）", n, n);
                    mac_indices[mac_count] = n;
                    mac_ptrs[mac_count] = mac_bufs[mac_count];
                    if (parsed_idx == n) mac_sel = mac_count;
                    ++mac_count;
                }
            }
            snprintf(mac_bufs[mac_count], sizeof(mac_bufs[mac_count]), "%s", "自定义切源命令（在下方高级参数编辑）");
            mac_indices[mac_count] = 0;
            mac_ptrs[mac_count] = mac_bufs[mac_count];
            if (mac_sel < 0) mac_sel = mac_count;
            ++mac_count;

            if (cui_select_ex(ui, "setting-mac-target-display", mac_ptrs, mac_count, &mac_sel,
                &(cui_select_opts){.label = "Mac 目标外接显示器", .width = CUI_FILL})) {
                if (mac_indices[mac_sel] > 0) {
                    hop_display_mac_build_input_cmd(mac_indices[mac_sel],
                        s_settings.cfg.mac_input_cmd, sizeof(s_settings.cfg.mac_input_cmd));
                    settings_mark_dirty();
                } else {
                    s_settings.mac_cmd_open = true;
                }
            }
            cui_muted(ui, "共屏拓扑由 Windows 端统一协调；概览页切屏按钮会优先通知 Windows，断线时自动回落本机切源。");
        }
#endif

        cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
            if (cui_select_ex(ui, "setting-disp-win-preset", input_opts, opt_count, &s_settings.win_input_preset,
                &(cui_select_opts){.label = "Windows 端输入源", .width = CUI_GROW(1)})) {
                if (s_settings.win_input_preset < pcount) {
                    const hop_display_input_preset *p = hop_display_input_preset_at((size_t)s_settings.win_input_preset);
                    if (p) snprintf(s_settings.disp_win, sizeof(s_settings.disp_win), "0x%02X", (unsigned)p->code);
                }
                settings_mark_dirty();
            }
            if (cui_select_ex(ui, "setting-disp-mac-preset", input_opts, opt_count, &s_settings.mac_input_preset,
                &(cui_select_opts){.label = "Mac 端输入源", .width = CUI_GROW(1)})) {
                if (s_settings.mac_input_preset < pcount) {
                    const hop_display_input_preset *p = hop_display_input_preset_at((size_t)s_settings.mac_input_preset);
                    if (p) snprintf(s_settings.disp_mac, sizeof(s_settings.disp_mac), "0x%02X", (unsigned)p->code);
                }
                settings_mark_dirty();
            }
        }
        if (s_settings.win_input_preset == custom_idx || s_settings.mac_input_preset == custom_idx) {
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (s_settings.win_input_preset == custom_idx) {
                    if (cui_input(ui, "setting-disp-win", s_settings.disp_win, sizeof(s_settings.disp_win),
                        .label = "自定义 Windows 输入源代码（十进制或 0x 十六进制）",
                        .placeholder = "例如 0x0F", .width = CUI_GROW(1)) & CUI_CHANGED)
                        settings_mark_dirty();
                }
                if (s_settings.mac_input_preset == custom_idx) {
                    if (cui_input(ui, "setting-disp-mac", s_settings.disp_mac, sizeof(s_settings.disp_mac),
                        .label = "自定义 Mac 输入源代码（十进制或 0x 十六进制）",
                        .placeholder = "例如 0x11", .width = CUI_GROW(1)) & CUI_CHANGED)
                        settings_mark_dirty();
                }
            }
        }

        if (cui_accordion_begin(ui, "高级切换参数（唤醒时长 / 重试超时 / 手动设备名与命令）", &s_settings.mac_cmd_open)) {
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-wake-sec", &s_settings.wake_sec, 1, 3600,
                    &(cui_number_opts){.label = "显示器唤醒时长", .step = 60, .decimals = 0, .suffix = "秒", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-ddc-retries", &s_settings.ddc_retries, 1, 10,
                    &(cui_number_opts){.label = "DDC 写入重试", .step = 1, .decimals = 0, .suffix = "次", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-verify-timeout", &s_settings.verify_timeout_ms, 1000, 30000,
                    &(cui_number_opts){.label = "回读/拓扑验证超时", .step = 500, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-disp-poll", &s_settings.poll_ms, 50, 2000,
                    &(cui_number_opts){.label = "验证轮询间隔", .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wake-settle", &s_settings.wake_settle_ms, 0, 10000,
                    &(cui_number_opts){.label = "共屏唤醒确认后延迟", .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wake-timeout", &s_settings.wake_timeout_ms, 1000, 30000,
                    &(cui_number_opts){.label = "对端应答超时", .step = 500, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-wake-delay", &s_settings.wake_delay_ms, 0, 10000,
                    &(cui_number_opts){.label = "普通模式唤醒延迟", .step = 100, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
#ifdef _WIN32
                if (cui_number_ex(ui, "setting-disp-monitor-index", &s_settings.monitor_index, 0, 15,
                    &(cui_number_opts){.label = "普通模式 DDC 序号（0=主屏）", .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
#endif
            }
#ifdef _WIN32
            if (cui_switch(ui, "切回 Windows 失败时允许休眠 Mac 显示器兜底", &s_settings.sleep_fallback,
                .description = "仅当 Windows DDC 与 Mac 切源命令都失败时，向 Mac 发送睡眠命令断掉信号强制回落"))
                settings_mark_dirty();
            if (cui_input(ui, "setting-shared-device", s_settings.cfg.disp_device,
                sizeof(s_settings.cfg.disp_device), .label = "手动指定 Windows 设备名（高级覆盖）",
                .placeholder = "例如 \\\\.\\DISPLAY2", .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
#endif
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_input(ui, "setting-mac-input-cmd", s_settings.cfg.mac_input_cmd,
                    sizeof(s_settings.cfg.mac_input_cmd), .label = "Mac 切源命令",
                    .placeholder = "例如 m1ddc display 1 set input", .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
                if (cui_input(ui, "setting-mac-sleep-cmd", s_settings.cfg.mac_sleep_cmd,
                    sizeof(s_settings.cfg.mac_sleep_cmd), .label = "Mac 睡眠命令",
                    .placeholder = "例如 pmset displaysleepnow", .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
            }
            cui_accordion_end(ui);
        }
    }
}

static void draw_settings_input_and_display(cui_ctx *ui) {
    int peer_mouse = s_model.connected && (s_model.peer_input_mode & HOP_INPUT_MOUSE) != 0;
    int peer_keyboard = s_model.connected && (s_model.peer_input_mode & HOP_INPUT_KEYBOARD) != 0;

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_heading(ui, "键鼠能力提供方", 3);
                cui_badge_ex(ui, input_summary_text(),
                    &(cui_badge_opts){.tone = input_summary_tone(), .dot = true});
            }
            cui_secondary(ui, "鼠标与键盘可各自选择由哪一端提供。开启本机提供后立即生效，对端会自动切换为“对端提供”。");
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 12) {
                cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                    if (cui_switch(ui, "本机提供鼠标", &s_settings.provide_mouse,
                        .description = peer_mouse
                            ? "当前由对端提供鼠标；开启后本机接管并自动关闭对端提供"
                            : "本机光标滑过切出边缘后控制对端设备")) {
                        unsigned next = (s_settings.provide_mouse ? HOP_INPUT_MOUSE : 0u) |
                                        (s_settings.provide_keyboard ? HOP_INPUT_KEYBOARD : 0u);
                        apply_input_mode(next, "已关闭本机鼠标提供。", s_settings.provide_mouse);
                    }
                }
                cui_badge_ex(ui, capability_owner_text(HOP_INPUT_MOUSE),
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_MOUSE), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 12) {
                cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                    if (cui_switch(ui, "本机提供键盘", &s_settings.provide_keyboard,
                        .description = peer_keyboard
                            ? "当前由对端提供键盘；开启后本机接管并自动关闭对端提供"
                            : "将本机键盘输入转发到对端（仅提供键盘时可按 Ctrl+Alt+Esc / Ctrl+Option+Esc 切换）")) {
                        unsigned next = (s_settings.provide_mouse ? HOP_INPUT_MOUSE : 0u) |
                                        (s_settings.provide_keyboard ? HOP_INPUT_KEYBOARD : 0u);
                        apply_input_mode(next, "已关闭本机键盘提供。", s_settings.provide_keyboard);
                    }
                }
                cui_badge_ex(ui, capability_owner_text(HOP_INPUT_KEYBOARD),
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_KEYBOARD), .dot = true});
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_heading(ui, "边缘移交", 3);
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_select_ex(ui, "setting-server-edge", k_edges, 4, &s_settings.server_edge,
                    &(cui_select_opts){.label = "切出到对端的边缘", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_select_ex(ui, "setting-client-edge", k_edges, 4, &s_settings.client_edge,
                    &(cui_select_opts){.label = "从对端返回的边缘", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-switch-delay", &s_settings.switch_delay, 0, 5000,
                    &(cui_number_opts){.label = "边缘触发延迟", .step = 10, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-cooldown", &s_settings.cooldown, 0, 10000,
                    &(cui_number_opts){.label = "切换冷却", .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-margin", &s_settings.margin, 0, 1000,
                    &(cui_number_opts){.label = "边缘内缩", .step = 1, .decimals = 0, .suffix = "px", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-mouse-scale", &s_settings.mouse_scale, 25, 400,
                    &(cui_number_opts){.label = "鼠标速度倍率", .step = 5, .decimals = 0, .suffix = "%", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wheel-scale", &s_settings.wheel_scale, 25, 400,
                    &(cui_number_opts){.label = "滚轮速度倍率", .step = 5, .decimals = 0, .suffix = "%", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            if (cui_switch(ui, "反转滚轮方向", &s_settings.wheel_invert,
                .description = "对注入到对端的滚动事件反转方向（适配自然滚动习惯）"))
                settings_mark_dirty();
        }

        draw_display_settings_card(ui);
    }
}

static void draw_settings_transfer(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_heading(ui, "文件传输调优", 3);
            cui_secondary(ui, "默认 256 KB 分块可在千兆局域网下兼顾吞吐与键鼠低尾延迟。更多高级预算与超时参数可通过 hopdesk config CLI 编辑。");
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-chunk-kb", &s_settings.chunk_kb, 64, 4096,
                    &(cui_number_opts){.label = "传输分块大小", .step = 64, .decimals = 0, .suffix = "KB", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-rate-limit", &s_settings.rate_limit, 16, 4096,
                    &(cui_number_opts){.label = "速率上限", .step = 16, .decimals = 0, .suffix = "MiB/s", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_heading(ui, "剪贴板同步策略", 3);
            cui_secondary(ui, "按类型独立开关双向剪贴板同步，保存后即时生效。");
            if (cui_switch(ui, "同步文本剪贴板", &s_settings.clip_text,
                .description = "通过控制通道同步纯文本")) settings_mark_dirty();
            if (cui_switch(ui, "同步图片剪贴板", &s_settings.clip_image,
                .description = "通过数据通道同步截图与位图")) settings_mark_dirty();
            if (cui_switch(ui, "同步文件剪贴板", &s_settings.clip_file,
                .description = "按需发送文件 Offer 并在粘贴时传输")) settings_mark_dirty();
        }
    }
}

static void draw_settings_maintenance(cui_ctx *ui) {
    const hop_update_snapshot *up = &s_model.update;
    int busy = up->phase == HOP_UPDATE_CHECKING ||
               up->phase == HOP_UPDATE_DOWNLOADING ||
               up->phase == HOP_UPDATE_APPLYING;
    int can_download = !busy && up->target_version[0] &&
                       (up->phase == HOP_UPDATE_AVAILABLE ||
                        (up->phase == HOP_UPDATE_ERROR && up->target_tag[0]));
    int can_apply = up->phase == HOP_UPDATE_READY;
    char current_info[192], target_info[224], dl_detail[128];

    snprintf(current_info, sizeof(current_info),
        "当前运行：v%s（%s）  ·  控制协议 v%d / 数据协议 v%d",
        HOP_VERSION, current_channel_zh(), HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_heading(ui, "版本与双通道更新", 3);
                cui_badge_ex(ui, update_phase_text(up->phase),
                    &(cui_badge_opts){.tone = update_phase_tone(up->phase), .dot = true});
            }
            cui_text(ui, current_info);
            cui_secondary(ui, "稳定版与开发版完全独立、互不影响。可随时在下方切换目标通道（支持开发版回切至稳定版，或稳定版切入开发版）。");
            if (cui_segmented_ex(ui, "setting-update-channel", k_update_channels, 2,
                    &s_settings.update_channel, &(cui_segmented_opts){.full_width = true})) {
                settings_mark_dirty();
            }

            if (up->target_version[0]) {
                if (up->target_ctrl_proto > 0 && up->target_xfer_proto > 0) {
                    snprintf(target_info, sizeof(target_info),
                        "目标版本：v%s（%s）  ·  控制协议 v%d / 数据协议 v%d",
                        up->target_version, update_channel_zh(up->target_channel),
                        up->target_ctrl_proto, up->target_xfer_proto);
                } else {
                    snprintf(target_info, sizeof(target_info),
                        "目标版本：v%s（%s）",
                        up->target_version, update_channel_zh(up->target_channel));
                }
                cui_separator(ui);
                cui_text(ui, target_info);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (up->is_channel_switch)
                        cui_badge_ex(ui, "跨通道切换", &(cui_badge_opts){.tone = CUI_TONE_INFO});
                    if (up->protocol_changed)
                        cui_badge_ex(ui, "协议版本有变化 · 建议两端同步更新", &(cui_badge_opts){.tone = CUI_TONE_WARNING});
                }
                if (up->summary[0]) cui_secondary(ui, up->summary);
            }

            if (up->message[0]) cui_text(ui, up->message);

            if (up->phase == HOP_UPDATE_DOWNLOADING || up->phase == HOP_UPDATE_READY) {
                float ratio = up->bytes_total > 0
                    ? (float)((double)up->bytes_done / (double)up->bytes_total)
                    : (up->phase == HOP_UPDATE_READY ? 1.0f : 0.0f);
                char done_str[48], total_str[48];
                format_bytes(up->bytes_done, done_str, sizeof(done_str));
                if (up->bytes_total > 0) {
                    format_bytes(up->bytes_total, total_str, sizeof(total_str));
                    snprintf(dl_detail, sizeof(dl_detail), "已下载 %s / %s", done_str, total_str);
                } else {
                    snprintf(dl_detail, sizeof(dl_detail), "已下载 %s", done_str);
                }
                cui_progress_ex(ui, ratio, &(cui_progress_opts){.width = CUI_FILL, .height = 7, .show_value = true});
                cui_muted(ui, dl_detail);
            }

            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_CHECKING ? "检查中…" : "检查更新", &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_REFRESH_CW,
                        .loading = up->phase == HOP_UPDATE_CHECKING,
                        .disabled = busy || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_CHECK, s_settings.update_channel);
                }
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_DOWNLOADING ? "下载中…" : "下载更新", &(cui_button_opts){
                        .variant = can_download ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                        .loading = up->phase == HOP_UPDATE_DOWNLOADING,
                        .disabled = !can_download || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_DOWNLOAD, up->target_channel);
                }
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_APPLYING ? "正在重启…" : "重启并安装更新", &(cui_button_opts){
                        .variant = CUI_BUTTON_PRIMARY,
                        .loading = up->phase == HOP_UPDATE_APPLYING,
                        .disabled = !can_apply || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_APPLY, up->target_channel);
                }
                if (up->notes_url[0]) {
                    if (cui_button_ex(ui, "复制更新日志链接", &(cui_button_opts){
                            .variant = CUI_BUTTON_OUTLINE,
                            .icon = CUI_ICON_COPY,
                            .disabled = !s_ops || !s_ops->copy_text
                        }) && s_ops && s_ops->copy_text) {
                        if (s_ops->copy_text(up->notes_url))
                            ui_toast("链接已复制", "已复制 GitHub Release 发布页链接，可在浏览器打开查看完整更新日志。", CUI_TONE_SUCCESS);
                    }
                }
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_heading(ui, "日志等级与权限", 3);
            if (cui_select_ex(ui, "setting-log-level", k_log_levels, 4, &s_settings.log_level,
                &(cui_select_opts){.label = "日志记录等级", .width = CUI_FILL}))
                settings_mark_dirty();
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, "检查键鼠权限", &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_SHIELD,
                        .disabled = !s_ops || !s_ops->request_permissions
                    }))
                    if (s_ops && s_ops->request_permissions) s_ops->request_permissions();
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "运行日志", 3);
                cui_row(ui, .gap = 8) {
                    if (cui_button_ex(ui, "刷新日志", &(cui_button_opts){
                            .variant = CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_REFRESH_CW
                        }) && s_ops && s_ops->load_logs) {
                        if (s_ops->load_logs(s_model.logs, sizeof(s_model.logs)))
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", "日志已更新");
                        else
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", "读取日志失败");
                        cui_request_redraw(ui);
                    }
                    if (cui_button_ex(ui, "复制日志", &(cui_button_opts){
                            .variant = CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_COPY,
                            .disabled = !s_model.logs[0]
                        }) && s_ops && s_ops->copy_text && s_model.logs[0]) {
                        if (s_ops->copy_text(s_model.logs))
                            ui_toast("日志已复制", "日志内容已放入系统剪贴板。", CUI_TONE_SUCCESS);
                        else
                            ui_toast("复制失败", "无法写入系统剪贴板。", CUI_TONE_WARNING);
                    }
                }
            }
            if (s_model.log_note[0]) cui_muted(ui, s_model.log_note);
            if (s_model.logs[0]) {
                cui_textarea(ui, "hopdesk-log-view", s_model.logs, sizeof(s_model.logs),
                    .read_only = true, .height = 360, .width = CUI_FILL);
            } else {
                cui_secondary(ui, "点击“刷新日志”读取最近的运行记录。");
            }
        }
    }
}

static void draw_settings(cui_ctx *ui) {
    if (!s_settings.loaded) settings_load();
    switch (s_settings.tab) {
        case 1: draw_settings_input_and_display(ui); break;
        case 2: draw_settings_audio(ui); break;
        case 3: draw_settings_transfer(ui); break;
        case 4: draw_settings_maintenance(ui); break;
        default: draw_settings_connection_and_appearance(ui); break;
    }
}

void hopdesk_ui_set_ops(const hopdesk_ui_ops *ops) {
    s_ops = ops;
}

void hopdesk_ui_init(cui_ctx *ui) {
    s_ctx = ui;
    s_selected_file = -1;
    if (!s_settings.dirty && !s_settings.saving) s_settings.loaded = 0;
    if (s_ops && s_ops->load_cjk_font) {
        cui_font font = s_ops->load_cjk_font(ui);
        if (font >= 0) {
            cui_font_add_fallback(ui, CUI_FONT_BODY, font);
            cui_font_add_fallback(ui, CUI_FONT_MEDIUM, font);
            cui_font_add_fallback(ui, CUI_FONT_DISPLAY, font);
            cui_font_add_fallback(ui, CUI_FONT_MONO, font);
        }
    }
}

static float next_refresh_interval(void) {
    if (s_model.display_inventory.probing)
        return 0.5f;
    if (s_model.update.phase == HOP_UPDATE_CHECKING ||
        s_model.update.phase == HOP_UPDATE_DOWNLOADING ||
        s_model.update.phase == HOP_UPDATE_APPLYING)
        return 0.5f;
    if (s_model.transfer.send.active || s_model.transfer.receive.active)
        return 0.5f;
    for (int i = 0; i < s_model.task_count; ++i) {
        switch (s_model.tasks[i].state) {
            case HOP_XFER_TASK_CREATED:
            case HOP_XFER_TASK_SCANNING:
            case HOP_XFER_TASK_QUEUED:
            case HOP_XFER_TASK_ACTIVE:
                return 0.5f;
            default:
                break;
        }
    }
    return 2.0f;
}

void hopdesk_ui_frame(cui_ctx *ui) {
    int found = 0;
    if (!ui) return;
    if (s_ops && s_ops->snapshot)
        s_ops->snapshot(&s_model, s_selected_task, s_file_offset);
    if (s_settings.loaded && !s_settings.saving &&
        s_model.config.input_mode != s_settings.cfg.input_mode) {
        s_settings.cfg.input_mode = s_model.config.input_mode & HOP_INPUT_MODES;
        s_settings.provide_mouse = (s_settings.cfg.input_mode & HOP_INPUT_MOUSE) != 0;
        s_settings.provide_keyboard = (s_settings.cfg.input_mode & HOP_INPUT_KEYBOARD) != 0;
    }
    if (s_settings.loaded && !s_settings.saving &&
        s_model.config.audio_mode != s_settings.cfg.audio_mode) {
        s_settings.cfg.audio_mode = s_model.config.audio_mode & HOP_AUDIO_MODES;
        s_settings.audio_dir = audio_mode_to_dir(s_settings.cfg.audio_mode);
    }
    for (int i = 0; i < s_model.task_count; ++i)
        if (s_model.tasks[i].id == s_selected_task) { found = 1; break; }
    if (s_selected_task && !found) s_selected_task = 0;
    if (!s_selected_task && s_model.task_count > 0) {
        s_selected_task = s_model.tasks[0].id;
        s_file_offset = 0;
        if (s_ops && s_ops->snapshot)
            s_ops->snapshot(&s_model, s_selected_task, s_file_offset);
    }

    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_column(ui, .width = CUI_PX(218), .height = CUI_FILL, .pad = 18, .gap = 12) {
            cui_heading(ui, "HopDesk", 2);
            cui_secondary(ui, "跨设备协作");
            cui_separator(ui);
            cui_nav_section(ui, "工作区");
            if (cui_nav_item_ex(ui, "概览", &(cui_nav_opts){.icon = CUI_ICON_LAYOUT_DASHBOARD, .selected = s_page == HOPDESK_UI_OVERVIEW}))
                hopdesk_ui_show_page(HOPDESK_UI_OVERVIEW);
            if (cui_nav_item_ex(ui, "设备", &(cui_nav_opts){.icon = CUI_ICON_MONITOR, .selected = s_page == HOPDESK_UI_DEVICE}))
                hopdesk_ui_show_page(HOPDESK_UI_DEVICE);
            if (cui_nav_item_ex(ui, "传输", &(cui_nav_opts){.icon = CUI_ICON_REPEAT, .selected = s_page == HOPDESK_UI_TRANSFER,
                .badge = s_model.task_total > 0 ? "●" : NULL}))
                hopdesk_ui_show_page(HOPDESK_UI_TRANSFER);
            cui_nav_section(ui, "偏好");
            if (cui_nav_item_ex(ui, "设置", &(cui_nav_opts){.icon = CUI_ICON_SETTINGS, .selected = s_page == HOPDESK_UI_SETTINGS,
                .badge = (s_settings.dirty ||
                          s_model.update.phase == HOP_UPDATE_AVAILABLE ||
                          s_model.update.phase == HOP_UPDATE_READY) ? "●" : NULL}))
                hopdesk_ui_show_page(HOPDESK_UI_SETTINGS);
            cui_spacer(ui);
            cui_separator(ui);
            {
                char ver_line[96], proto_line[96];
                snprintf(ver_line, sizeof(ver_line), "v%s (%s)", HOP_VERSION, current_channel_zh());
                snprintf(proto_line, sizeof(proto_line), "控制 v%d · 数据 v%d",
                         HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
                cui_secondary(ui, ver_line);
                cui_muted(ui, proto_line);
            }
            cui_muted(ui, "轻量 · 跨平台 · 本地优先");
        }

        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 22, .gap = 14) {
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                bool is_dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
                const char *theme_tip = s_model.config.ui_theme == HOP_THEME_SYSTEM
                    ? (is_dark ? "当前：深色（跟随系统）" : "当前：浅色（跟随系统）")
                    : (is_dark ? "当前：深色模式" : "当前：浅色模式");
                cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                    cui_heading(ui, k_pages[s_page], 2);
                    cui_secondary(ui, k_page_subtitles[s_page]);
                }
                cui_push_id(ui, "##hopdesk-theme-toggle");
                if (cui_icon_button_ex(ui, is_dark ? CUI_ICON_MOON : CUI_ICON_SUN, &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .size = CUI_SIZE_SM,
                        .tooltip = theme_tip
                    })) {
                    apply_ui_theme(ui, is_dark ? HOP_THEME_LIGHT : HOP_THEME_DARK, 1);
                }
                cui_pop_id(ui);
                cui_badge_ex(ui, link_status_text(),
                    &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
            }
            cui_separator(ui);
            if (s_page == HOPDESK_UI_SETTINGS) {
                draw_settings_bar(ui);
            }
            if (s_scroll_reset_pending) {
                cui_scroll_to(ui, "hopdesk-page-scroll", 0.0f);
                s_scroll_reset_pending = 0;
            }
            cui_scroll(ui, .id = "hopdesk-page-scroll", .width = CUI_FILL, .height = CUI_FILL, .gap = 14) {
                switch (s_page) {
                    case HOPDESK_UI_DEVICE: draw_device(ui); break;
                    case HOPDESK_UI_TRANSFER: draw_transfer(ui); break;
                    case HOPDESK_UI_SETTINGS: draw_settings(ui); break;
                    default: draw_overview(ui); break;
                }
            }
        }
    }
    cui_request_wakeup(ui, next_refresh_interval());
}

void hopdesk_ui_shutdown(void) {
    s_ctx = NULL;
    if (s_ops && s_ops->release_cjk_font)
        s_ops->release_cjk_font();
    memset(&s_model, 0, sizeof(s_model));
    memset(&s_settings, 0, sizeof(s_settings));
    s_selected_task = 0;
    s_selected_file = -1;
    s_cancel_confirm_task = 0;
    s_file_offset = 0;
}

void hopdesk_ui_refresh(void) {
    if (s_ctx) cui_request_redraw(s_ctx);
}

void hopdesk_ui_show_page(int page) {
    if (page < 0 || page >= HOPDESK_UI_PAGE_COUNT) page = HOPDESK_UI_OVERVIEW;
    if (s_page != page) s_scroll_reset_pending = 1;
    s_page = page;
    if (s_ctx) cui_request_redraw(s_ctx);
}

void hopdesk_ui_save_complete(int ok, const char *message) {
    s_settings.saving = 0;
    if (ok) {
        s_settings.dirty = 0;
        s_model.config = s_settings.cfg;
        snprintf(s_settings.note, sizeof(s_settings.note), "%s",
            message && message[0] ? message : "设置已保存。连接方式、端口和认证参数需要重启。");
        ui_toast("设置已保存", s_settings.note, CUI_TONE_SUCCESS);
    } else {
        s_settings.dirty = 1;
        snprintf(s_settings.note, sizeof(s_settings.note), "%s",
            message && message[0] ? message : "保存失败，原配置未改变。");
        ui_toast("保存失败", s_settings.note, CUI_TONE_DANGER);
    }
    hopdesk_ui_refresh();
}

int hopdesk_ui_settings_dirty(void) {
    return s_settings.dirty || s_settings.saving;
}
