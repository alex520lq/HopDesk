#include "hopdesk_ui.h"
#include "../core/config_schema.h"
#include "../core/i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T(id) hop_tr(HOP_STR_##id)

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
    int role, server_edge, client_edge, ui_theme, ui_lang, log_level, update_channel;
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

static const char *const k_theme_icons[] = {
    CUI_ICON_MONITOR, CUI_ICON_SUN, CUI_ICON_MOON
};
static const char *const k_audio_direction_icons[] = {
    CUI_ICON_VOLUME_X, CUI_ICON_SEND, CUI_ICON_VOLUME_2
};
static const char *const k_settings_tab_icons[] = {
    CUI_ICON_WIFI, CUI_ICON_KEYBOARD, CUI_ICON_VOLUME_2, CUI_ICON_REPEAT, CUI_ICON_TERMINAL
};

static const char *page_title(int page) {
    switch (page) {
        case HOPDESK_UI_DEVICE:   return T(NAV_DEVICE);
        case HOPDESK_UI_TRANSFER: return T(NAV_TRANSFER);
        case HOPDESK_UI_SETTINGS: return T(NAV_SETTINGS);
        default:                  return T(NAV_OVERVIEW);
    }
}

static const char *page_tip(int page) {
    switch (page) {
        case HOPDESK_UI_DEVICE:   return T(PAGE_DEVICE_TIP);
        case HOPDESK_UI_TRANSFER: return T(PAGE_TRANSFER_TIP);
        case HOPDESK_UI_SETTINGS: return T(PAGE_SETTINGS_TIP);
        default:                  return T(PAGE_OVERVIEW_TIP);
    }
}

static void draw_help_hint(cui_ctx *ui, const char *id, const char *tip) {
    if (!ui || !id || !tip || !tip[0]) return;
    cui_push_id(ui, id);
    (void)cui_icon_button_ex(ui, CUI_ICON_CIRCLE_HELP, &(cui_button_opts){
        .variant = CUI_BUTTON_GHOST,
        .size = CUI_SIZE_SM
    });
    cui_pop_id(ui);
    if (cui_hover_card_begin(ui, id, &(cui_hover_card_opts){
            .placement = CUI_PLACE_BOTTOM,
            .width = 300.0f,
            .delay = 0.2f
        })) {
        cui_secondary(ui, tip);
        cui_hover_card_end(ui);
    }
}

static void draw_heading_with_help(cui_ctx *ui, const char *title, int level,
                                   const char *help_id, const char *tip) {
    cui_row(ui, .align = CUI_ALIGN_CENTER, .gap = 4) {
        cui_heading(ui, title, level);
        draw_help_hint(ui, help_id, tip);
    }
}

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
    if (s_model.audio_unavailable) return T(AUDIO_SUMMARY_UNAVAIL);
    if (act & HOP_AUDIO_SEND) return T(AUDIO_SUMMARY_SENDING);
    if (act & HOP_AUDIO_PLAY) return T(AUDIO_SUMMARY_PLAYING);
    if (req == 0u) return T(AUDIO_SUMMARY_OFF);
    if (!s_model.connected) return req == HOP_AUDIO_SEND ? T(AUDIO_SUMMARY_WAIT_SEND) : T(AUDIO_SUMMARY_WAIT_PLAY);
    if (s_model.peer_capabilities_known && req == HOP_AUDIO_PLAY && peer == HOP_AUDIO_PLAY)
        return T(AUDIO_SUMMARY_CONFLICT_PLAY);
    if (s_model.peer_capabilities_known && req == HOP_AUDIO_SEND && peer == HOP_AUDIO_SEND)
        return T(AUDIO_SUMMARY_CONFLICT_SEND);
    if (req == HOP_AUDIO_SEND) return T(AUDIO_SUMMARY_READY_SEND);
    if (req == HOP_AUDIO_PLAY) return T(AUDIO_SUMMARY_READY_PLAY);
    return T(AUDIO_SUMMARY_STANDBY);
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
    if (s_model.connected) return T(STATUS_CONNECTED);
    if (s_model.manually_disconnected) return T(STATUS_DISCONNECTED_MANUAL);
    return T(STATUS_DISCONNECTED);
}

static cui_tone link_status_tone(void) {
    if (s_model.connected) return CUI_TONE_SUCCESS;
    if (s_model.manually_disconnected) return CUI_TONE_WARNING;
    return CUI_TONE_NEUTRAL;
}

static const char *capability_owner_text(unsigned bit) {
    if (s_model.config.input_mode & bit) return T(CAP_OWNER_LOCAL);
    if (s_model.connected && (s_model.peer_input_mode & bit)) return T(CAP_OWNER_PEER);
    return T(CAP_OWNER_NONE);
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
    if (lm && lk) return T(CAP_INPUT_BOTH_LOCAL);
    if (pm && pk) return T(CAP_INPUT_BOTH_PEER);
    if (lm) return T(CAP_INPUT_MOUSE_LOCAL);
    if (lk) return T(CAP_INPUT_KBD_LOCAL);
    if (pm) return T(CAP_INPUT_MOUSE_PEER);
    if (pk) return T(CAP_INPUT_KBD_PEER);
    return T(CAP_INPUT_NONE);
}

static cui_tone input_summary_tone(void) {
    if (s_model.sharing) return CUI_TONE_INFO;
    if (s_model.config.input_mode & HOP_INPUT_MODES) return CUI_TONE_INFO;
    if (s_model.connected && (s_model.peer_input_mode & HOP_INPUT_MODES)) return CUI_TONE_SUCCESS;
    return CUI_TONE_NEUTRAL;
}

