#include "display_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const hop_display_input_preset k_input_presets[] = {
    { 0x0Fu, "DisplayPort 1 (0x0F)" },
    { 0x10u, "DisplayPort 2 (0x10)" },
    { 0x11u, "HDMI 1 (0x11)" },
    { 0x12u, "HDMI 2 (0x12)" },
    { 0x1Bu, "USB-C / DP Alt (0x1B)" },
    { 0x1Cu, "USB-C 2 (0x1C)" },
    { 0x03u, "DVI 1 (0x03)" },
    { 0x04u, "DVI 2 (0x04)" },
    { 0x01u, "VGA 1 (0x01)" }
};

size_t hop_display_input_preset_count(void) {
    return sizeof(k_input_presets) / sizeof(k_input_presets[0]);
}

const hop_display_input_preset *hop_display_input_preset_at(size_t index) {
    return index < hop_display_input_preset_count() ? &k_input_presets[index] : NULL;
}

int hop_display_input_find_preset(uint16_t code) {
    uint16_t low = (uint16_t)(code & 0xFFu);
    for (size_t i = 0; i < hop_display_input_preset_count(); ++i) {
        if (k_input_presets[i].code == code || (code <= 0xFFu && k_input_presets[i].code == low))
            return (int)i;
    }
    return -1;
}

void hop_display_input_format_label(uint16_t code, char *out, size_t cap) {
    if (!out || !cap) return;
    int idx = hop_display_input_find_preset(code);
    if (idx >= 0) {
        snprintf(out, cap, "%s", k_input_presets[idx].label);
        return;
    }
    snprintf(out, cap, "自定义 (0x%02X)", (unsigned)code);
}

static const char *position_hint(const hop_display_info *info) {
    if (!info) return "";
    if (info->is_primary) return "主屏";
    if (!info->attached) return "未启用";
    if (info->pos_x > 0) return "右侧扩展屏";
    if (info->pos_x < 0) return "左侧扩展屏";
    if (info->pos_y < 0) return "上方扩展屏";
    if (info->pos_y > 0) return "下方扩展屏";
    return "扩展屏";
}

void hop_display_format_monitor_label(const hop_display_info *info, char *out, size_t cap) {
    if (!out || !cap) return;
    if (!info) { out[0] = '\0'; return; }

    const char *name = info->friendly_name[0] ? info->friendly_name :
                       (info->is_builtin ? "内置显示器" : "外接显示器");
    const char *role = info->is_builtin ? "内置屏" : position_hint(info);
    int idx = info->index > 0 ? info->index : 1;
    char input_part[48] = {0};
    if (info->current_input >= 0 && info->current_input <= 65535) {
        char input_name[36];
        hop_display_input_format_label((uint16_t)info->current_input, input_name, sizeof(input_name));
        snprintf(input_part, sizeof(input_part), " · 当前 %s", input_name);
    }

    if (info->attached && info->width > 0 && info->height > 0) {
        snprintf(out, cap, "显示器 %d · %s (%d×%d · %s)%s",
                 idx, name, info->width, info->height, role, input_part);
    } else {
        snprintf(out, cap, "显示器 %d · %s (%s)%s",
                 idx, name, role, input_part);
    }
}

void hop_display_mac_build_input_cmd(int display_index, char *out, size_t cap) {
    if (!out || !cap) return;
    if (display_index < 1 || display_index > 16) display_index = 1;
    snprintf(out, cap, "m1ddc display %d set input", display_index);
}

int hop_display_mac_parse_input_cmd_index(const char *cmd) {
    static const char prefix[] = "m1ddc display ";
    static const char suffix[] = " set input";
    if (!cmd) return 0;
    while (*cmd == ' ' || *cmd == '\t') ++cmd;
    if (strncmp(cmd, prefix, sizeof(prefix) - 1) != 0) return 0;
    const char *p = cmd + sizeof(prefix) - 1;
    if (*p < '1' || *p > '9') return 0;
    long idx = 0;
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        if (idx > 16) return 0;
        ++p;
    }
    if (strncmp(p, suffix, sizeof(suffix) - 1) != 0) return 0;
    p += sizeof(suffix) - 1;
    while (*p == ' ' || *p == '\t') ++p;
    return *p == '\0' ? (int)idx : 0;
}
