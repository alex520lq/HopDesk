#include "hopdesk_ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const hopdesk_ui_ops *s_ops;
static hopdesk_ui_model s_model;
static cui_ctx *s_ctx;
static int s_page = HOPDESK_UI_OVERVIEW;
static int s_force_refresh = 1;
static hop_xfer_task_id s_selected_task;
static int s_selected_file = -1;
static uint32_t s_file_offset;

typedef struct {
    hop_config cfg;
    double port, data_port, switch_delay, cooldown, margin, wake_sec;
    double chunk_kb, rate_limit;
    char disp_win[16], disp_mac[16];
    int role, server_edge, client_edge;
    bool audio_send, audio_play;
    int loaded, dirty, saving;
    char note[256];
} hopdesk_ui_settings;

static hopdesk_ui_settings s_settings;

static const char *const k_pages[HOPDESK_UI_PAGE_COUNT] = {
    "概览", "设备", "传输", "设置"
};
static const char *const k_roles[] = {
    "服务器 · 提供键鼠", "客户端 · 接收键鼠"
};
static const char *const k_edges[] = {
    "左边缘", "右边缘", "上边缘", "下边缘"
};

static void ui_toast(const char *title, const char *detail, cui_tone tone) {
    if (!s_ctx) return;
    cui_toast_ex(s_ctx, title, &(cui_toast_opts){
        .description = detail,
        .tone = tone,
        .duration = 4.0f
    });
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
        (s_model.config.role == HOP_ROLE_CLIENT ? "尚未配置服务器" : "等待客户端连接"));
    snprintf(control, sizeof(control), "控制端口 %u", (unsigned)s_model.config.port);
    snprintf(data, sizeof(data), "数据端口 %u", (unsigned)s_model.config.data_port);
    snprintf(audio, sizeof(audio), "请求 %s · 活跃 %s",
        s_model.audio_requested ? "已配置" : "关闭",
        s_model.audio_active ? "是" : "否");

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_heading(ui, "设备关系", 2);
        cui_text(ui, "连接、控制权、文件通道和声音通道的实时状态。");
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true) {
            draw_stat_card(ui, "控制连接",
                s_model.connected ? "已连接" : "未连接",
                peer, s_model.connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL);
            draw_stat_card(ui, "文件通道",
                s_model.data_connected ? "已连接" : "未连接",
                data, s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL);
            draw_stat_card(ui, "键鼠控制权",
                s_model.sharing ? "对端控制" : "本机控制",
                s_model.sharing ? "正在共享" : "等待切换",
                s_model.sharing ? CUI_TONE_INFO : CUI_TONE_NEUTRAL);
            draw_stat_card(ui, "声音通道",
                s_model.audio_active ? "运行中" : "未运行",
                audio, s_model.audio_active ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL);
        }

        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 12) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "本机与对端", 3);
                cui_badge_ex(ui,
                    s_model.connected ? "在线" : "离线",
                    &(cui_badge_opts){.tone = s_model.connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
            }
            cui_row(ui, .width = CUI_FILL, .gap = 18, .wrap = true) {
                cui_column(ui, .width = CUI_GROW(1), .gap = 4) {
                    cui_muted(ui, "本机设备");
                    cui_label(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk");
                    cui_secondary(ui, s_model.config.role == HOP_ROLE_SERVER ? "服务器 · 提供键鼠" : "客户端 · 接收键鼠");
                    cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : "未获取到本机地址");
                }
                cui_column(ui, .width = CUI_GROW(1), .gap = 4) {
                    cui_muted(ui, "对端设备");
                    cui_label(ui, peer);
                    cui_secondary(ui, control);
                    cui_text(ui, data);
                }
            }
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button(ui, "打开传输管理", .variant = CUI_BUTTON_PRIMARY))
                    hopdesk_ui_show_page(HOPDESK_UI_TRANSFER);
                if (cui_button(ui, "重新连接", .disabled = !s_ops || !s_ops->reconnect))
                    if (s_ops && s_ops->reconnect) s_ops->reconnect();
                if (cui_button(ui, "切换到 Mac 输入源", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_MAC);
                if (cui_button(ui, "切换到 Windows 输入源", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_TO_WINDOWS);
                if (cui_button(ui, "唤醒显示器", .disabled = !s_ops || !s_ops->display_action))
                    if (s_ops && s_ops->display_action) s_ops->display_action(HOPDESK_UI_DISPLAY_WAKE);
            }
        }

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
    char ports[128], role[64], dimensions[96];
    snprintf(ports, sizeof(ports), "控制 %u  ·  数据 %u",
        (unsigned)s_model.config.port, (unsigned)s_model.config.data_port);
    snprintf(role, sizeof(role), "%s",
        s_model.config.role == HOP_ROLE_SERVER ? "服务器 · 提供键鼠" : "客户端 · 接收键鼠");
    snprintf(dimensions, sizeof(dimensions), "%d × %d",
        s_model.config.screen_w, s_model.config.screen_h);

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_heading(ui, "设备", 2);
        cui_text(ui, "查看本机角色、对端地址和连接参数。角色、认证和端口修改后需要重启生效。");
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true) {
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, "本机设备名称");
                cui_heading(ui, s_model.config.device_name[0] ? s_model.config.device_name : "HopDesk", 3);
                cui_secondary(ui, role);
                cui_text(ui, s_model.local_addresses[0] ? s_model.local_addresses : "未获取到本机地址");
                cui_secondary(ui, dimensions);
            }
            cui_card(ui, .width = CUI_GROW(1), .min_width = 260, .pad = 18, .gap = 8) {
                cui_muted(ui, "对端设备");
                cui_heading(ui, s_model.peer_label[0] ? s_model.peer_label : "尚未连接", 3);
                cui_secondary(ui, s_model.connected ? "控制连接已建立" : "等待连接");
                cui_text(ui, ports);
                if (s_model.config.role == HOP_ROLE_CLIENT)
                    cui_text(ui, s_model.config.peer_host[0] ? s_model.config.peer_host : "尚未配置服务器地址");
                else
                    cui_text(ui, "服务器等待客户端连接");
            }
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 10) {
            cui_heading(ui, "连接状态", 3);
            cui_row(ui, .width = CUI_FILL, .gap = 10, .wrap = true) {
                cui_badge_ex(ui, s_model.connected ? "控制通道在线" : "控制通道离线",
                    &(cui_badge_opts){.tone = s_model.connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
                cui_badge_ex(ui, s_model.data_connected ? "数据通道在线" : "数据通道离线",
                    &(cui_badge_opts){.tone = s_model.data_connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
                cui_badge_ex(ui, s_model.sharing ? "键鼠正在共享" : "键鼠由本机控制",
                    &(cui_badge_opts){.tone = s_model.sharing ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
            }
            if (cui_button(ui, "重新连接", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !s_ops || !s_ops->reconnect))
                if (s_ops && s_ops->reconnect) s_ops->reconnect();
        }
        cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 8) {
            cui_heading(ui, "声音能力", 3);
            cui_text(ui, "声音发送和播放是独立能力，不随服务器/客户端角色自动反转。");
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                cui_badge_ex(ui, (s_model.audio_requested & HOP_AUDIO_SEND) ? "发送已配置" : "发送已关闭",
                    &(cui_badge_opts){.tone = (s_model.audio_requested & HOP_AUDIO_SEND) ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
                cui_badge_ex(ui, (s_model.audio_requested & HOP_AUDIO_PLAY) ? "播放已配置" : "播放已关闭",
                    &(cui_badge_opts){.tone = (s_model.audio_requested & HOP_AUDIO_PLAY) ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
                cui_badge_ex(ui, s_model.audio_unavailable ? "音频设备不可用" : "音频状态正常",
                    &(cui_badge_opts){.tone = s_model.audio_unavailable ? CUI_TONE_WARNING : CUI_TONE_SUCCESS});
            }
            if (cui_button(ui, "配置声音能力", .variant = CUI_BUTTON_SECONDARY))
                hopdesk_ui_show_page(HOPDESK_UI_SETTINGS);
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
            (float)((double)status->bytes_done / (double)status->bytes_total) :
            (status->active ? 0.0f : 1.0f),
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
        cui_heading(ui, "传输", 2);
        cui_text(ui, "活动任务、文件分页明细、取消、失败文件重试和历史记录。");
        cui_row(ui, .width = CUI_FILL, .gap = 12, .wrap = true) {
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
                        s_file_offset = 0;
                        s_selected_file = -1;
                        s_force_refresh = 1;
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
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button(ui, "取消选中任务", .variant = CUI_BUTTON_DANGER,
                    .disabled = !chosen || chosen->state >= HOP_XFER_TASK_COMPLETED || chosen->cancel_requested ||
                                !s_ops || !s_ops->cancel_task)) {
                    if (chosen && s_ops && s_ops->cancel_task && s_ops->cancel_task(chosen->id))
                        ui_toast("已请求取消", "任务会在安全的生命周期节点停止。", CUI_TONE_INFO);
                    else
                        ui_toast("无法取消", "任务可能已经结束或正在提交结果。", CUI_TONE_WARNING);
                    s_force_refresh = 1;
                }
                if (chosen) {
                    cui_muted(ui, chosen->detail[0] ? chosen->detail : task_state_text(chosen->state));
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
                        s_force_refresh = 1;
                    }
                    if (cui_button(ui, "下一页", .disabled =
                        s_file_offset + HOPDESK_UI_FILE_PAGE >= (uint32_t)s_model.file_total)) {
                        s_file_offset += HOPDESK_UI_FILE_PAGE;
                        s_selected_file = -1;
                        s_force_refresh = 1;
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
                        s_force_refresh = 1;
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

static void settings_load(void) {
    s_settings.cfg = s_model.config;
    s_settings.port = s_model.config.port;
    s_settings.data_port = s_model.config.data_port;
    s_settings.switch_delay = s_model.config.switch_delay_ms;
    s_settings.cooldown = s_model.config.switch_cooldown_ms;
    s_settings.margin = s_model.config.edge_margin_px;
    s_settings.wake_sec = s_model.config.disp_wake_sec;
    s_settings.chunk_kb = s_model.config.xfer_chunk_kb;
    s_settings.rate_limit = s_model.config.xfer_rate_limit_mib;
    snprintf(s_settings.disp_win, sizeof(s_settings.disp_win), "%u", (unsigned)s_model.config.disp_win_input);
    snprintf(s_settings.disp_mac, sizeof(s_settings.disp_mac), "%u", (unsigned)s_model.config.disp_mac_input);
    s_settings.role = (int)s_model.config.role;
    s_settings.server_edge = s_model.config.server_edge;
    s_settings.client_edge = s_model.config.client_edge;
    s_settings.audio_send = (s_model.config.audio_mode & HOP_AUDIO_SEND) != 0;
    s_settings.audio_play = (s_model.config.audio_mode & HOP_AUDIO_PLAY) != 0;
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
    settings_mark_dirty();
}

static int settings_build_config(hop_config *out) {
    uint16_t win_input, mac_input;
    if (!out) return 0;
    *out = s_settings.cfg;
    if (!out->device_name[0]) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "设备名称不能为空。");
        return 0;
    }
    if (!out->token[0]) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "共享 Token 不能为空。");
        return 0;
    }
    if (s_settings.role == HOP_ROLE_CLIENT && !out->peer_host[0]) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "客户端模式必须填写服务器地址。");
        return 0;
    }
    if (s_settings.port < 1 || s_settings.port > 65535 ||
        s_settings.data_port < 1 || s_settings.data_port > 65535 ||
        (unsigned)s_settings.port == (unsigned)s_settings.data_port) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "控制端口和数据端口必须在 1–65535 之间，且不能相同。");
        return 0;
    }
    if (s_settings.switch_delay < 0 || s_settings.switch_delay > 5000 ||
        s_settings.cooldown < 0 || s_settings.cooldown > 10000 ||
        s_settings.margin < 0 || s_settings.margin > 1000 ||
        s_settings.wake_sec < 1 || s_settings.wake_sec > 3600 ||
        s_settings.chunk_kb < 64 || s_settings.chunk_kb > 4096 ||
        s_settings.rate_limit < 16 || s_settings.rate_limit > 4096) {
        snprintf(s_settings.note, sizeof(s_settings.note), "%s", "边缘参数、唤醒时长或传输参数超出允许范围。");
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
    out->server_edge = s_settings.server_edge;
    out->client_edge = s_settings.client_edge;
    out->switch_delay_ms = (int)s_settings.switch_delay;
    out->switch_cooldown_ms = (int)s_settings.cooldown;
    out->edge_margin_px = (int)s_settings.margin;
    out->disp_win_input = win_input;
    out->disp_mac_input = mac_input;
    out->disp_wake_sec = (int)s_settings.wake_sec;
    out->xfer_chunk_kb = (int)s_settings.chunk_kb;
    out->xfer_rate_limit_mib = (int)s_settings.rate_limit;
    out->audio_mode = (s_settings.audio_send ? HOP_AUDIO_SEND : 0u) |
                      (s_settings.audio_play ? HOP_AUDIO_PLAY : 0u);
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

static void draw_settings(cui_ctx *ui) {
    if (!s_settings.loaded) settings_load();

    cui_column(ui, .width = CUI_FILL, .gap = 16) {
        cui_heading(ui, "设置", 2);
        cui_text(ui, "两端共用同一套设置界面。角色、网络和认证参数保存后需要重启；声音能力独立配置并尝试即时应用。");

        cui_row(ui, .width = CUI_FILL, .gap = 14, .wrap = true) {
            cui_column(ui, .width = CUI_GROW(1), .gap = 14) {
                cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                    cui_heading(ui, "设备与连接", 3);
                    if (cui_input(ui, "setting-device-name", s_settings.cfg.device_name,
                        sizeof(s_settings.cfg.device_name), .label = "设备名称", .placeholder = "例如：Mac / Windows") & CUI_CHANGED)
                        settings_mark_dirty();
                    if (cui_select_ex(ui, "setting-role", k_roles, 2, &s_settings.role,
                        &(cui_select_opts){.label = "本机角色", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-peer-host", s_settings.cfg.peer_host,
                        sizeof(s_settings.cfg.peer_host), .label = "服务器地址",
                        .placeholder = "客户端模式填写服务器 IP 或主机名",
                        .disabled = s_settings.role != HOP_ROLE_CLIENT) & CUI_CHANGED)
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-control-port", &s_settings.port, 1, 65535,
                        &(cui_number_opts){.label = "控制端口", .step = 1, .decimals = 0, .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-data-port", &s_settings.data_port, 1, 65535,
                        &(cui_number_opts){.label = "数据端口", .step = 1, .decimals = 0, .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-token", s_settings.cfg.token,
                        sizeof(s_settings.cfg.token), .label = "共享 Token", .password = true,
                        .placeholder = "两台设备必须一致") & CUI_CHANGED)
                        settings_mark_dirty();
                }
                cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                    cui_heading(ui, "声音通道", 3);
                    cui_text(ui, "发送与播放分别控制本机能力，不依赖服务器/客户端角色。");
                    if (cui_switch(ui, "发送本机系统声音", &s_settings.audio_send,
                        .description = "将本机系统声音发送到对端")) settings_mark_dirty();
                    if (cui_switch(ui, "播放对端系统声音", &s_settings.audio_play,
                        .description = "接收并播放对端系统声音")) settings_mark_dirty();
                    cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                        cui_badge_ex(ui, (s_model.audio_requested & HOP_AUDIO_SEND) ? "发送已配置" : "发送关闭",
                            &(cui_badge_opts){.tone = (s_model.audio_requested & HOP_AUDIO_SEND) ? CUI_TONE_INFO : CUI_TONE_NEUTRAL});
                        cui_badge_ex(ui, (s_model.audio_active & HOP_AUDIO_SEND) ? "发送运行中" : "发送未运行",
                            &(cui_badge_opts){.tone = (s_model.audio_active & HOP_AUDIO_SEND) ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL});
                        cui_badge_ex(ui, (s_model.audio_active & HOP_AUDIO_PLAY) ? "播放运行中" : "播放未运行",
                            &(cui_badge_opts){.tone = (s_model.audio_active & HOP_AUDIO_PLAY) ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL});
                    }
                }
            }

            cui_column(ui, .width = CUI_GROW(1), .gap = 14) {
                cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                    cui_heading(ui, "键鼠与显示器", 3);
                    if (cui_select_ex(ui, "setting-server-edge", k_edges, 4, &s_settings.server_edge,
                        &(cui_select_opts){.label = "服务器移交边缘", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_select_ex(ui, "setting-client-edge", k_edges, 4, &s_settings.client_edge,
                        &(cui_select_opts){.label = "客户端离开边缘", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-switch-delay", &s_settings.switch_delay, 0, 5000,
                        &(cui_number_opts){.label = "边缘触发延迟", .step = 10, .decimals = 0, .suffix = "ms", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-cooldown", &s_settings.cooldown, 0, 10000,
                        &(cui_number_opts){.label = "切换冷却", .step = 50, .decimals = 0, .suffix = "ms", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-margin", &s_settings.margin, 0, 1000,
                        &(cui_number_opts){.label = "边缘内缩", .step = 1, .decimals = 0, .suffix = "px", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-disp-win", s_settings.disp_win, sizeof(s_settings.disp_win),
                        .label = "Windows 显示器输入源", .placeholder = "例如 0x11") & CUI_CHANGED)
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-disp-mac", s_settings.disp_mac, sizeof(s_settings.disp_mac),
                        .label = "Mac 显示器输入源", .placeholder = "例如 0x0f") & CUI_CHANGED)
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-wake-sec", &s_settings.wake_sec, 1, 3600,
                        &(cui_number_opts){.label = "显示器唤醒时长", .step = 1, .decimals = 0, .suffix = "秒", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-mac-input-cmd", s_settings.cfg.mac_input_cmd,
                        sizeof(s_settings.cfg.mac_input_cmd), .label = "Mac 切源命令", .placeholder = "例如 m1ddc display") & CUI_CHANGED)
                        settings_mark_dirty();
                    if (cui_input(ui, "setting-mac-sleep-cmd", s_settings.cfg.mac_sleep_cmd,
                        sizeof(s_settings.cfg.mac_sleep_cmd), .label = "Mac 睡眠命令", .placeholder = "例如 pmset displaysleepnow") & CUI_CHANGED)
                        settings_mark_dirty();
                }
                cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
                    cui_heading(ui, "文件传输", 3);
                    if (cui_number_ex(ui, "setting-chunk-kb", &s_settings.chunk_kb, 64, 4096,
                        &(cui_number_opts){.label = "传输分块大小", .step = 64, .decimals = 0, .suffix = "KB", .width = CUI_FILL}))
                        settings_mark_dirty();
                    if (cui_number_ex(ui, "setting-rate-limit", &s_settings.rate_limit, 16, 4096,
                        &(cui_number_opts){.label = "速率上限", .step = 16, .decimals = 0, .suffix = "MiB/s", .width = CUI_FILL}))
                        settings_mark_dirty();
                }
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                if (cui_button(ui, s_settings.saving ? "保存中…" : "保存设置",
                    .variant = CUI_BUTTON_PRIMARY, .loading = s_settings.saving,
                    .disabled = !s_settings.dirty || s_settings.saving))
                    settings_save();
                if (cui_button(ui, "恢复默认", .disabled = s_settings.saving))
                    settings_reset();
                if (cui_button(ui, "检查更新", .disabled = !s_ops || !s_ops->check_updates))
                    if (s_ops && s_ops->check_updates) s_ops->check_updates();
                if (cui_button(ui, "检查键鼠权限", .disabled = !s_ops || !s_ops->request_permissions))
                    if (s_ops && s_ops->request_permissions) s_ops->request_permissions();
            }
            cui_text(ui, s_settings.note[0] ? s_settings.note : "保存后音频能力会尝试即时应用；角色、地址、端口和认证参数需要重启。");
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN) {
                cui_heading(ui, "运行日志", 3);
                cui_row(ui, .gap = 8) {
                    if (cui_button(ui, "刷新日志", .variant = CUI_BUTTON_SECONDARY) && s_ops && s_ops->load_logs) {
                        if (s_ops->load_logs(s_model.logs, sizeof(s_model.logs)))
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", "日志已更新");
                        else
                            snprintf(s_model.log_note, sizeof(s_model.log_note), "%s", "读取日志失败");
                        cui_request_redraw(ui);
                    }
                    if (cui_button(ui, "复制日志", .variant = CUI_BUTTON_SECONDARY) &&
                        s_ops && s_ops->copy_text && s_model.logs[0]) {
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
                    .read_only = true, .height = 260, .width = CUI_FILL);
            } else {
                cui_secondary(ui, "点击“刷新日志”读取最近的运行记录。");
            }
        }
    }
}

void hopdesk_ui_set_ops(const hopdesk_ui_ops *ops) {
    s_ops = ops;
}

void hopdesk_ui_init(cui_ctx *ui) {
    s_ctx = ui;
    s_force_refresh = 1;
    s_selected_file = -1;
    if (!s_settings.dirty && !s_settings.saving) s_settings.loaded = 0;
    if (s_ops && s_ops->cjk_font_path) {
        for (i = 0; i < 4; ++i) {
            const char *path = s_ops->cjk_font_path(i);
            if (path && path[0]) {
                cui_font font = cui_font_load_file(ui, path);
                if (font >= 0) {
                    cui_font_add_fallback(ui, CUI_FONT_BODY, font);
                    cui_font_add_fallback(ui, CUI_FONT_MEDIUM, font);
                    cui_font_add_fallback(ui, CUI_FONT_DISPLAY, font);
                    cui_font_add_fallback(ui, CUI_FONT_MONO, font);
                }
            }
        }
    }
}

void hopdesk_ui_frame(cui_ctx *ui) {
    int found = 0;
    if (!ui) return;
    if (s_ops && s_ops->snapshot)
        s_ops->snapshot(&s_model, s_selected_task, s_file_offset);
    s_force_refresh = 0;
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
            if (cui_nav_item_ex(ui, "概览", &(cui_nav_opts){.icon = "layout-dashboard", .selected = s_page == HOPDESK_UI_OVERVIEW}))
                hopdesk_ui_show_page(HOPDESK_UI_OVERVIEW);
            if (cui_nav_item_ex(ui, "设备", &(cui_nav_opts){.icon = "monitor", .selected = s_page == HOPDESK_UI_DEVICE}))
                hopdesk_ui_show_page(HOPDESK_UI_DEVICE);
            if (cui_nav_item_ex(ui, "传输", &(cui_nav_opts){.icon = "arrow-left-right", .selected = s_page == HOPDESK_UI_TRANSFER,
                .badge = s_model.task_total > 0 ? "●" : NULL}))
                hopdesk_ui_show_page(HOPDESK_UI_TRANSFER);
            cui_nav_section(ui, "偏好");
            if (cui_nav_item_ex(ui, "设置", &(cui_nav_opts){.icon = "settings", .selected = s_page == HOPDESK_UI_SETTINGS}))
                hopdesk_ui_show_page(HOPDESK_UI_SETTINGS);
            cui_spacer(ui);
            cui_separator(ui);
            cui_muted(ui, "轻量 · 跨平台 · 本地优先");
        }

        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 22, .gap = 14) {
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                cui_heading(ui, k_pages[s_page], 1);
                cui_spacer(ui);
                cui_badge_ex(ui, s_model.connected ? "已连接" : "未连接",
                    &(cui_badge_opts){.tone = s_model.connected ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL, .dot = true});
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
    cui_request_wakeup(ui, 0.5f);
}

void hopdesk_ui_shutdown(void) {
    s_ctx = NULL;
}

void hopdesk_ui_refresh(void) {
    s_force_refresh = 1;
    if (s_ctx) cui_request_redraw(s_ctx);
}

void hopdesk_ui_show_page(int page) {
    if (page < 0 || page >= HOPDESK_UI_PAGE_COUNT) page = HOPDESK_UI_OVERVIEW;
    s_page = page;
    s_force_refresh = 1;
    if (s_ctx) cui_request_redraw(s_ctx);
}

void hopdesk_ui_save_complete(int ok, const char *message) {
    s_settings.saving = 0;
    if (ok) {
        s_settings.dirty = 0;
        s_model.config = s_settings.cfg;
        snprintf(s_settings.note, sizeof(s_settings.note), "%s",
            message && message[0] ? message : "设置已保存。网络、认证和角色参数需要重启。");
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