static void apply_input_mode(unsigned next_mode, const char *detail_on, const char *detail_off, int enabled) {
    next_mode &= HOP_INPUT_MODES;
    s_model.config.input_mode = next_mode;
    s_settings.cfg.input_mode = next_mode;
    s_settings.provide_mouse = (next_mode & HOP_INPUT_MOUSE) != 0;
    s_settings.provide_keyboard = (next_mode & HOP_INPUT_KEYBOARD) != 0;
    if (enabled) s_model.peer_input_mode &= ~next_mode;
    if (s_ops && s_ops->set_input_mode) {
        s_ops->set_input_mode(next_mode);
        ui_toast(T(TOAST_INPUT_UPDATED), enabled ? detail_on : detail_off,
                 enabled ? CUI_TONE_INFO : CUI_TONE_NEUTRAL);
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
            cui_badge_ex(ui, T(CAP_MOUSE), &(cui_badge_opts){.tone = CUI_TONE_INFO, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (input_mode & HOP_INPUT_KEYBOARD) {
            cui_badge_ex(ui, T(CAP_KEYBOARD), &(cui_badge_opts){.tone = CUI_TONE_INFO, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (audio_mode & HOP_AUDIO_SEND) {
            cui_badge_ex(ui, T(CAP_AUDIO_SEND), &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (audio_mode & HOP_AUDIO_PLAY) {
            cui_badge_ex(ui, T(CAP_AUDIO_PLAY), &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM});
            any = 1;
        }
        if (!any && show_empty) {
            cui_badge_ex(ui, T(CAP_NONE), &(cui_badge_opts){.tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM});
        }
    }
}

static void action_disconnect(void) {
    if (!s_ops || !s_ops->disconnect) return;
    s_ops->disconnect();
    ui_toast(T(TOAST_DISCONNECT_TITLE), T(TOAST_DISCONNECT_BODY), CUI_TONE_WARNING);
}

static void action_reconnect(void) {
    if (!s_ops || !s_ops->reconnect) return;
    s_ops->reconnect();
    ui_toast(T(TOAST_RECONNECT_TITLE), T(TOAST_RECONNECT_BODY), CUI_TONE_INFO);
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
        case HOP_XFER_TASK_CREATED:
        case HOP_XFER_TASK_QUEUED:      return T(TRANSFER_STATE_QUEUED);
        case HOP_XFER_TASK_SCANNING:    return T(TRANSFER_STATE_SCANNING);
        case HOP_XFER_TASK_ACTIVE:      return T(TRANSFER_STATE_ACTIVE);
        case HOP_XFER_TASK_COMPLETED:   return T(TRANSFER_STATE_COMPLETED);
        case HOP_XFER_TASK_PARTIAL:     return T(TRANSFER_STATE_PARTIAL);
        case HOP_XFER_TASK_FAILED:      return T(TRANSFER_STATE_FAILED);
        case HOP_XFER_TASK_INTERRUPTED: return T(TRANSFER_STATE_INTERRUPTED);
        case HOP_XFER_TASK_CANCELLED:   return T(TRANSFER_STATE_CANCELLED);
        default:                        return T(TRANSFER_STATE_UNKNOWN);
    }
}

static cui_tone task_tone(int state) {
    switch (state) {
        case HOP_XFER_TASK_COMPLETED:   return CUI_TONE_SUCCESS;
        case HOP_XFER_TASK_PARTIAL:
        case HOP_XFER_TASK_FAILED:
        case HOP_XFER_TASK_INTERRUPTED: return CUI_TONE_DANGER;
        case HOP_XFER_TASK_CANCELLED:   return CUI_TONE_NEUTRAL;
        case HOP_XFER_TASK_ACTIVE:      return CUI_TONE_INFO;
        default:                        return CUI_TONE_WARNING;
    }
}

static const char *file_state_text(int state) {
    switch (state) {
        case HOP_XFER_FILE_SENDING:
        case HOP_XFER_FILE_RECEIVING:
        case HOP_XFER_FILE_VERIFYING: return T(TRANSFER_FILE_ACTIVE);
        case HOP_XFER_FILE_COMPLETED: return T(TRANSFER_FILE_DONE);
        case HOP_XFER_FILE_FAILED:    return T(TRANSFER_FILE_FAILED);
        default:                      return T(TRANSFER_FILE_QUEUED);
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

static const char *update_channel_label(int ch) {
    return ch == HOP_UPDATE_CHANNEL_DEV ? T(VER_CHANNEL_DEV) : T(VER_CHANNEL_STABLE);
}

static const char *current_channel_label(void) {
    return strcmp(HOP_CHANNEL, "dev") == 0 ? T(VER_CHANNEL_DEV) : T(VER_CHANNEL_STABLE);
}

static const char *update_phase_text(int phase) {
    switch (phase) {
        case HOP_UPDATE_CHECKING:    return T(UPD_PHASE_CHECKING);
        case HOP_UPDATE_UP_TO_DATE:  return T(UPD_PHASE_UPTODATE);
        case HOP_UPDATE_AVAILABLE:   return T(UPD_PHASE_AVAILABLE);
        case HOP_UPDATE_DOWNLOADING: return T(UPD_PHASE_DOWNLOADING);
        case HOP_UPDATE_READY:       return T(UPD_PHASE_READY);
        case HOP_UPDATE_APPLYING:    return T(UPD_PHASE_APPLYING);
        case HOP_UPDATE_ERROR:       return T(UPD_PHASE_ERROR);
        default:                     return T(UPD_PHASE_IDLE);
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

static void trigger_permission_action(int action) {
    int ok = 0;
    if (s_ops && s_ops->permission_action) {
        ok = s_ops->permission_action(action);
    } else if (action == HOPDESK_UI_PERM_DIALOG && s_ops && s_ops->request_permissions) {
        s_ops->request_permissions();
        ok = 1;
    }
    switch (action) {
        case HOPDESK_UI_PERM_ACCESSIBILITY:
            ui_toast(ok ? T(PERM_TOAST_AX_OK) : T(PERM_TOAST_OPEN_FAIL),
                     ok ? T(PERM_TOAST_AX_BODY) : T(PERM_TOAST_OPEN_FAIL_BODY),
                     ok ? CUI_TONE_INFO : CUI_TONE_WARNING);
            break;
        case HOPDESK_UI_PERM_INPUT_MONITORING:
            ui_toast(ok ? T(PERM_TOAST_LISTEN_OK) : T(PERM_TOAST_OPEN_FAIL),
                     ok ? T(PERM_TOAST_LISTEN_BODY) : T(PERM_TOAST_OPEN_FAIL_BODY),
                     ok ? CUI_TONE_INFO : CUI_TONE_WARNING);
            break;
        case HOPDESK_UI_PERM_SCREEN_AUDIO:
            ui_toast(ok ? T(PERM_TOAST_AUDIO_OK) : T(PERM_TOAST_OPEN_FAIL),
                     ok ? T(PERM_TOAST_AUDIO_BODY) : T(PERM_TOAST_OPEN_FAIL_BODY),
                     ok ? CUI_TONE_INFO : CUI_TONE_WARNING);
            break;
        case HOPDESK_UI_PERM_ELEVATE:
            ui_toast(ok ? T(PERM_TOAST_ELEVATE_OK) : T(PERM_TOAST_ELEVATE_FAIL),
                     ok ? T(PERM_TOAST_ELEVATE_OK_BODY) : T(PERM_TOAST_ELEVATE_FAIL_BODY),
                     ok ? CUI_TONE_INFO : CUI_TONE_WARNING);
            break;
        case HOPDESK_UI_PERM_FIREWALL_ALLOW:
            ui_toast(ok ? T(PERM_TOAST_FW_OK) : T(PERM_TOAST_FW_FAIL),
                     ok ? T(PERM_TOAST_FW_OK_BODY) : T(PERM_TOAST_FW_FAIL_BODY),
                     ok ? CUI_TONE_INFO : CUI_TONE_WARNING);
            break;
        case HOPDESK_UI_PERM_FIREWALL_OPEN:
            if (!ok) ui_toast(T(PERM_TOAST_FW_OPEN_FAIL), T(PERM_TOAST_FW_OPEN_FAIL_BODY), CUI_TONE_WARNING);
            break;
        default:
            break;
    }
}

static void draw_permissions_card(cui_ctx *ui) {
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
#ifdef _WIN32
            draw_heading_with_help(ui, T(PERM_CARD_TITLE), 3, "help-perm-win", T(PERM_TIP_WIN));
            cui_badge_ex(ui, s_model.permission_elevated ? T(PERM_BADGE_ADMIN) : T(PERM_BADGE_STANDARD),
                &(cui_badge_opts){.tone = s_model.permission_elevated ? CUI_TONE_SUCCESS : CUI_TONE_INFO, .dot = true});
#else
            int all_ok = s_model.permissions_known &&
                         s_model.permission_post &&
                         s_model.permission_listen &&
                         s_model.permission_accessibility;
            draw_heading_with_help(ui, T(PERM_CARD_TITLE), 3, "help-perm-mac", T(PERM_TIP_MAC));
            cui_badge_ex(ui, all_ok ? T(PERM_BADGE_READY) : T(PERM_BADGE_NEEDED),
                &(cui_badge_opts){.tone = all_ok ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .dot = true});
#endif
        }

#ifdef _WIN32
        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            if (cui_button_ex(ui, s_model.permission_elevated ? T(PERM_BTN_IS_ADMIN) : T(PERM_BTN_ELEVATE),
                    &(cui_button_opts){
                        .variant = s_model.permission_elevated ? CUI_BUTTON_OUTLINE : CUI_BUTTON_PRIMARY,
                        .size = CUI_SIZE_SM,
                        .icon = CUI_ICON_SHIELD,
                        .disabled = s_model.permission_elevated || !s_ops || !s_ops->permission_action
                    })) {
                trigger_permission_action(HOPDESK_UI_PERM_ELEVATE);
            }
            if (cui_button_ex(ui, T(PERM_BTN_FW_ALLOW), &(cui_button_opts){
                    .variant = CUI_BUTTON_SECONDARY,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_WIFI,
                    .disabled = !s_ops || !s_ops->permission_action
                })) {
                trigger_permission_action(HOPDESK_UI_PERM_FIREWALL_ALLOW);
            }
            if (cui_button_ex(ui, T(PERM_BTN_FW_OPEN), &(cui_button_opts){
                    .variant = CUI_BUTTON_OUTLINE,
                    .size = CUI_SIZE_SM,
                    .disabled = !s_ops || !s_ops->permission_action
                })) {
                trigger_permission_action(HOPDESK_UI_PERM_FIREWALL_OPEN);
            }
        }
#else
        if (s_model.permissions_known) {
            cui_row(ui, .width = CUI_FILL, .gap = 6, .wrap = true) {
                cui_badge_ex(ui, (s_model.permission_accessibility && s_model.permission_post) ? T(PERM_BADGE_AX_OK) : T(PERM_BADGE_AX_NO),
                    &(cui_badge_opts){.tone = (s_model.permission_accessibility && s_model.permission_post) ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .size = CUI_SIZE_SM});
                cui_badge_ex(ui, s_model.permission_listen ? T(PERM_BADGE_LISTEN_OK) : T(PERM_BADGE_LISTEN_NO),
                    &(cui_badge_opts){.tone = s_model.permission_listen ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .size = CUI_SIZE_SM});
                cui_badge_ex(ui, T(PERM_BADGE_AUDIO_OPT),
                    &(cui_badge_opts){.tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM});
            }
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            if (cui_button_ex(ui, T(PERM_BTN_AX), &(cui_button_opts){
                    .variant = (s_model.permissions_known && (!s_model.permission_accessibility || !s_model.permission_post))
                        ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_SHIELD,
                    .disabled = !s_ops || !s_ops->permission_action
                })) {
                trigger_permission_action(HOPDESK_UI_PERM_ACCESSIBILITY);
            }
            if (cui_button_ex(ui, T(PERM_BTN_LISTEN), &(cui_button_opts){
                    .variant = (s_model.permissions_known && !s_model.permission_listen)
                        ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_KEYBOARD,
                    .disabled = !s_ops || !s_ops->permission_action
                })) {
                trigger_permission_action(HOPDESK_UI_PERM_INPUT_MONITORING);
            }
            if (cui_button_ex(ui, T(PERM_BTN_AUDIO), &(cui_button_opts){
                    .variant = CUI_BUTTON_OUTLINE,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_VOLUME_2,
                    .disabled = !s_ops || !s_ops->permission_action
                })) {
                trigger_permission_action(HOPDESK_UI_PERM_SCREEN_AUDIO);
            }
        }
#endif
        if (s_model.app_path[0]) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 8) {
                char path_line[560];
                snprintf(path_line, sizeof(path_line), T(PERM_PATH_LABEL), s_model.app_path);
                cui_muted(ui, path_line);
                if (cui_button_ex(ui, T(BTN_COPY_PATH), &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .size = CUI_SIZE_SM,
                        .icon = CUI_ICON_COPY,
                        .disabled = !s_ops || !s_ops->copy_text
                    }) && s_ops && s_ops->copy_text) {
                    if (s_ops->copy_text(s_model.app_path))
                        ui_toast(T(TOAST_COPIED_PATH), T(TOAST_COPIED_PATH_BODY), CUI_TONE_SUCCESS);
                }
            }
        }
    }
}

static void draw_overview(cui_ctx *ui) {
    char peer[160], data[96], audio[96];
    snprintf(peer, sizeof(peer), "%s",
        s_model.peer_label[0] ? s_model.peer_label : T(STATUS_WAITING_PEER));
    snprintf(data, sizeof(data), "%s",
        s_model.data_connected ? T(STATUS_DATA_READY) : T(STATUS_DATA_IDLE));
    snprintf(audio, sizeof(audio), T(STATUS_AUDIO_META),
        s_model.audio_requested ? T(STATUS_CONFIGURED) : T(STATUS_OFF),
        s_model.audio_active ? T(STATUS_YES) : T(STATUS_NO));

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        if (s_model.protocol_mismatch) {
            char proto_detail[280];
            snprintf(proto_detail, sizeof(proto_detail), T(OVERVIEW_PROTO_DETAIL),
                HOP_VERSION, current_channel_label(), HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                    cui_heading(ui, T(OVERVIEW_PROTO_MISMATCH), 3);
                    cui_badge_ex(ui, T(OVERVIEW_PROTO_NEED_SYNC), &(cui_badge_opts){.tone = CUI_TONE_WARNING, .dot = true});
                }
                cui_text(ui, proto_detail);
                if (cui_button(ui, T(BTN_GO_UPDATE), .variant = CUI_BUTTON_SECONDARY))
                    open_settings_tab(4);
            }
        }
        if (s_model.update.phase == HOP_UPDATE_READY) {
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                    cui_heading(ui, T(OVERVIEW_UPDATE_READY), 3);
                    cui_badge_ex(ui, T(OVERVIEW_UPDATE_PENDING), &(cui_badge_opts){.tone = CUI_TONE_SUCCESS, .dot = true});
                }
                cui_text(ui, s_model.update.message[0] ? s_model.update.message : T(OVERVIEW_UPDATE_READY_FALLBACK));
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, T(BTN_RESTART_APPLY_UPDATE), .variant = CUI_BUTTON_PRIMARY,
                        .disabled = !s_ops || !s_ops->update_action)) {
                        if (s_ops && s_ops->update_action)
                            s_ops->update_action(HOP_UPDATE_ACTION_APPLY, s_model.update.target_channel);
                    }
                    if (cui_button(ui, T(BTN_VIEW_UPDATE), .variant = CUI_BUTTON_SECONDARY))
                        open_settings_tab(4);
                }
            }
        }

        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            draw_stat_card(ui, T(OVERVIEW_CARD_CONTROL), link_status_text(), peer, link_status_tone());
            draw_stat_card(ui, T(OVERVIEW_CARD_DATA),
                s_model.data_connected ? T(STATUS_CONNECTED) : T(STATUS_DISCONNECTED),
                data, s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL);
            draw_stat_card(ui, T(OVERVIEW_CARD_INPUT), input_summary_text(),
                s_model.sharing ? T(STATUS_SHARING_PEER) : (s_model.connected ? T(STATUS_CAPS_SYNCED) : T(STATUS_WAITING_PEER)),
                input_summary_tone());
            draw_stat_card(ui, T(OVERVIEW_CARD_AUDIO), audio_direction_summary(), audio, audio_direction_tone());
        }

        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                draw_heading_with_help(ui, T(OVERVIEW_CARD_PEERS), 3, "help-overview-peers", T(OVERVIEW_CARD_PEERS_TIP));
                cui_badge_ex(ui, link_status_text(), &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 18, .wrap = true, .align = CUI_ALIGN_START) {
                cui_column(ui, .width = CUI_GROW(1), .gap = 6) {
                    cui_muted(ui, T(OVERVIEW_LOCAL_DEVICE));
                    cui_label(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk");
                    cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : T(STATUS_NO_LAN_IP));
                    draw_capability_badges(ui, s_model.config.input_mode, s_model.audio_requested, 1);
                }
                cui_column(ui, .width = CUI_GROW(1), .gap = 6) {
                    cui_muted(ui, T(OVERVIEW_PEER_DEVICE));
                    cui_label(ui, peer);
                    cui_text(ui, data);
                    if (s_model.connected && s_model.peer_capabilities_known)
                        draw_capability_badges(ui, s_model.peer_input_mode, s_model.peer_audio_mode, 1);
                    else
                        cui_muted(ui, T(OVERVIEW_PEER_WAITING));
                }
            }
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, s_model.manually_disconnected ? T(BTN_RESUME_CONNECT) : T(BTN_RECONNECT), &(cui_button_opts){
                        .variant = s_model.manually_disconnected ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE,
                        .icon = s_model.manually_disconnected ? CUI_ICON_LINK : CUI_ICON_REFRESH_CW,
                        .disabled = !s_ops || !s_ops->reconnect
                    }))
                    action_reconnect();
                if (cui_button_ex(ui, T(BTN_DISCONNECT), &(cui_button_opts){
                        .variant = CUI_BUTTON_DANGER,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = s_model.manually_disconnected || !s_ops || !s_ops->disconnect
                    }))
                    action_disconnect();
                if (cui_button_ex(ui, T(OVERVIEW_BTN_DISP_TOGGLE), &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_MONITOR,
                        .disabled = !s_ops || !s_ops->display_action
                    }))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TOGGLE);
                if (cui_button(ui, T(OVERVIEW_BTN_DISP_MAC), .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_MAC);
                if (cui_button(ui, T(OVERVIEW_BTN_DISP_WIN), .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_WINDOWS);
                if (cui_button(ui, T(OVERVIEW_BTN_DISP_WAKE), .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_WAKE);
            }
        }

        if (s_model.display_status[0]) cui_text(ui, s_model.display_status);
        if (s_model.permissions_known &&
            (!s_model.permission_post || !s_model.permission_listen || !s_model.permission_accessibility)) {
            cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_heading(ui, T(OVERVIEW_CARD_PERM_WARN), 3);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    cui_badge_ex(ui, s_model.permission_post ? T(PERM_BADGE_POST_OK) : T(PERM_BADGE_POST_NO),
                        &(cui_badge_opts){.tone = s_model.permission_post ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                    cui_badge_ex(ui, s_model.permission_listen ? T(PERM_BADGE_LISTEN_OK) : T(PERM_BADGE_LISTEN_NO),
                        &(cui_badge_opts){.tone = s_model.permission_listen ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                    cui_badge_ex(ui, s_model.permission_accessibility ? T(PERM_BADGE_AX_OK) : T(PERM_BADGE_AX_NO),
                        &(cui_badge_opts){.tone = s_model.permission_accessibility ? CUI_TONE_SUCCESS : CUI_TONE_WARNING});
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button_ex(ui, T(PERM_BTN_AX), &(cui_button_opts){
                            .variant = (!s_model.permission_accessibility || !s_model.permission_post)
                                ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_SHIELD,
                            .disabled = !s_ops || !s_ops->permission_action
                        }))
                        trigger_permission_action(HOPDESK_UI_PERM_ACCESSIBILITY);
                    if (cui_button_ex(ui, T(PERM_BTN_LISTEN), &(cui_button_opts){
                            .variant = !s_model.permission_listen ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_KEYBOARD,
                            .disabled = !s_ops || !s_ops->permission_action
                        }))
                        trigger_permission_action(HOPDESK_UI_PERM_INPUT_MONITORING);
                    if (cui_button_ex(ui, T(PERM_BTN_CHECK), &(cui_button_opts){
                            .variant = CUI_BUTTON_OUTLINE,
                            .size = CUI_SIZE_SM,
                            .disabled = !s_ops || !s_ops->request_permissions
                        }) && s_ops && s_ops->request_permissions)
                        s_ops->request_permissions();
                }
            }
        }
    }
}

