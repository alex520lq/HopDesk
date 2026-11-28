#ifndef HOP_DISPLAY_CATALOG_H
#define HOP_DISPLAY_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#define HOP_DISPLAY_MAX_DEVICES 8

typedef struct {
    uint16_t    code;
    const char *label;
} hop_display_input_preset;

typedef struct {
    int32_t index;            /* 1-based human display index */
    int32_t is_primary;       /* 1 if primary display */
    int32_t attached;         /* 1 if attached/active on desktop */
    int32_t is_builtin;       /* 1 if built-in panel (e.g. laptop screen) */
    int32_t width;
    int32_t height;
    int32_t pos_x;
    int32_t pos_y;
    int32_t current_input;    /* live VCP 0x60 code, or -1 if unavailable */
    char    device_id[32];    /* Windows: \\.\DISPLAY2; macOS: display index */
    char    friendly_name[64];/* EDID / OS monitor model name, e.g. BenQ PD2700U */
} hop_display_info;

typedef struct {
    int32_t          count;
    int32_t          probing;
    int32_t          ddc_tool_ready; /* Windows: dxva2 ok; macOS: m1ddc found */
    char             probe_note[160];
    hop_display_info items[HOP_DISPLAY_MAX_DEVICES];
} hop_display_inventory;

/* Standard VESA MCCS VCP 0x60 input source presets. */
size_t hop_display_input_preset_count(void);
const hop_display_input_preset *hop_display_input_preset_at(size_t index);
/* Returns preset index (0..count-1) or -1 if custom. */
int hop_display_input_find_preset(uint16_t code);
/* Formats a user-facing input label, e.g. "DisplayPort 1 (0x0F)" or "自定义 (0xD0)". */
void hop_display_input_format_label(uint16_t code, char *out, size_t cap);

/* Formats a human-readable monitor label without requiring users to parse \\.\DISPLAY2. */
void hop_display_format_monitor_label(const hop_display_info *info, char *out, size_t cap);

/* Maps between a 1-based macOS m1ddc display index and the standard command string. */
void hop_display_mac_build_input_cmd(int display_index, char *out, size_t cap);
/* Returns 1..16 if cmd matches "m1ddc display <N> set input", else 0 (custom command). */
int  hop_display_mac_parse_input_cmd_index(const char *cmd);

#endif