static void draw_device(cui_ctx *ui) {
    char ports[128], dimensions[96], mouse_badge[64], kbd_badge[64];
    snprintf(ports, sizeof(ports), T(VER_PROTO_LINE),
        (int)s_model.config.port, (int)s_model.config.data_port);
    snprintf(dimensions, sizeof(dimensions), "%d × %d",
        s_model.config.screen_w, s_model.config.screen_h);
    snprintf(mouse_badge, sizeof(mouse_badge), T(DEVICE_MOUSE_BADGE), capability_owner_text(HOP_INPUT_MOUSE));
    snprintf(kbd_badge, sizeof(kbd_badge), T(DEVICE_KBD_BADGE), capability_owner_text(HOP_INPUT_KEYBOARD));

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, T(DEVICE_LOCAL));
                cui_heading(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk", 3);
                cui_secondary(ui, s_model.config.role == HOP_ROLE_SERVER
                    ? T(CONNECT_MODE_SERVER) : T(CONNECT_MODE_CLIENT));
                cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : T(STATUS_NO_LAN_IP));
                cui_secondary(ui, dimensions);
                draw_capability_badges(ui, s_model.config.input_mode, s_model.audio_requested, 1);
            }
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, T(DEVICE_PEER));
                cui_heading(ui, s_model.peer_label[0] ? s_model.peer_label : T(STATUS_DISCONNECTED), 3);
                cui_secondary(ui, link_status_text());
                cui_text(ui, ports);
                if (s_model.config.role == HOP_ROLE_CLIENT)
                    cui_text(ui, s_model.config.peer_host[0] ? s_model.config.peer_host : T(STATUS_PEER_HOST_UNSET));
                if (s_model.connected && s_model.peer_capabilities_known)
                    draw_capability_badges(ui, s_model.peer_input_mode, s_model.peer_audio_mode, 1);
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 10) {
            draw_heading_with_help(ui, T(DEVICE_CARD_CONTROL), 3, "help-device-ctrl", T(DEVICE_CARD_CONTROL_TIP));
            cui_row(ui, .width = CUI_FILL, .gap = 10, .wrap = true) {
                cui_badge_ex(ui, link_status_text(), &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
                cui_badge_ex(ui, s_model.data_connected ? T(STATUS_DATA_READY) : T(STATUS_DATA_IDLE),
                    &(cui_badge_opts){.tone = s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
                cui_badge_ex(ui, mouse_badge, &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_MOUSE)});
                cui_badge_ex(ui, kbd_badge, &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_KEYBOARD)});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, s_model.manually_disconnected ? T(BTN_RESUME_CONNECT) : T(BTN_RECONNECT), &(cui_button_opts){
                        .variant = CUI_BUTTON_PRIMARY,
                        .icon = s_model.manually_disconnected ? CUI_ICON_LINK : CUI_ICON_REFRESH_CW,
                        .disabled = !s_ops || !s_ops->reconnect
                    }))
                    action_reconnect();
                if (cui_button_ex(ui, T(BTN_DISCONNECT), &(cui_button_opts){
                        .variant = CUI_BUTTON_DANGER,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = s_model.manually_disconnected || !s_ops || !s_ops->disconnect
                    }))
                    action_disconnect();
                if (cui_button(ui, T(DEVICE_BTN_CFG_INPUT), .variant = CUI_BUTTON_SECONDARY))
                    open_settings_tab(1);
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 8) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                draw_heading_with_help(ui, T(DEVICE_CARD_AUDIO), 3, "help-device-audio", T(DEVICE_CARD_AUDIO_TIP));
                cui_badge_ex(ui, audio_direction_summary(),
                    &(cui_badge_opts){.tone = audio_direction_tone(), .dot = true});
            }
            if (cui_button(ui, T(DEVICE_BTN_CFG_AUDIO), .variant = CUI_BUTTON_SECONDARY))
                open_settings_tab(2);
        }
    }
}

static void draw_transfer_summary(cui_ctx *ui, const hop_xfer_status *status, const char *title) {
    char bytes[64], counts[96];
    format_bytes(status->bytes_done, bytes, sizeof(bytes));
    snprintf(counts, sizeof(counts), T(TRANSFER_FILES_BYTES),
        status->files_done, status->files_total, bytes);
    cui_card(ui, .width = CUI_GROW(1), .min_width = 250, .pad = 14, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
            cui_label(ui, title);
            cui_badge_ex(ui, status->active ? T(STATUS_ACTIVE) : T(STATUS_IDLE),
                &(cui_badge_opts){.tone = status->active ? CUI_TONE_INFO : CUI_TONE_NEUTRAL, .dot = true});
        }
        if (status->name[0]) cui_text(ui, status->name);
        if (status->detail[0]) cui_secondary(ui, status->detail);
        cui_progress_ex(ui, status->bytes_total ?
            (float)((double)status->bytes_done / (double)status->bytes_total) : 0.0f,
            &(cui_progress_opts){.width = CUI_FILL, .height = 7, .show_value = true});
        cui_muted(ui, counts);
    }
}

static void draw_transfer(cui_ctx *ui) {
    const cui_table_column task_columns[] = {
        {T(TRANSFER_COL_NAME),     CUI_GROW(2), CUI_TEXT_START, false},
        {T(TRANSFER_COL_DIR),      CUI_PX(72),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_STATE),    CUI_PX(96),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_PROGRESS), CUI_PX(170), CUI_TEXT_START, false}
    };
    const cui_table_column file_columns[] = {
        {T(TRANSFER_COL_FILE),     CUI_GROW(2), CUI_TEXT_START, false},
        {T(TRANSFER_COL_STATE),    CUI_PX(92),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_PROGRESS), CUI_PX(150), CUI_TEXT_START, false}
    };
    const cui_table_column history_columns[] = {
        {T(TRANSFER_COL_NAME),  CUI_GROW(2), CUI_TEXT_START, false},
        {T(TRANSFER_COL_DIR),   CUI_PX(72),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_STATE), CUI_PX(96),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_FILES), CUI_PX(90),  CUI_TEXT_START, false},
        {T(TRANSFER_COL_SIZE),  CUI_PX(100), CUI_TEXT_END,   false}
    };
    int i;
    const hop_xfer_task_snapshot *chosen = selected_task();

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true, .align = CUI_ALIGN_STRETCH) {
            draw_transfer_summary(ui, &s_model.transfer.send, T(TRANSFER_SEND));
            draw_transfer_summary(ui, &s_model.transfer.receive, T(TRANSFER_RECEIVE));
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                char total_lbl[64];
                cui_heading(ui, T(TRANSFER_CARD_TASKS), 3);
                snprintf(total_lbl, sizeof(total_lbl), T(TRANSFER_CARD_TASKS_TOTAL), s_model.task_total);
                cui_muted(ui, total_lbl);
            }
            if (s_model.task_count == 0) {
                cui_secondary(ui, T(TRANSFER_EMPTY_TASKS));
            } else if (cui_table_begin_ex(ui, "hopdesk-active-tasks", task_columns, 4,
                       &(cui_table_opts){.height = CUI_PX(240), .striped = true, .compact = true, .selectable = true})) {
                for (i = 0; i < s_model.task_count; ++i) {
                    const hop_xfer_task_snapshot *task = &s_model.tasks[i];
                    if (cui_table_row(ui, task->id == s_selected_task)) {
                        s_selected_task = task->id;
                        s_cancel_confirm_task = 0;
                        s_file_offset = 0;
                        s_selected_file = -1;
                    }
                    cui_table_cell_text(ui, task->name[0] ? task->name : T(TRANSFER_DEFAULT_NAME));
                    cui_table_cell_text(ui, task->direction == HOP_XFER_DIR_SEND ? T(TRANSFER_SEND) : T(TRANSFER_RECEIVE));
                    cui_table_cell(ui);
                    cui_badge_ex(ui, task_state_text(task->state),
                        &(cui_badge_opts){.tone = task_tone(task->state), .size = CUI_SIZE_SM});
                    cui_table_cell(ui);
                    cui_progress_ex(ui, task_progress(task),
                        &(cui_progress_opts){.width = CUI_FILL, .height = 5});
                }
                cui_table_end(ui);
            }
            cui_column(ui, .width = CUI_FILL, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, T(TRANSFER_BTN_CANCEL), .variant = CUI_BUTTON_DANGER,
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
                            if (cui_button(ui, T(TRANSFER_BTN_KEEP), .variant = CUI_BUTTON_SECONDARY))
                                s_cancel_confirm_task = 0;
                            if (cui_button(ui, T(TRANSFER_BTN_CONFIRM_CANCEL), .variant = CUI_BUTTON_DANGER)) {
                                if (s_ops && s_ops->cancel_task &&
                                    s_ops->cancel_task(s_cancel_confirm_task))
                                    ui_toast(T(TRANSFER_TOAST_CANCEL_OK), T(TRANSFER_TOAST_CANCEL_OK_BODY), CUI_TONE_INFO);
                                else
                                    ui_toast(T(TRANSFER_TOAST_CANCEL_FAIL), T(TRANSFER_TOAST_CANCEL_FAIL_BODY), CUI_TONE_WARNING);
                                s_cancel_confirm_task = 0;
                            }
                        }
                    }
                }
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, T(TRANSFER_CARD_FILES), 3);
                if (s_model.file_total > 0) {
                    char page_str[64];
                    unsigned start_n = s_file_offset + 1u;
                    unsigned end_n = s_file_offset + (unsigned)s_model.file_count;
                    snprintf(page_str, sizeof(page_str), T(TRANSFER_FILES_PAGE), start_n, end_n, s_model.file_total);
                    cui_muted(ui, page_str);
                }
            }
            if (s_model.file_total > 0) {
                if (cui_table_begin_ex(ui, "hopdesk-transfer-files", file_columns, 3,
                    &(cui_table_opts){.height = CUI_PX(280), .striped = true, .compact = true, .selectable = true})) {
                    for (i = 0; i < s_model.file_count; ++i) {
                        const hop_xfer_file_status *file = &s_model.files[i];
                        float ratio = file->bytes_total ?
                            (float)((double)file->bytes_done / (double)file->bytes_total) :
                            (file->phase == HOP_XFER_FILE_COMPLETED ? 1.0f : 0.0f);
                        if (cui_table_row(ui, (int)file->index == s_selected_file))
                            s_selected_file = (int)file->index;
                        cui_table_cell_text(ui, file->relative_path[0] ? file->relative_path : "-");
                        cui_table_cell_text(ui, file_state_text(file->phase));
                        cui_table_cell(ui);
                        cui_progress_ex(ui, ratio, &(cui_progress_opts){.width = CUI_FILL, .height = 5});
                    }
                    cui_table_end(ui);
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (cui_button(ui, T(TRANSFER_BTN_PREV), .disabled = s_file_offset == 0)) {
                        s_file_offset = s_file_offset >= HOPDESK_UI_FILE_PAGE ?
                            s_file_offset - HOPDESK_UI_FILE_PAGE : 0;
                        s_selected_file = -1;
                    }
                    if (cui_button(ui, T(TRANSFER_BTN_NEXT), .disabled =
                        s_file_offset + HOPDESK_UI_FILE_PAGE >= (uint32_t)s_model.file_total)) {
                        s_file_offset += HOPDESK_UI_FILE_PAGE;
                        s_selected_file = -1;
                    }
                    if (cui_button(ui, T(TRANSFER_BTN_RETRY), .variant = CUI_BUTTON_SECONDARY,
                        .disabled = !selected_file() || selected_file()->phase != HOP_XFER_FILE_FAILED ||
                                    s_model.file_direction != HOP_XFER_DIR_SEND ||
                                    !s_ops || !s_ops->retry_file)) {
                        if (selected_file() && s_ops && s_ops->retry_file &&
                            s_ops->retry_file(s_model.file_batch, s_selected_file))
                            ui_toast(T(TRANSFER_TOAST_RETRY_OK), T(TRANSFER_TOAST_RETRY_OK_BODY), CUI_TONE_INFO);
                        else
                            ui_toast(T(TRANSFER_TOAST_RETRY_FAIL), T(TRANSFER_TOAST_RETRY_FAIL_BODY), CUI_TONE_WARNING);
                    }
                }
            } else {
                cui_secondary(ui, T(TRANSFER_EMPTY_FILES));
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, T(TRANSFER_CARD_HISTORY), 3);
                cui_muted(ui, s_model.history_total > s_model.history_count ?
                    T(TRANSFER_HISTORY_RECENT) : T(TRANSFER_HISTORY_ALL));
            }
            if (s_model.history_count == 0) {
                cui_secondary(ui, T(TRANSFER_EMPTY_HISTORY));
            } else if (cui_table_begin_ex(ui, "hopdesk-transfer-history", history_columns, 5,
                       &(cui_table_opts){.height = CUI_PX(220), .striped = true, .compact = true})) {
                for (i = 0; i < s_model.history_count; ++i) {
                    const hop_xfer_task_history_record *entry = &s_model.history[i];
                    char files[48], bytes[64];
                    snprintf(files, sizeof(files), "%u / %u", entry->files_done, entry->files_total);
                    format_bytes(entry->bytes_total, bytes, sizeof(bytes));
                    (void)cui_table_row(ui, false);
                    cui_table_cell_text(ui, entry->name[0] ? entry->name : T(TRANSFER_DEFAULT_NAME));
                    cui_table_cell_text(ui, entry->direction == HOP_XFER_DIR_SEND ? T(TRANSFER_SEND) : T(TRANSFER_RECEIVE));
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

static void apply_ui_lang(int ui_lang, int persist) {
    if (ui_lang < HOP_LANG_AUTO || ui_lang > HOP_LANG_EN)
        ui_lang = HOP_LANG_AUTO;
    s_model.config.ui_lang = ui_lang;
    s_settings.cfg.ui_lang = ui_lang;
    s_settings.ui_lang = ui_lang;
    hop_i18n_set_lang(ui_lang);
    if (s_ops && s_ops->apply_lang) {
        s_ops->apply_lang(ui_lang, persist);
    }
    if (s_ctx) cui_request_redraw(s_ctx);
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
    s_settings.ui_lang = (s_model.config.ui_lang >= HOP_LANG_AUTO &&
                          s_model.config.ui_lang <= HOP_LANG_EN)
        ? s_model.config.ui_lang : HOP_LANG_AUTO;
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
    if (!s_settings.saving) snprintf(s_settings.note, sizeof(s_settings.note), "%s", T(SETTINGS_NOTE_UNSAVED));
}

static void settings_reset(void) {
    if (s_ops && s_ops->default_config) s_ops->default_config(&s_settings.cfg);
    else hop_config_defaults(&s_settings.cfg);
    s_model.config = s_settings.cfg;
    s_settings.loaded = 0;
    settings_load();
    apply_ui_theme(s_ctx, s_settings.ui_theme, 0);
    apply_ui_lang(s_settings.ui_lang, 0);
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
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", T(SETTINGS_ERR_PORTS));
        return 0;
    }
    if (!parse_u16(s_settings.disp_win, &win_input) ||
        !parse_u16(s_settings.disp_mac, &mac_input)) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", T(SETTINGS_ERR_DISP_INPUT));
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
    out->ui_lang = (s_settings.ui_lang >= HOP_LANG_AUTO &&
                    s_settings.ui_lang <= HOP_LANG_EN)
        ? s_settings.ui_lang : HOP_LANG_AUTO;
    out->log_level = (s_settings.log_level >= HOP_LOG_INFO &&
                      s_settings.log_level <= HOP_LOG_OFF)
        ? s_settings.log_level : HOP_LOG_INFO;
    out->update_channel = s_settings.update_channel == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;

    if (!hop_config_validate(out, 1, err, sizeof(err))) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", err[0] ? err : T(SETTINGS_ERR_VALIDATE));
        return 0;
    }
    return 1;
}

static void settings_save(void) {
    hop_config config;
    if (s_settings.saving) return;
    if (!settings_build_config(&config)) {
        ui_toast(T(TOAST_INVALID_TITLE), s_settings.note, CUI_TONE_WARNING);
        return;
    }
    s_settings.cfg = config;
    s_settings.saving = 1;
    snprintf(s_settings.note, sizeof(s_settings.note), "%s", T(SETTINGS_NOTE_SAVING));
    if (s_ops && s_ops->save_config_async) s_ops->save_config_async(&config);
    else hopdesk_ui_save_complete(0, T(SETTINGS_NOTE_NO_SERVICE));
}

static void draw_settings_bar(cui_ctx *ui) {
    const char *tabs[5] = {
        T(SETTINGS_TAB_CONN), T(SETTINGS_TAB_INPUT), T(SETTINGS_TAB_AUDIO),
        T(SETTINGS_TAB_TRANSFER), T(SETTINGS_TAB_MAINT)
    };
    if (!s_settings.loaded) settings_load();
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S) && s_settings.dirty && !s_settings.saving)
        settings_save();

    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10, .wrap = true) {
        if (cui_tabs_ex(ui, "hopdesk-settings-tabs", tabs, 5, &s_settings.tab,
                &(cui_tabs_opts){.variant = CUI_TABS_PILL, .icons = k_settings_tab_icons})) {
            s_scroll_reset_pending = 1;
        }
        cui_spacer(ui);
        if (s_settings.dirty && !s_settings.saving) {
            cui_badge_ex(ui, T(SETTINGS_UNSAVED), &(cui_badge_opts){.tone = CUI_TONE_WARNING, .dot = true});
        }
        if (cui_button_ex(ui, T(BTN_RESET_DEFAULTS), &(cui_button_opts){
                .variant = CUI_BUTTON_OUTLINE,
                .size = CUI_SIZE_SM,
                .icon = CUI_ICON_ROTATE_CCW,
                .disabled = s_settings.saving
            })) {
            settings_reset();
        }
        if (cui_button_ex(ui, s_settings.saving ? T(BTN_SAVING) : T(BTN_SAVE), &(cui_button_opts){
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
    if (s_settings.note[0]) cui_muted(ui, s_settings.note);
}

static void draw_settings_connection_and_appearance(cui_ctx *ui) {
    const char *connect_modes[2] = { T(CONNECT_MODE_SERVER), T(CONNECT_MODE_CLIENT) };
    const char *themes[3] = { T(THEME_SYSTEM), T(THEME_LIGHT), T(THEME_DARK) };
    const char *langs[3] = { T(LANG_AUTO), T(LANG_ZH_CN), T(LANG_EN) };
    static const char *const k_lang_icons[3] = { CUI_ICON_GLOBE, NULL, NULL };

    cui_row(ui, .width = CUI_FILL, .gap = 14, .wrap = true, .align = CUI_ALIGN_START) {
        cui_card(ui, .width = CUI_GROW(3), .min_width = 360, .pad = 16, .gap = 12) {
            draw_heading_with_help(ui, T(CONN_CARD_TITLE), 3, "help-conn-card", T(CONN_CARD_TIP));
            cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_START) {
                if (cui_input(ui, "setting-device-name", s_settings.cfg.device_name,
                    sizeof(s_settings.cfg.device_name), .label = T(CONN_FIELD_DEVICE_NAME),
                    .placeholder = T(CONN_FIELD_DEVICE_PH), .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
                if (cui_select_ex(ui, "setting-connect-mode", connect_modes, 2, &s_settings.role,
                    &(cui_select_opts){.label = T(CONN_FIELD_ROLE), .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            if (cui_input(ui, "setting-peer-host", s_settings.cfg.peer_host,
                sizeof(s_settings.cfg.peer_host), .label = T(CONN_FIELD_PEER_HOST),
                .placeholder = T(CONN_FIELD_PEER_HOST_PH),
                .disabled = s_settings.role != HOP_ROLE_CLIENT, .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
            cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-control-port", &s_settings.port, 1, 65535,
                    &(cui_number_opts){.label = T(CONN_FIELD_PORT), .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-data-port", &s_settings.data_port, 1, 65535,
                    &(cui_number_opts){.label = T(CONN_FIELD_DATA_PORT), .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-audio-port", &s_settings.audio_port, 1, 65535,
                    &(cui_number_opts){.label = T(CONN_FIELD_AUDIO_PORT), .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            if (cui_input(ui, "setting-token", s_settings.cfg.token,
                sizeof(s_settings.cfg.token), .label = T(CONN_FIELD_TOKEN), .password = true,
                .placeholder = T(CONN_FIELD_TOKEN_PH), .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_row(ui, .align = CUI_ALIGN_CENTER, .gap = 4) {
                    cui_label(ui, T(CONN_SECTION_CONTROL));
                    draw_help_hint(ui, "help-conn-ctrl", T(CONN_SECTION_CONTROL_TIP));
                }
                cui_badge_ex(ui, link_status_text(),
                    &(cui_badge_opts){.tone = link_status_tone(), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (s_model.manually_disconnected) {
                    if (cui_button_ex(ui, T(BTN_RESUME_CONNECT), &(cui_button_opts){
                            .variant = CUI_BUTTON_PRIMARY,
                            .icon = CUI_ICON_LINK,
                            .disabled = !s_ops || !s_ops->reconnect
                        }))
                        action_reconnect();
                    (void)cui_button_ex(ui, T(BTN_DISCONNECTED), &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .icon = CUI_ICON_PAUSE,
                        .disabled = true
                    });
                } else {
                    if (cui_button_ex(ui, T(BTN_DISCONNECT), &(cui_button_opts){
                            .variant = CUI_BUTTON_DANGER,
                            .icon = CUI_ICON_PAUSE,
                            .disabled = !s_ops || !s_ops->disconnect
                        }))
                        action_disconnect();
                    if (cui_button_ex(ui, T(BTN_RECONNECT), &(cui_button_opts){
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
                draw_heading_with_help(ui, T(APPEARANCE_CARD_TITLE), 3, "help-appearance", T(APPEARANCE_CARD_TIP));
                cui_muted(ui, T(APPEARANCE_LABEL_THEME));
                if (cui_segmented_ex(ui, "setting-ui-theme", themes, 3, &s_settings.ui_theme,
                        &(cui_segmented_opts){.full_width = true, .icons = k_theme_icons})) {
                    apply_ui_theme(ui, s_settings.ui_theme, 1);
                }
                cui_muted(ui, T(APPEARANCE_LABEL_LANG));
                if (cui_segmented_ex(ui, "setting-ui-lang", langs, 3, &s_settings.ui_lang,
                        &(cui_segmented_opts){.full_width = true, .icons = k_lang_icons})) {
                    apply_ui_lang(s_settings.ui_lang, 1);
                }
            }
            if (s_model.autostart_supported) {
                bool autostart_on = s_model.autostart_enabled != 0;
#ifdef _WIN32
                const char *autostart_tip = T(AUTOSTART_TIP_WIN);
#else
                const char *autostart_tip = T(AUTOSTART_TIP_MAC);
#endif
                cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                        draw_heading_with_help(ui, T(AUTOSTART_CARD_TITLE), 3, "help-autostart", autostart_tip);
                        cui_badge_ex(ui, autostart_on ? T(STATUS_ON) : T(STATUS_DISABLED),
                            &(cui_badge_opts){.tone = autostart_on ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
                    }
                    if (cui_switch(ui, T(AUTOSTART_SWITCH), &autostart_on)) {
                        if (s_ops && s_ops->set_autostart) {
                            if (s_ops->set_autostart(autostart_on ? 1 : 0)) {
                                s_model.autostart_enabled = autostart_on ? 1 : 0;
                                ui_toast(autostart_on ? T(AUTOSTART_TOAST_ON) : T(AUTOSTART_TOAST_OFF),
                                         autostart_on ? T(AUTOSTART_TOAST_ON_BODY) : T(AUTOSTART_TOAST_OFF_BODY),
                                         CUI_TONE_SUCCESS);
                            } else {
                                ui_toast(T(AUTOSTART_TOAST_FAIL), T(AUTOSTART_TOAST_FAIL_BODY), CUI_TONE_WARNING);
                            }
                        }
                    }
                }
            }
            draw_permissions_card(ui);
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
            ui_toast(T(AUDIO_TOAST_SWITCHED),
                s_model.connected ? T(AUDIO_TOAST_SEND_ON) : T(AUDIO_TOAST_SEND_WAIT),
                CUI_TONE_SUCCESS);
        } else if (next == HOP_AUDIO_PLAY) {
            ui_toast(T(AUDIO_TOAST_SWITCHED),
                s_model.connected ? T(AUDIO_TOAST_PLAY_ON) : T(AUDIO_TOAST_PLAY_WAIT),
                CUI_TONE_SUCCESS);
        } else {
            ui_toast(T(AUDIO_TOAST_OFF),
                s_model.connected ? T(AUDIO_TOAST_OFF_BOTH) : T(AUDIO_TOAST_OFF_LOCAL),
                CUI_TONE_INFO);
        }
    } else {
        settings_mark_dirty();
    }
}

static void draw_settings_audio(cui_ctx *ui) {
    const char *audio_dirs[3] = { T(AUDIO_DIR_OFF), T(AUDIO_DIR_SEND), T(AUDIO_DIR_PLAY) };
    const char *peer_audio_text = T(AUDIO_PEER_UNKNOWN);
    cui_tone peer_audio_tone = CUI_TONE_NEUTRAL;
    if (s_model.connected && s_model.peer_capabilities_known) {
        if (s_model.peer_audio_mode == HOP_AUDIO_SEND) {
            peer_audio_text = T(AUDIO_PEER_SEND);
            peer_audio_tone = CUI_TONE_INFO;
        } else if (s_model.peer_audio_mode == HOP_AUDIO_PLAY) {
            peer_audio_text = T(AUDIO_PEER_PLAY);
            peer_audio_tone = CUI_TONE_INFO;
        } else if (s_model.peer_audio_mode == HOP_AUDIO_MODES) {
            peer_audio_text = T(AUDIO_PEER_DUPLEX);
            peer_audio_tone = CUI_TONE_WARNING;
        } else {
            peer_audio_text = T(AUDIO_PEER_OFF);
        }
    } else if (!s_model.connected) {
        peer_audio_text = T(AUDIO_PEER_DISCONNECTED);
    }

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                draw_heading_with_help(ui, T(AUDIO_CARD_TITLE), 3, "help-audio-dir", T(AUDIO_CARD_TIP));
                cui_badge_ex(ui, audio_direction_summary(),
                    &(cui_badge_opts){.tone = audio_direction_tone(), .dot = true});
            }
            if (cui_segmented_ex(ui, "setting-audio-direction", audio_dirs, 3,
                    &s_settings.audio_dir,
                    &(cui_segmented_opts){.full_width = true, .icons = k_audio_direction_icons})) {
                apply_audio_direction(s_settings.audio_dir);
            }

            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true, .align = CUI_ALIGN_CENTER) {
                cui_badge_ex(ui,
                    s_settings.audio_dir == 1 ? T(AUDIO_LOCAL_SEND) :
                    (s_settings.audio_dir == 2 ? T(AUDIO_LOCAL_PLAY) : T(AUDIO_LOCAL_OFF)),
                    &(cui_badge_opts){.tone = s_settings.audio_dir ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
                cui_badge_ex(ui, peer_audio_text, &(cui_badge_opts){.tone = peer_audio_tone});
                cui_badge_ex(ui,
                    s_model.audio_active ? T(AUDIO_CH_ACTIVE) : T(AUDIO_CH_IDLE),
                    &(cui_badge_opts){.tone = s_model.audio_active ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL});
                if (s_model.audio_unavailable) {
                    cui_badge_ex(ui, T(AUDIO_SUMMARY_UNAVAIL), &(cui_badge_opts){.tone = CUI_TONE_WARNING});
                }
                cui_spacer(ui);
#ifdef _WIN32
                draw_help_hint(ui, "help-audio-platform", T(AUDIO_TIP_WIN));
#else
                draw_help_hint(ui, "help-audio-platform", T(AUDIO_TIP_MAC));
                if (cui_button_ex(ui, T(PERM_BTN_AUDIO), &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .size = CUI_SIZE_SM,
                        .icon = CUI_ICON_SHIELD,
                        .disabled = !s_ops || !s_ops->permission_action
                    })) {
                    trigger_permission_action(HOPDESK_UI_PERM_SCREEN_AUDIO);
                }
#endif
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            if (cui_accordion_begin(ui, T(AUDIO_ADV_TITLE), &s_settings.audio_adv_open)) {
                cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                    if (cui_number_ex(ui, "setting-audio-keepalive", &s_settings.audio_keepalive_ms, 200, 10000,
                        &(cui_number_opts){.label = T(AUDIO_FIELD_KEEPALIVE), .step = 100, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-audio-timeout", &s_settings.audio_peer_timeout_ms, 500, 30000,
                        &(cui_number_opts){.label = T(AUDIO_FIELD_TIMEOUT), .step = 200, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-audio-send-limit", &s_settings.audio_send_limit_ms, 20, 2000,
                        &(cui_number_opts){.label = T(AUDIO_FIELD_SEND_LIMIT), .step = 20, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                        settings_mark_dirty();
                    draw_help_hint(ui, "help-audio-adv", T(AUDIO_ADV_TIP));
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
        ui_toast(T(DISP_TOAST_SET_WIN), label, CUI_TONE_SUCCESS);
    } else {
        snprintf(s_settings.disp_mac, sizeof(s_settings.disp_mac), "0x%02X", (unsigned)code);
        s_settings.mac_input_preset = preset >= 0 ? preset : pcount;
        ui_toast(T(DISP_TOAST_SET_MAC), label, CUI_TONE_SUCCESS);
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
    input_opts[opt_count++] = T(DISP_INPUT_CUSTOM_OPT);
    int custom_idx = opt_count - 1;
    if (s_settings.win_input_preset < 0 || s_settings.win_input_preset > custom_idx)
        s_settings.win_input_preset = custom_idx;
    if (s_settings.mac_input_preset < 0 || s_settings.mac_input_preset > custom_idx)
        s_settings.mac_input_preset = custom_idx;

    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            draw_heading_with_help(ui, T(DISP_CARD_TITLE), 3, "help-disp-card", T(DISP_CARD_TIP));
#ifdef _WIN32
            cui_badge_ex(ui, s_settings.shared_display ? T(DISP_BADGE_SHARED_ON) : T(DISP_BADGE_NORMAL),
                &(cui_badge_opts){.tone = s_settings.shared_display ? CUI_TONE_INFO : CUI_TONE_NEUTRAL, .dot = true});
#else
            cui_badge_ex(ui, inv->ddc_tool_ready ? T(DISP_BADGE_M1DDC_OK) : T(DISP_BADGE_M1DDC_NO),
                &(cui_badge_opts){.tone = inv->ddc_tool_ready ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .dot = true});
#endif
        }

        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            if (cui_button_ex(ui, T(DISP_BTN_IDENTIFY), &(cui_button_opts){
                    .variant = CUI_BUTTON_SECONDARY,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_MONITOR,
                    .disabled = !s_ops || !s_ops->display_action
                })) {
                if (s_ops && s_ops->display_action) {
                    s_ops->display_action(HOPDESK_UI_DISPLAY_IDENTIFY);
                    ui_toast(T(DISP_TOAST_IDENTIFY), T(DISP_TOAST_IDENTIFY_BODY), CUI_TONE_INFO);
                }
            }
            if (cui_button_ex(ui, inv->probing ? T(DISP_BTN_PROBING) : T(DISP_BTN_PROBE), &(cui_button_opts){
                    .variant = CUI_BUTTON_PRIMARY,
                    .size = CUI_SIZE_SM,
                    .icon = CUI_ICON_REFRESH_CW,
                    .loading = inv->probing != 0,
                    .disabled = inv->probing != 0 || !s_ops || !s_ops->display_action
                })) {
                if (s_ops && s_ops->display_action) {
                    s_ops->display_action(HOPDESK_UI_DISPLAY_PROBE);
                    ui_toast(T(DISP_TOAST_PROBE), T(DISP_TOAST_PROBE_BODY), CUI_TONE_INFO);
                }
            }
#ifndef _WIN32
            if (!inv->ddc_tool_ready && s_ops && s_ops->copy_text) {
                if (cui_button_ex(ui, T(DISP_BTN_COPY_M1DDC), &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .size = CUI_SIZE_SM,
                        .icon = CUI_ICON_COPY
                    })) {
                    if (s_ops->copy_text("brew install m1ddc"))
                        ui_toast(T(TOAST_COPIED_CMD), T(TOAST_COPIED_CMD_BODY), CUI_TONE_SUCCESS);
                }
            }
#endif
        }

        if (inv->probe_note[0]) cui_muted(ui, inv->probe_note);

        if (inv->count > 0) {
            cui_column(ui, .width = CUI_FILL, .gap = 6) {
                cui_muted(ui, T(DISP_DETECTED_TITLE));
                for (int i = 0; i < inv->count && i < HOP_DISPLAY_MAX_DEVICES; ++i) {
                    char mon_label[192], row_id[40];
                    hop_display_format_monitor_label(&inv->items[i], mon_label, sizeof(mon_label));
                    snprintf(row_id, sizeof(row_id), "##disp-inv-%d", i);
                    cui_push_id(ui, row_id);
                    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 8) {
                        cui_text(ui, mon_label);
                        if (inv->items[i].current_input >= 0 && inv->items[i].current_input <= 65535) {
#ifdef _WIN32
                            if (cui_button_ex(ui, T(DISP_BTN_SET_WIN), &(cui_button_opts){
                                    .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM })) {
                                apply_detected_input_source(1, (uint16_t)inv->items[i].current_input);
                            }
#else
                            if (cui_button_ex(ui, T(DISP_BTN_SET_MAC), &(cui_button_opts){
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
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 6) {
            if (cui_switch(ui, T(DISP_SWITCH_SHARED), &s_settings.shared_display))
                settings_mark_dirty();
            draw_help_hint(ui, "help-disp-shared", T(DISP_SWITCH_SHARED_TIP));
        }
        if (s_settings.shared_display) {
            char mon_bufs[HOP_DISPLAY_MAX_DEVICES + 1][192];
            char mon_devs[HOP_DISPLAY_MAX_DEVICES + 1][32];
            const char *mon_ptrs[HOP_DISPLAY_MAX_DEVICES + 1];
            int mon_count = 0, mon_sel = 0, matched_saved = 0;

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
                         T(DISP_SAVED_OFFLINE), s_settings.cfg.disp_device);
                snprintf(mon_devs[mon_count], sizeof(mon_devs[mon_count]), "%s", s_settings.cfg.disp_device);
                mon_ptrs[mon_count] = mon_bufs[mon_count];
                mon_sel = mon_count;
                ++mon_count;
            }
            if (mon_count > 0) {
                if (!s_settings.cfg.disp_device[0]) {
                    snprintf(s_settings.cfg.disp_device, sizeof(s_settings.cfg.disp_device), "%s", mon_devs[0]);
                }
                if (cui_select_ex(ui, "setting-shared-monitor", mon_ptrs, mon_count, &mon_sel,
                    &(cui_select_opts){.label = T(DISP_FIELD_SHARED_MON), .width = CUI_FILL})) {
                    snprintf(s_settings.cfg.disp_device, sizeof(s_settings.cfg.disp_device), "%s", mon_devs[mon_sel]);
                    settings_mark_dirty();
                }
            } else {
                cui_secondary(ui, T(DISP_NO_EXT_HINT));
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
                    snprintf(mac_bufs[mac_count], sizeof(mac_bufs[mac_count]), T(DISP_MAC_FALLBACK_OPT), n, n);
                    mac_indices[mac_count] = n;
                    mac_ptrs[mac_count] = mac_bufs[mac_count];
                    if (parsed_idx == n) mac_sel = mac_count;
                    ++mac_count;
                }
            }
            snprintf(mac_bufs[mac_count], sizeof(mac_bufs[mac_count]), "%s", T(DISP_MAC_CUSTOM_OPT));
            mac_indices[mac_count] = 0;
            mac_ptrs[mac_count] = mac_bufs[mac_count];
            if (mac_sel < 0) mac_sel = mac_count;
            ++mac_count;

            if (cui_select_ex(ui, "setting-mac-target-display", mac_ptrs, mac_count, &mac_sel,
                &(cui_select_opts){.label = T(DISP_FIELD_MAC_TARGET), .width = CUI_FILL})) {
                if (mac_indices[mac_sel] > 0) {
                    hop_display_mac_build_input_cmd(mac_indices[mac_sel],
                        s_settings.cfg.mac_input_cmd, sizeof(s_settings.cfg.mac_input_cmd));
                    settings_mark_dirty();
                } else {
                    s_settings.mac_cmd_open = true;
                }
            }
        }
#endif

        cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
            if (cui_select_ex(ui, "setting-disp-win-preset", input_opts, opt_count, &s_settings.win_input_preset,
                &(cui_select_opts){.label = T(DISP_FIELD_WIN_INPUT), .width = CUI_GROW(1)})) {
                if (s_settings.win_input_preset < pcount) {
                    const hop_display_input_preset *p = hop_display_input_preset_at((size_t)s_settings.win_input_preset);
                    if (p) snprintf(s_settings.disp_win, sizeof(s_settings.disp_win), "0x%02X", (unsigned)p->code);
                }
                settings_mark_dirty();
            }
            if (cui_select_ex(ui, "setting-disp-mac-preset", input_opts, opt_count, &s_settings.mac_input_preset,
                &(cui_select_opts){.label = T(DISP_FIELD_MAC_INPUT), .width = CUI_GROW(1)})) {
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
                        .label = T(DISP_FIELD_WIN_CUSTOM), .placeholder = "0x0F", .width = CUI_GROW(1)) & CUI_CHANGED)
                        settings_mark_dirty();
                }
                if (s_settings.mac_input_preset == custom_idx) {
                    if (cui_input(ui, "setting-disp-mac", s_settings.disp_mac, sizeof(s_settings.disp_mac),
                        .label = T(DISP_FIELD_MAC_CUSTOM), .placeholder = "0x11", .width = CUI_GROW(1)) & CUI_CHANGED)
                        settings_mark_dirty();
                }
            }
        }

        if (cui_accordion_begin(ui, T(DISP_ADV_TITLE), &s_settings.mac_cmd_open)) {
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-wake-sec", &s_settings.wake_sec, 1, 3600,
                    &(cui_number_opts){.label = T(DISP_FIELD_WAKE_SEC), .step = 60, .decimals = 0, .suffix = "s", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-ddc-retries", &s_settings.ddc_retries, 1, 10,
                    &(cui_number_opts){.label = T(DISP_FIELD_DDC_RETRIES), .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-verify-timeout", &s_settings.verify_timeout_ms, 1000, 30000,
                    &(cui_number_opts){.label = T(DISP_FIELD_VERIFY_TIMEOUT), .step = 500, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-disp-poll", &s_settings.poll_ms, 50, 2000,
                    &(cui_number_opts){.label = T(DISP_FIELD_POLL_MS), .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wake-settle", &s_settings.wake_settle_ms, 0, 10000,
                    &(cui_number_opts){.label = T(DISP_FIELD_WAKE_SETTLE), .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wake-timeout", &s_settings.wake_timeout_ms, 1000, 30000,
                    &(cui_number_opts){.label = T(DISP_FIELD_WAKE_TIMEOUT), .step = 500, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-wake-delay", &s_settings.wake_delay_ms, 0, 10000,
                    &(cui_number_opts){.label = T(DISP_FIELD_WAKE_DELAY), .step = 100, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
#ifdef _WIN32
                if (cui_number_ex(ui, "setting-disp-monitor-index", &s_settings.monitor_index, 0, 15,
                    &(cui_number_opts){.label = T(DISP_FIELD_MON_INDEX), .step = 1, .decimals = 0, .width = CUI_GROW(1)}))
                    settings_mark_dirty();
#endif
            }
#ifdef _WIN32
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 6) {
                if (cui_switch(ui, T(DISP_SWITCH_SLEEP_FB), &s_settings.sleep_fallback))
                    settings_mark_dirty();
                draw_help_hint(ui, "help-disp-sleep-fb", T(DISP_SWITCH_SLEEP_FB_TIP));
            }
            if (cui_input(ui, "setting-shared-device", s_settings.cfg.disp_device,
                sizeof(s_settings.cfg.disp_device), .label = T(DISP_FIELD_MANUAL_DEV),
                .placeholder = "\\\\.\\DISPLAY2", .width = CUI_FILL) & CUI_CHANGED)
                settings_mark_dirty();
#endif
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_input(ui, "setting-mac-input-cmd", s_settings.cfg.mac_input_cmd,
                    sizeof(s_settings.cfg.mac_input_cmd), .label = T(DISP_FIELD_MAC_INPUT_CMD),
                    .placeholder = "m1ddc display 1 set input", .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
                if (cui_input(ui, "setting-mac-sleep-cmd", s_settings.cfg.mac_sleep_cmd,
                    sizeof(s_settings.cfg.mac_sleep_cmd), .label = T(DISP_FIELD_MAC_SLEEP_CMD),
                    .placeholder = "pmset displaysleepnow", .width = CUI_GROW(1)) & CUI_CHANGED)
                    settings_mark_dirty();
            }
            cui_accordion_end(ui);
        }
    }
}

static void draw_settings_input_and_display(cui_ctx *ui) {
    const char *edges[4] = { T(EDGE_LEFT), T(EDGE_RIGHT), T(EDGE_TOP), T(EDGE_BOTTOM) };

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                draw_heading_with_help(ui, T(INPUT_CARD_TITLE), 3, "help-input-card", T(INPUT_CARD_TIP));
                cui_badge_ex(ui, input_summary_text(),
                    &(cui_badge_opts){.tone = input_summary_tone(), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 12) {
                cui_row(ui, .align = CUI_ALIGN_CENTER, .gap = 6) {
                    if (cui_switch(ui, T(INPUT_SWITCH_MOUSE), &s_settings.provide_mouse)) {
                        unsigned next = (s_settings.provide_mouse ? HOP_INPUT_MOUSE : 0u) |
                                        (s_settings.provide_keyboard ? HOP_INPUT_KEYBOARD : 0u);
                        apply_input_mode(next, T(TOAST_INPUT_LOCAL_MOUSE), T(TOAST_INPUT_MOUSE_OFF),
                                         s_settings.provide_mouse);
                    }
                    draw_help_hint(ui, "help-input-mouse", T(INPUT_SWITCH_MOUSE_TIP));
                }
                cui_badge_ex(ui, capability_owner_text(HOP_INPUT_MOUSE),
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_MOUSE), .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .gap = 12) {
                cui_row(ui, .align = CUI_ALIGN_CENTER, .gap = 6) {
                    if (cui_switch(ui, T(INPUT_SWITCH_KBD), &s_settings.provide_keyboard)) {
                        unsigned next = (s_settings.provide_mouse ? HOP_INPUT_MOUSE : 0u) |
                                        (s_settings.provide_keyboard ? HOP_INPUT_KEYBOARD : 0u);
                        apply_input_mode(next, T(TOAST_INPUT_LOCAL_KBD), T(TOAST_INPUT_KBD_OFF),
                                         s_settings.provide_keyboard);
                    }
                    draw_help_hint(ui, "help-input-kbd", T(INPUT_SWITCH_KBD_TIP));
                }
                cui_badge_ex(ui, capability_owner_text(HOP_INPUT_KEYBOARD),
                    &(cui_badge_opts){.tone = capability_owner_tone(HOP_INPUT_KEYBOARD), .dot = true});
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            draw_heading_with_help(ui, T(EDGE_CARD_TITLE), 3, "help-edge-card", T(EDGE_CARD_TIP));
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_select_ex(ui, "setting-server-edge", edges, 4, &s_settings.server_edge,
                    &(cui_select_opts){.label = T(EDGE_FIELD_OUT), .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_select_ex(ui, "setting-client-edge", edges, 4, &s_settings.client_edge,
                    &(cui_select_opts){.label = T(EDGE_FIELD_IN), .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-switch-delay", &s_settings.switch_delay, 0, 5000,
                    &(cui_number_opts){.label = T(EDGE_FIELD_DELAY), .step = 10, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-cooldown", &s_settings.cooldown, 0, 10000,
                    &(cui_number_opts){.label = T(EDGE_FIELD_COOLDOWN), .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-margin", &s_settings.margin, 0, 1000,
                    &(cui_number_opts){.label = T(EDGE_FIELD_MARGIN), .step = 1, .decimals = 0, .suffix = "px", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-mouse-scale", &s_settings.mouse_scale, 25, 400,
                    &(cui_number_opts){.label = T(EDGE_FIELD_MOUSE_SCALE), .step = 5, .decimals = 0, .suffix = "%", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-wheel-scale", &s_settings.wheel_scale, 25, 400,
                    &(cui_number_opts){.label = T(EDGE_FIELD_WHEEL_SCALE), .step = 5, .decimals = 0, .suffix = "%", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 6) {
                if (cui_switch(ui, T(EDGE_SWITCH_INVERT), &s_settings.wheel_invert))
                    settings_mark_dirty();
                draw_help_hint(ui, "help-wheel-invert", T(EDGE_SWITCH_INVERT_TIP));
            }
        }

        draw_display_settings_card(ui);
    }
}

static void draw_settings_transfer(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            draw_heading_with_help(ui, T(XFER_CARD_TITLE), 3, "help-xfer-tuning", T(XFER_CARD_TIP));
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_START) {
                if (cui_number_ex(ui, "setting-chunk-kb", &s_settings.chunk_kb, 64, 4096,
                    &(cui_number_opts){.label = T(XFER_FIELD_CHUNK), .step = 64, .decimals = 0, .suffix = "KB", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
                if (cui_number_ex(ui, "setting-rate-limit", &s_settings.rate_limit, 16, 4096,
                    &(cui_number_opts){.label = T(XFER_FIELD_RATE), .step = 16, .decimals = 0, .suffix = "MiB/s", .width = CUI_GROW(1)}))
                    settings_mark_dirty();
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            draw_heading_with_help(ui, T(CLIP_CARD_TITLE), 3, "help-clip-sync", T(CLIP_CARD_TIP));
            if (cui_switch(ui, T(CLIP_SWITCH_TEXT), &s_settings.clip_text)) settings_mark_dirty();
            if (cui_switch(ui, T(CLIP_SWITCH_IMAGE), &s_settings.clip_image)) settings_mark_dirty();
            if (cui_switch(ui, T(CLIP_SWITCH_FILE), &s_settings.clip_file)) settings_mark_dirty();
        }
    }
}

static void draw_settings_maintenance(cui_ctx *ui) {
    const hop_update_snapshot *up = &s_model.update;
    const char *channels[2] = { T(CHANNEL_STABLE_OPT), T(CHANNEL_DEV_OPT) };
    const char *log_levels[4] = { T(LOG_INFO), T(LOG_WARN), T(LOG_ERROR), T(LOG_OFF) };
    int busy = up->phase == HOP_UPDATE_CHECKING ||
               up->phase == HOP_UPDATE_DOWNLOADING ||
               up->phase == HOP_UPDATE_APPLYING;
    int can_download = !busy && up->target_version[0] &&
                       (up->phase == HOP_UPDATE_AVAILABLE ||
                        (up->phase == HOP_UPDATE_ERROR && up->target_tag[0]));
    int can_apply = up->phase == HOP_UPDATE_READY;
    char current_info[192], target_info[224], dl_detail[128];

    snprintf(current_info, sizeof(current_info), T(UPD_CURRENT_LINE),
        HOP_VERSION, current_channel_label(), HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);

    cui_column(ui, .width = CUI_FILL, .gap = 14) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                draw_heading_with_help(ui, T(UPD_CARD_TITLE), 3, "help-upd-card", T(UPD_CARD_TIP));
                cui_badge_ex(ui, update_phase_text(up->phase),
                    &(cui_badge_opts){.tone = update_phase_tone(up->phase), .dot = true});
            }
            cui_text(ui, current_info);
            if (cui_segmented_ex(ui, "setting-update-channel", channels, 2,
                    &s_settings.update_channel, &(cui_segmented_opts){.full_width = true})) {
                settings_mark_dirty();
            }

            if (up->target_version[0]) {
                if (up->target_ctrl_proto > 0 && up->target_xfer_proto > 0) {
                    snprintf(target_info, sizeof(target_info), T(UPD_TARGET_LINE_PROTO),
                        up->target_version, update_channel_label(up->target_channel),
                        up->target_ctrl_proto, up->target_xfer_proto);
                } else {
                    snprintf(target_info, sizeof(target_info), T(UPD_TARGET_LINE),
                        up->target_version, update_channel_label(up->target_channel));
                }
                cui_separator(ui);
                cui_text(ui, target_info);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                    if (up->is_channel_switch)
                        cui_badge_ex(ui, T(UPD_BADGE_CROSS_CHANNEL), &(cui_badge_opts){.tone = CUI_TONE_INFO});
                    if (up->protocol_changed)
                        cui_badge_ex(ui, T(UPD_BADGE_PROTO_CHANGED), &(cui_badge_opts){.tone = CUI_TONE_WARNING});
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
                    snprintf(dl_detail, sizeof(dl_detail), T(UPD_DL_KNOWN), done_str, total_str);
                } else {
                    snprintf(dl_detail, sizeof(dl_detail), T(UPD_DL_UNKNOWN), done_str);
                }
                cui_progress_ex(ui, ratio, &(cui_progress_opts){.width = CUI_FILL, .height = 7, .show_value = true});
                cui_muted(ui, dl_detail);
            }

            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_CHECKING ? T(UPD_BTN_CHECKING) : T(UPD_BTN_CHECK), &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_REFRESH_CW,
                        .loading = up->phase == HOP_UPDATE_CHECKING,
                        .disabled = busy || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_CHECK, s_settings.update_channel);
                }
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_DOWNLOADING ? T(UPD_BTN_DOWNLOADING) : T(UPD_BTN_DOWNLOAD), &(cui_button_opts){
                        .variant = can_download ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                        .loading = up->phase == HOP_UPDATE_DOWNLOADING,
                        .disabled = !can_download || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_DOWNLOAD, up->target_channel);
                }
                if (cui_button_ex(ui, up->phase == HOP_UPDATE_APPLYING ? T(UPD_PHASE_APPLYING) : T(UPD_BTN_APPLY), &(cui_button_opts){
                        .variant = CUI_BUTTON_PRIMARY,
                        .loading = up->phase == HOP_UPDATE_APPLYING,
                        .disabled = !can_apply || !s_ops || !s_ops->update_action
                    })) {
                    if (s_ops && s_ops->update_action)
                        s_ops->update_action(HOP_UPDATE_ACTION_APPLY, up->target_channel);
                }
                if (up->notes_url[0]) {
                    if (cui_button_ex(ui, T(UPD_BTN_COPY_NOTES), &(cui_button_opts){
                            .variant = CUI_BUTTON_OUTLINE,
                            .icon = CUI_ICON_COPY,
                            .disabled = !s_ops || !s_ops->copy_text
                        }) && s_ops && s_ops->copy_text) {
                        if (s_ops->copy_text(up->notes_url))
                            ui_toast(T(TOAST_COPIED_LINK), T(TOAST_COPIED_LINK_BODY), CUI_TONE_SUCCESS);
                    }
                }
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_heading(ui, T(LOGS_CARD_TITLE), 3);
            if (cui_select_ex(ui, "setting-log-level", log_levels, 4, &s_settings.log_level,
                &(cui_select_opts){.label = T(LOGS_FIELD_LEVEL), .width = CUI_FILL}))
                settings_mark_dirty();
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button_ex(ui, T(LOGS_BTN_GO_PERM), &(cui_button_opts){
                        .variant = CUI_BUTTON_SECONDARY,
                        .icon = CUI_ICON_SHIELD
                    }))
                    s_settings.tab = 0;
#ifndef _WIN32
                if (cui_button_ex(ui, T(PERM_BTN_CHECK), &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .icon = CUI_ICON_SHIELD,
                        .disabled = !s_ops || !s_ops->request_permissions
                    }))
                    if (s_ops && s_ops->request_permissions) s_ops->request_permissions();
#endif
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, T(LOGS_VIEW_TITLE), 3);
                cui_row(ui, .gap = 8) {
                    if (cui_button_ex(ui, T(BTN_REFRESH_LOGS), &(cui_button_opts){
                            .variant = CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_REFRESH_CW
                        }) && s_ops && s_ops->load_logs) {
                        if (s_ops->load_logs(s_model.logs, sizeof(s_model.logs)))
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", T(LOGS_VIEW_UPDATED));
                        else
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", T(LOGS_VIEW_FAILED));
                        cui_request_redraw(ui);
                    }
                    if (cui_button_ex(ui, T(BTN_COPY_LOGS), &(cui_button_opts){
                            .variant = CUI_BUTTON_SECONDARY,
                            .size = CUI_SIZE_SM,
                            .icon = CUI_ICON_COPY,
                            .disabled = !s_model.logs[0]
                        }) && s_ops && s_ops->copy_text && s_model.logs[0]) {
                        if (s_ops->copy_text(s_model.logs))
                            ui_toast(T(TOAST_COPIED_LOGS), T(TOAST_COPIED_LOGS_BODY), CUI_TONE_SUCCESS);
                        else
                            ui_toast(T(TOAST_COPY_FAILED), T(TOAST_COPY_FAILED_BODY), CUI_TONE_WARNING);
                    }
                }
            }
            if (s_model.log_note[0]) cui_muted(ui, s_model.log_note);
            if (s_model.logs[0]) {
                cui_textarea(ui, "hopdesk-log-view", s_model.logs, sizeof(s_model.logs),
                    .read_only = true, .height = 360, .width = CUI_FILL);
            } else {
                cui_secondary(ui, T(LOGS_VIEW_EMPTY));
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
            cui_secondary(ui, T(APP_SUBTITLE));
            cui_separator(ui);
            cui_nav_section(ui, T(NAV_SECTION_WORKSPACE));
            if (cui_nav_item_ex(ui, T(NAV_OVERVIEW), &(cui_nav_opts){.icon = CUI_ICON_LAYOUT_DASHBOARD, .selected = s_page == HOPDESK_UI_OVERVIEW}))
                hopdesk_ui_show_page(HOPDESK_UI_OVERVIEW);
            if (cui_nav_item_ex(ui, T(NAV_DEVICE), &(cui_nav_opts){.icon = CUI_ICON_MONITOR, .selected = s_page == HOPDESK_UI_DEVICE}))
                hopdesk_ui_show_page(HOPDESK_UI_DEVICE);
            if (cui_nav_item_ex(ui, T(NAV_TRANSFER), &(cui_nav_opts){.icon = CUI_ICON_REPEAT, .selected = s_page == HOPDESK_UI_TRANSFER,
                .badge = s_model.task_total > 0 ? "●" : NULL}))
                hopdesk_ui_show_page(HOPDESK_UI_TRANSFER);
            cui_nav_section(ui, T(NAV_SECTION_PREFS));
            if (cui_nav_item_ex(ui, T(NAV_SETTINGS), &(cui_nav_opts){.icon = CUI_ICON_SETTINGS, .selected = s_page == HOPDESK_UI_SETTINGS,
                .badge = (s_settings.dirty ||
                          s_model.update.phase == HOP_UPDATE_AVAILABLE ||
                          s_model.update.phase == HOP_UPDATE_READY) ? "●" : NULL}))
                hopdesk_ui_show_page(HOPDESK_UI_SETTINGS);
            cui_spacer(ui);
            cui_separator(ui);
            {
                char ver_line[96], proto_line[96];
                snprintf(ver_line, sizeof(ver_line), "v%s (%s)", HOP_VERSION, current_channel_label());
                snprintf(proto_line, sizeof(proto_line), T(VER_PROTO_LINE),
                         HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER);
                cui_secondary(ui, ver_line);
                cui_muted(ui, proto_line);
            }
            cui_muted(ui, T(APP_TAGLINE));
        }

        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 22, .gap = 14) {
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                bool is_dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
                const char *theme_tip = s_model.config.ui_theme == HOP_THEME_SYSTEM
                    ? (is_dark ? T(THEME_TIP_SYSTEM_DARK) : T(THEME_TIP_SYSTEM_LIGHT))
                    : (is_dark ? T(THEME_TIP_DARK) : T(THEME_TIP_LIGHT));
                cui_row(ui, .width = CUI_GROW(1), .align = CUI_ALIGN_CENTER, .gap = 6) {
                    cui_heading(ui, page_title(s_page), 2);
                    draw_help_hint(ui, "help-page-header", page_tip(s_page));
                }
                cui_push_id(ui, "##hopdesk-lang-toggle");
                if (cui_icon_button_ex(ui, CUI_ICON_GLOBE, &(cui_button_opts){
                        .variant = CUI_BUTTON_OUTLINE,
                        .size = CUI_SIZE_SM,
                        .tooltip = hop_i18n_active_lang() == HOP_LANG_EN ? "Switch to 简体中文" : "Switch to English"
                    })) {
                    apply_ui_lang(hop_i18n_active_lang() == HOP_LANG_EN ? HOP_LANG_ZH_CN : HOP_LANG_EN, 1);
                }
                cui_pop_id(ui);
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
                    case HOPDESK_UI_DEVICE:   draw_device(ui); break;
                    case HOPDESK_UI_TRANSFER: draw_transfer(ui); break;
                    case HOPDESK_UI_SETTINGS: draw_settings(ui); break;
                    default:                  draw_overview(ui); break;
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
    if (page != s_page) s_scroll_reset_pending = 1;
    s_page = page;
    if (s_ctx) cui_request_redraw(s_ctx);
}

void hopdesk_ui_save_complete(int ok, const char *message) {
    s_settings.saving = 0;
    if (ok) {
        s_settings.dirty = 0;
        s_model.config = s_settings.cfg;
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", message ? message : T(TOAST_SAVED_TITLE));
        ui_toast(T(TOAST_SAVED_TITLE), s_settings.note, CUI_TONE_SUCCESS);
    } else {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", message ? message : T(TOAST_SAVE_FAILED_TITLE));
        ui_toast(T(TOAST_SAVE_FAILED_TITLE), s_settings.note, CUI_TONE_DANGER);
    }
    if (s_ctx) cui_request_redraw(s_ctx);
}

int hopdesk_ui_settings_dirty(void) {
    return s_settings.dirty;
}
