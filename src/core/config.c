/* =====================================================================
 *  HopDesk - Unified Configuration Schema, Parsing, Validation & Atomic I/O
 *  Platform-independent single source of truth for hopdesk.conf, GUI, IPC & CLI.
 * ===================================================================== */
#include "config_schema.h"
#include "audio_protocol.h"
#include "xfer_budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <windows.h>
#include <io.h>
#endif

#define CFG_FIELD(member) offsetof(hop_config, member), sizeof(((const hop_config *)0)->member)

static const hop_config_spec k_specs[] = {
    /* ---- Connection & Identity ---- */
    { "role", NULL, "connection", HOP_CFG_ENUM, HOP_CFG_APPLY_RESTART, 0,
      HOP_ROLE_SERVER, HOP_ROLE_CLIENT, "server|client",
#if defined(_WIN32)
      "server",
#else
      "client",
#endif
      "Transport role: server listens on ports, client dials peer",
      CFG_FIELD(role) },
    { "name", NULL, "connection", HOP_CFG_STRING, HOP_CFG_APPLY_RECONNECT, 0,
      0, HOP_NAME_MAX - 1, NULL, "",
      "Device display name shown to peer",
      CFG_FIELD(device_name) },
    { "host", NULL, "connection", HOP_CFG_STRING, HOP_CFG_APPLY_RECONNECT, 0,
      0, 63, NULL, "",
      "Peer IP address or hostname (required when role=client)",
      CFG_FIELD(peer_host) },
    { "port", NULL, "connection", HOP_CFG_U16, HOP_CFG_APPLY_RESTART, 0,
      1, 65535, NULL, "24817",
      "Control channel TCP port",
      CFG_FIELD(port) },
    { "data_port", NULL, "connection", HOP_CFG_U16, HOP_CFG_APPLY_RESTART, 0,
      1, 65535, NULL, "24818",
      "File/image data channel TCP port",
      CFG_FIELD(data_port) },
    { "audio_port", NULL, "connection", HOP_CFG_U16, HOP_CFG_APPLY_RESTART, 0,
      1, 65535, NULL, "24819",
      "System audio streaming TCP port",
      CFG_FIELD(audio_port) },
    { "token", NULL, "connection", HOP_CFG_STRING, HOP_CFG_APPLY_RECONNECT, 1,
      0, HOP_TOKEN_MAX - 1, NULL, "",
      "Shared authentication token (must be non-empty and match peer)",
      CFG_FIELD(token) },

    /* ---- Capabilities & Appearance ---- */
    { "input_mode", NULL, "capabilities", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, (int)HOP_INPUT_MODES, NULL,
#if defined(_WIN32)
      "3",
#else
      "0",
#endif
      "Local input provider bitmask: 1=mouse, 2=keyboard, 3=both, 0=none",
      CFG_FIELD(input_mode) },
    { "audio_mode", NULL, "capabilities", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, (int)HOP_AUDIO_MODES, NULL,
#if defined(_WIN32)
      "2",
#else
      "1",
#endif
      "Local audio capability bitmask: 1=send, 2=play, 3=both, 0=off",
      CFG_FIELD(audio_mode) },
    { "ui_theme", NULL, "capabilities", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_THEME_SYSTEM, HOP_THEME_DARK, "system|light|dark", "system",
      "UI color theme",
      CFG_FIELD(ui_theme) },

    /* ---- Input, Edge & Keymap ---- */
    { "server_edge", NULL, "input", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_EDGE_LEFT, HOP_EDGE_BOTTOM, "left|right|top|bottom", "left",
      "Screen edge to cross when handing control to peer (also accepts 0-3)",
      CFG_FIELD(server_edge) },
    { "client_edge", NULL, "input", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_EDGE_LEFT, HOP_EDGE_BOTTOM, "left|right|top|bottom", "right",
      "Screen edge to cross when returning control from peer (also accepts 0-3)",
      CFG_FIELD(client_edge) },
    { "switch_delay_ms", "switch_push_ms", "input", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, 5000, NULL, "250",
      "Edge dwell delay before handing control to peer (ms)",
      CFG_FIELD(switch_delay_ms) },
    { "switch_cooldown_ms", NULL, "input", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, 10000, NULL, "300",
      "Cooldown after edge switch to prevent ping-ponging (ms)",
      CFG_FIELD(switch_cooldown_ms) },
    { "edge_margin_px", NULL, "input", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, 1000, NULL, "40",
      "Inner deadzone inset and return overshoot distance (px)",
      CFG_FIELD(edge_margin_px) },
    { "mouse_scale_pct", NULL, "input", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      25, 400, NULL, "100",
      "Injected mouse pointer speed scale percentage (100 = 1.0x)",
      CFG_FIELD(mouse_scale_pct) },
    { "wheel_scale_pct", NULL, "input", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      25, 400, NULL, "100",
      "Injected scroll wheel speed scale percentage (100 = 1.0x)",
      CFG_FIELD(wheel_scale_pct) },
    { "wheel_invert", NULL, "input", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "0",
      "Invert injected scroll wheel direction (natural scrolling toggle)",
      CFG_FIELD(wheel_invert) },
    { "mod_ctrl", NULL, "input", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_MOD_TARGET_COMMAND, HOP_MOD_TARGET_SHIFT, "command|control|option|shift", "command",
      "Target modifier mapped from physical Ctrl key",
      CFG_FIELD(mod_ctrl) },
    { "mod_alt", NULL, "input", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_MOD_TARGET_COMMAND, HOP_MOD_TARGET_SHIFT, "command|control|option|shift", "option",
      "Target modifier mapped from physical Alt key",
      CFG_FIELD(mod_alt) },
    { "mod_win", NULL, "input", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_MOD_TARGET_COMMAND, HOP_MOD_TARGET_SHIFT, "command|control|option|shift", "control",
      "Target modifier mapped from physical Win/Super key",
      CFG_FIELD(mod_win) },

    /* ---- Display Switching (DDC/CI) ---- */
    { "disp_shared", NULL, "display", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "false",
      "Windows owns shared-monitor topology switching (explicit opt-in)",
      CFG_FIELD(disp_shared) },
    { "disp_device", NULL, "display", HOP_CFG_STRING, HOP_CFG_APPLY_HOT, 0,
      0, 31, NULL, "",
      "Shared Windows secondary display device name; never inferred from resolution",
      CFG_FIELD(disp_device) },
    { "disp_win_input", NULL, "display", HOP_CFG_HEX16, HOP_CFG_APPLY_HOT, 0,
      0, 65535, NULL, "0x000F",
      "VCP 0x60 DDC input source code for Windows (decimal or 0x hex)",
      CFG_FIELD(disp_win_input) },
    { "disp_mac_input", NULL, "display", HOP_CFG_HEX16, HOP_CFG_APPLY_HOT, 0,
      0, 65535, NULL, "0x0011",
      "VCP 0x60 DDC input source code for macOS (decimal or 0x hex)",
      CFG_FIELD(disp_mac_input) },
    { "disp_wake_sec", NULL, "display", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 3600, NULL, "60",
      "Display wake assertion hold duration (seconds)",
      CFG_FIELD(disp_wake_sec) },
    { "disp_wake_delay_ms", NULL, "display", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, 10000, NULL, "1200",
      "Delay between waking macOS and switching DDC input source (ms)",
      CFG_FIELD(disp_wake_delay_ms) },
    { "disp_monitor_index", NULL, "display", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      0, 15, NULL, "0",
      "Windows DDC target monitor index (0 = primary monitor)",
      CFG_FIELD(disp_monitor_index) },
    { "mac_input_cmd", NULL, "display", HOP_CFG_STRING, HOP_CFG_APPLY_HOT, 0,
      0, 95, NULL, "m1ddc display 1 set input",
      "macOS command prefix for DDC input switching (VCP value appended)",
      CFG_FIELD(mac_input_cmd) },
    { "mac_sleep_cmd", NULL, "display", HOP_CFG_STRING, HOP_CFG_APPLY_HOT, 0,
      0, 63, NULL, "pmset displaysleepnow",
      "macOS command to sleep the display",
      CFG_FIELD(mac_sleep_cmd) },

    /* ---- File/Image Transfer & Resource Budgets ---- */
    { "xfer_chunk_kb", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      64, HOP_CHUNK_KB_MAX, NULL, "256",
      "Data channel chunk size (KB); 256 KB isolates mouse latency on 1 GbE",
      CFG_FIELD(xfer_chunk_kb) },
    { "xfer_rate_limit_mib", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      16, 4096, NULL, "128",
      "Data channel maximum pacing rate (MiB/s)",
      CFG_FIELD(xfer_rate_limit_mib) },
    { "xfer_file_retry_max", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 10, NULL, "3",
      "Maximum per-file transmission attempts including first try",
      CFG_FIELD(xfer_file_retry_max) },
    { "xfer_job_retry_max", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 20, NULL, "5",
      "Maximum connection-level job retry attempts",
      CFG_FIELD(xfer_job_retry_max) },
    { "xfer_ack_timeout_ms", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1000, 120000, NULL, "10000",
      "Per-file ACK wait timeout (ms)",
      CFG_FIELD(xfer_ack_timeout_ms) },
    { "xfer_reconnect_budget_ms", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      5000, 600000, NULL, "30000",
      "Sender reconnect grace budget after data disconnect (ms)",
      CFG_FIELD(xfer_reconnect_budget_ms) },
    { "xfer_rx_disconnect_budget_ms", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      5000, 600000, NULL, "60000",
      "Receiver disconnect grace budget before aborting batch (ms)",
      CFG_FIELD(xfer_rx_disconnect_budget_ms) },
    { "xfer_max_jobs", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 128, NULL, "16",
      "Global concurrent/queued transfer job limit",
      CFG_FIELD(xfer_max_jobs) },
    { "xfer_max_entries", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      64, 262144, NULL, "32768",
      "Global pending file/directory entry limit",
      CFG_FIELD(xfer_max_entries) },
    { "xfer_max_pending_gib", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 512, NULL, "64",
      "Global pending transfer byte budget (GiB)",
      CFG_FIELD(xfer_max_pending_gib) },
    { "xfer_max_image_mb", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      16, 512, NULL, "256",
      "Maximum clipboard image payload memory budget (MiB)",
      CFG_FIELD(xfer_max_image_mb) },
    { "xfer_max_image_temp_mb", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      32, 1024, NULL, "512",
      "Maximum temporary image conversion memory budget (MiB)",
      CFG_FIELD(xfer_max_image_temp_mb) },
    { "xfer_rx_queue_slots", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_RESTART, 0,
      2, 32, NULL, "8",
      "Bounded disk writer queue depth (slots)",
      CFG_FIELD(xfer_rx_queue_slots) },
    { "xfer_rx_queue_mb", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_RESTART, 0,
      2, 64, NULL, "8",
      "Bounded disk writer queue byte budget (MiB)",
      CFG_FIELD(xfer_rx_queue_mb) },
    { "xfer_scan_max_entries", NULL, "transfer", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      64, 65536, NULL, "8192",
      "Maximum entries scanned in a single file clipboard batch",
      CFG_FIELD(xfer_scan_max_entries) },
    { "xfer_rx_dir", NULL, "transfer", HOP_CFG_STRING, HOP_CFG_APPLY_HOT, 0,
      0, HOP_RX_DIR_MAX - 1, NULL, "",
      "Custom staging root directory for received files (empty = OS temp/HopDesk)",
      CFG_FIELD(xfer_rx_dir) },

    /* ---- Network, Heartbeat & Audio Link ---- */
    { "net_handshake_timeout_ms", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      1000, 60000, NULL, "5000",
      "Control/data handshake completion deadline (ms)",
      CFG_FIELD(net_handshake_timeout_ms) },
    { "net_heartbeat_interval_ms", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      500, 30000, NULL, "2000",
      "Control channel PING keepalive interval (ms)",
      CFG_FIELD(net_heartbeat_interval_ms) },
    { "net_heartbeat_timeout_ms", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1500, 120000, NULL, "6000",
      "Control channel silence timeout before disconnect (ms)",
      CFG_FIELD(net_heartbeat_timeout_ms) },
    { "net_connect_timeout_ms", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      500, 30000, NULL, "3000",
      "TCP connect attempt timeout (ms)",
      CFG_FIELD(net_connect_timeout_ms) },
    { "net_send_timeout_ms", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      500, 30000, NULL, "3000",
      "Control socket SO_SNDTIMEO upper bound (ms)",
      CFG_FIELD(net_send_timeout_ms) },
    { "net_reconnect_max_sec", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 120, NULL, "10",
      "Maximum exponential backoff interval for client reconnect (seconds)",
      CFG_FIELD(net_reconnect_max_sec) },
    { "net_ctrl_queue_soft", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      64, 16384, NULL, "2048",
      "Control queue soft limit where droppable motion/wheel frames are shed",
      CFG_FIELD(net_ctrl_queue_soft) },
    { "net_ctrl_queue_hard", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      128, 32768, NULL, "4096",
      "Control queue hard frame limit triggering disconnect reset",
      CFG_FIELD(net_ctrl_queue_hard) },
    { "net_ctrl_queue_mb", NULL, "network", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 64, NULL, "16",
      "Control queue payload byte limit (MiB)",
      CFG_FIELD(net_ctrl_queue_mb) },
    { "audio_keepalive_ms", NULL, "audio", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      200, 10000, NULL, "1000",
      "Audio stream idle keepalive packet interval (ms)",
      CFG_FIELD(audio_keepalive_ms) },
    { "audio_peer_timeout_ms", NULL, "audio", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      500, 30000, NULL, "3000",
      "Audio stream peer silence timeout (ms)",
      CFG_FIELD(audio_peer_timeout_ms) },
    { "audio_send_limit_ms", NULL, "audio", HOP_CFG_INT, HOP_CFG_APPLY_RECONNECT, 0,
      20, 2000, NULL, "120",
      "Maximum wait for a single audio packet send before dropping link (ms)",
      CFG_FIELD(audio_send_limit_ms) },

    /* ---- Clipboard Policy ---- */
    { "clip_text", NULL, "clipboard", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "1",
      "Enable bidirectional text clipboard sync",
      CFG_FIELD(clip_text) },
    { "clip_image", NULL, "clipboard", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "1",
      "Enable bidirectional image clipboard sync",
      CFG_FIELD(clip_image) },
    { "clip_file", NULL, "clipboard", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "1",
      "Enable bidirectional file clipboard offer/paste sync",
      CFG_FIELD(clip_file) },
    { "clip_text_max_kb", NULL, "clipboard", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      4, 64, NULL, "64",
      "Maximum clipboard text payload size (KB)",
      CFG_FIELD(clip_text_max_kb) },
    { "clip_poll_ms", NULL, "clipboard", HOP_CFG_INT, HOP_CFG_APPLY_RESTART, 0,
      50, 2000, NULL, "250",
      "macOS NSPasteboard changeCount polling interval (ms)",
      CFG_FIELD(clip_poll_ms) },

    /* ---- Logging & Maintenance ---- */
    { "log_level", NULL, "logging", HOP_CFG_ENUM, HOP_CFG_APPLY_HOT, 0,
      HOP_LOG_INFO, HOP_LOG_OFF, "info|warn|error|off", "info",
      "Minimum log level recorded to hopdesk.log",
      CFG_FIELD(log_level) },
    { "log_rotate_mb", NULL, "logging", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      1, 100, NULL, "5",
      "Log file rotation size threshold (MB)",
      CFG_FIELD(log_rotate_mb) },
    { "io_deadline_ms", NULL, "maintenance", HOP_CFG_INT, HOP_CFG_APPLY_HOT, 0,
      2000, 120000, NULL, "15000",
      "Watchdog deadline for blocking I/O operations (ms)",
      CFG_FIELD(io_deadline_ms) },
    { "update_check_enabled", NULL, "maintenance", HOP_CFG_BOOL, HOP_CFG_APPLY_HOT, 0,
      0, 1, NULL, "1",
      "Allow update manifest checks",
      CFG_FIELD(update_check_enabled) }
};

static const size_t k_spec_count = sizeof(k_specs) / sizeof(k_specs[0]);

size_t hop_config_spec_count(void) { return k_spec_count; }

const hop_config_spec *hop_config_spec_at(size_t index) {
    return index < k_spec_count ? &k_specs[index] : NULL;
}

const hop_config_spec *hop_config_spec_find(const char *key) {
    if (!key || !key[0]) return NULL;
    for (size_t i = 0; i < k_spec_count; ++i) {
        if (strcmp(k_specs[i].key, key) == 0 ||
            (k_specs[i].alias && strcmp(k_specs[i].alias, key) == 0))
            return &k_specs[i];
    }
    return NULL;
}

const char *hop_cfg_type_name(hop_cfg_type type) {
    switch (type) {
        case HOP_CFG_INT:    return "int";
        case HOP_CFG_U16:    return "u16";
        case HOP_CFG_HEX16:  return "hex16";
        case HOP_CFG_BOOL:   return "bool";
        case HOP_CFG_ENUM:   return "enum";
        case HOP_CFG_STRING: return "string";
        default:             return "unknown";
    }
}

const char *hop_cfg_apply_name(hop_cfg_apply apply) {
    switch (apply) {
        case HOP_CFG_APPLY_HOT:       return "hot";
        case HOP_CFG_APPLY_RECONNECT: return "reconnect";
        case HOP_CFG_APPLY_RESTART:   return "restart";
        default:                      return "restart";
    }
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

void hop_config_defaults(hop_config *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->role = HOP_ROLE_SERVER;
#if !defined(_WIN32)
    c->role = HOP_ROLE_CLIENT; /* preserve existing macOS installations */
    c->input_mode = 0;
    c->audio_mode = HOP_AUDIO_SEND;
#else
    c->input_mode = HOP_INPUT_MODES;
    c->audio_mode = HOP_AUDIO_PLAY;
#endif
    c->port            = HOP_DEFAULT_PORT;
    c->data_port       = HOP_DATA_PORT;
    c->audio_port      = HOP_AUDIO_PORT;
    c->ui_theme        = HOP_THEME_SYSTEM;
    c->server_edge     = HOP_EDGE_LEFT;
    c->client_edge     = HOP_EDGE_RIGHT;
    c->screen_w        = 0;
    c->screen_h        = 0;

    c->switch_delay_ms    = 250;
    c->switch_cooldown_ms = 300;
    c->edge_margin_px     = 40;
    c->mouse_scale_pct    = 100;
    c->wheel_scale_pct    = 100;
    c->wheel_invert       = 0;
    c->mod_ctrl           = HOP_MOD_TARGET_COMMAND;
    c->mod_alt            = HOP_MOD_TARGET_OPTION;
    c->mod_win            = HOP_MOD_TARGET_CONTROL;

    c->token[0]        = '\0';
    c->peer_host[0]    = '\0';
    c->device_name[0]  = '\0';

    c->disp_win_input     = 0x0F;
    c->disp_mac_input     = 0x11;
    c->disp_wake_sec      = 60;
    c->disp_wake_delay_ms = 1200;
    c->disp_monitor_index = 0;
    snprintf(c->mac_input_cmd, sizeof(c->mac_input_cmd),
             "%s", "m1ddc display 1 set input");
    snprintf(c->mac_sleep_cmd, sizeof(c->mac_sleep_cmd),
             "%s", "pmset displaysleepnow");

    c->xfer_chunk_kb                = HOP_CHUNK_KB_DEFAULT;
    c->xfer_rate_limit_mib          = 128;
    c->xfer_file_retry_max          = HOP_XFER_FILE_RETRY_MAX;
    c->xfer_job_retry_max           = HOP_XFER_JOB_RETRY_MAX;
    c->xfer_ack_timeout_ms          = HOP_XFER_ACK_TIMEOUT_MS;
    c->xfer_reconnect_budget_ms     = HOP_XFER_RECONNECT_BUDGET_MS;
    c->xfer_rx_disconnect_budget_ms = HOP_XFER_RX_DISCONNECT_BUDGET_MS;
    c->xfer_max_jobs                = (int)HOP_XFER_DEFAULT_MAX_JOBS;
    c->xfer_max_entries             = (int)HOP_XFER_DEFAULT_MAX_ENTRIES;
    c->xfer_max_pending_gib         = 64;
    c->xfer_max_image_mb            = 256;
    c->xfer_max_image_temp_mb       = 512;
    c->xfer_rx_queue_slots          = (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS;
    c->xfer_rx_queue_mb             = 8;
    c->xfer_scan_max_entries        = 8192;
    c->xfer_rx_dir[0]               = '\0';

    c->net_handshake_timeout_ms  = HOP_HANDSHAKE_TIMEOUT_MS;
    c->net_heartbeat_interval_ms = HOP_HEARTBEAT_INTERVAL_MS;
    c->net_heartbeat_timeout_ms  = HOP_HEARTBEAT_TIMEOUT_MS;
    c->net_connect_timeout_ms    = HOP_CONNECT_TIMEOUT_MS;
    c->net_send_timeout_ms       = HOP_CTRL_SEND_TIMEOUT_MS;
    c->net_reconnect_max_sec     = 10;
    c->net_ctrl_queue_soft       = HOP_CTRL_QUEUE_SOFT;
    c->net_ctrl_queue_hard       = HOP_CTRL_QUEUE_HARD;
    c->net_ctrl_queue_mb         = 16;

    c->audio_keepalive_ms    = 1000;
    c->audio_peer_timeout_ms = 3000;
    c->audio_send_limit_ms   = 120;

    c->clip_text        = 1;
    c->clip_image       = 1;
    c->clip_file        = 1;
    c->clip_text_max_kb = 64;
    c->clip_poll_ms     = 250;

    c->log_level            = HOP_LOG_INFO;
    c->log_rotate_mb        = 5;
    c->io_deadline_ms       = (int)HOP_IO_DEADLINE_MS;
    c->update_check_enabled = 1;
}

static int parse_long_strict(const char *s, int base, long *out) {
    char *end = NULL;
    if (!s || !s[0]) return 0;
    errno = 0;
    long v = strtol(s, &end, base);
    if (errno || end == s || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int parse_enum_index(const char *values, const char *v, int *out) {
    if (!values || !v || !v[0]) return 0;
    const char *p = values;
    int idx = 0;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (strlen(v) == len && strncmp(p, v, len) == 0) {
            *out = idx;
            return 1;
        }
        if (!bar) break;
        p = bar + 1;
        ++idx;
    }
    return 0;
}

static int format_enum_name(const char *values, int idx, char *out, size_t cap) {
    if (!values || idx < 0 || !out || !cap) return 0;
    const char *p = values;
    int cur = 0;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (cur == idx) {
            if (len + 1 > cap) return 0;
            memcpy(out, p, len);
            out[len] = '\0';
            return 1;
        }
        if (!bar) break;
        p = bar + 1;
        ++cur;
    }
    return 0;
}

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int read_int_field(const hop_config *c, const hop_config_spec *spec) {
    const unsigned char *base = (const unsigned char *)c;
    if (spec->offset == offsetof(hop_config, role)) return (int)c->role;
    if (spec->offset == offsetof(hop_config, input_mode)) return (int)c->input_mode;
    if (spec->offset == offsetof(hop_config, audio_mode)) return (int)c->audio_mode;
    if (spec->type == HOP_CFG_U16 || spec->type == HOP_CFG_HEX16) {
        uint16_t v = 0;
        memcpy(&v, base + spec->offset, sizeof(v));
        return (int)v;
    }
    int v = 0;
    memcpy(&v, base + spec->offset, sizeof(v));
    return v;
}

static void write_int_field(hop_config *c, const hop_config_spec *spec, int val) {
    unsigned char *base = (unsigned char *)c;
    if (spec->offset == offsetof(hop_config, role)) {
        c->role = (hop_role)val;
        return;
    }
    if (spec->offset == offsetof(hop_config, input_mode)) {
        c->input_mode = (unsigned)val;
        return;
    }
    if (spec->offset == offsetof(hop_config, audio_mode)) {
        c->audio_mode = (unsigned)val;
        return;
    }
    if (spec->type == HOP_CFG_U16 || spec->type == HOP_CFG_HEX16) {
        uint16_t v = (uint16_t)val;
        memcpy(base + spec->offset, &v, sizeof(v));
        return;
    }
    memcpy(base + spec->offset, &val, sizeof(val));
}

static int valid_text_buf(const char *p, size_t size) {
    const char *end = (const char *)memchr(p, 0, size);
    if (!end) return 0;
    for (; p < end; ++p)
        if (*p == '\r' || *p == '\n') return 0;
    return 1;
}

int hop_config_set_kv(hop_config *c, const char *key, const char *value,
                      char *err, size_t err_cap) {
    if (!c || !key || !value) {
        set_err(err, err_cap, "null argument");
        return -1;
    }
    const hop_config_spec *spec = hop_config_spec_find(key);
    if (!spec) {
        if (err && err_cap) snprintf(err, err_cap, "unknown config key '%s'", key);
        return 0;
    }

    switch (spec->type) {
    case HOP_CFG_STRING: {
        size_t len = strlen(value);
        if (len >= spec->size) {
            if (err && err_cap)
                snprintf(err, err_cap, "%s exceeds max length %u", spec->key, (unsigned)(spec->size - 1));
            return -1;
        }
        for (size_t i = 0; i < len; ++i) {
            if (value[i] == '\r' || value[i] == '\n') {
                if (err && err_cap)
                    snprintf(err, err_cap, "%s must not contain line breaks", spec->key);
                return -1;
            }
        }
        char *dst = (char *)c + spec->offset;
        memcpy(dst, value, len + 1);
        return 1;
    }
    case HOP_CFG_BOOL: {
        int b = -1;
        if (eq_ci(value, "1") || eq_ci(value, "true") || eq_ci(value, "yes") || eq_ci(value, "on"))
            b = 1;
        else if (eq_ci(value, "0") || eq_ci(value, "false") || eq_ci(value, "no") || eq_ci(value, "off"))
            b = 0;
        if (b < 0) {
            if (err && err_cap)
                snprintf(err, err_cap, "%s must be a boolean (0/1/true/false)", spec->key);
            return -1;
        }
        write_int_field(c, spec, b);
        return 1;
    }
    case HOP_CFG_ENUM: {
        int idx = -1;
        if (parse_enum_index(spec->enum_values, value, &idx)) {
            write_int_field(c, spec, idx);
            return 1;
        }
        /* Backward compatibility: server_edge and client_edge accept numeric 0..3. */
        if (spec->offset == offsetof(hop_config, server_edge) ||
            spec->offset == offsetof(hop_config, client_edge)) {
            long num = 0;
            if (parse_long_strict(value, 10, &num) && num >= spec->min_val && num <= spec->max_val) {
                write_int_field(c, spec, (int)num);
                return 1;
            }
        }
        if (err && err_cap)
            snprintf(err, err_cap, "%s must be one of: %s", spec->key, spec->enum_values);
        return -1;
    }
    case HOP_CFG_INT:
    case HOP_CFG_U16:
    case HOP_CFG_HEX16: {
        int base = (spec->type == HOP_CFG_HEX16 ||
                    spec->offset == offsetof(hop_config, input_mode) ||
                    spec->offset == offsetof(hop_config, audio_mode) ||
                    spec->offset == offsetof(hop_config, data_port)) ? 0 : 10;
        long num = 0;
        if (!parse_long_strict(value, base, &num) || num < spec->min_val || num > spec->max_val) {
            if (err && err_cap)
                snprintf(err, err_cap, "%s must be in range %d..%d",
                         spec->key, spec->min_val, spec->max_val);
            return -1;
        }
        write_int_field(c, spec, (int)num);
        return 1;
    }
    }
    return -1;
}

int hop_config_reset_key(hop_config *c, const char *key) {
    if (!c || !key) return 0;
    const hop_config_spec *spec = hop_config_spec_find(key);
    if (!spec) return 0;
    hop_config def;
    hop_config_defaults(&def);
    memcpy((unsigned char *)c + spec->offset, (const unsigned char *)&def + spec->offset, spec->size);
    return 1;
}

int hop_config_format_value(const hop_config *c, const hop_config_spec *spec,
                            char *out, size_t cap) {
    if (!c || !spec || !out || !cap) return 0;
    switch (spec->type) {
    case HOP_CFG_STRING: {
        const char *src = (const char *)c + spec->offset;
        if (!valid_text_buf(src, spec->size)) return 0;
        int n = snprintf(out, cap, "%s", src);
        return n >= 0 && (size_t)n < cap;
    }
    case HOP_CFG_BOOL:
    case HOP_CFG_INT:
    case HOP_CFG_U16: {
        int v = read_int_field(c, spec);
        int n = snprintf(out, cap, "%d", v);
        return n >= 0 && (size_t)n < cap;
    }
    case HOP_CFG_HEX16: {
        int v = read_int_field(c, spec);
        int n = snprintf(out, cap, "0x%04X", (unsigned)(v & 0xFFFF));
        return n >= 0 && (size_t)n < cap;
    }
    case HOP_CFG_ENUM: {
        int v = read_int_field(c, spec);
        return format_enum_name(spec->enum_values, v, out, cap);
    }
    }
    return 0;
}

int hop_config_values_equal(const hop_config *a, const hop_config *b,
                            const hop_config_spec *spec) {
    if (!a || !b || !spec) return 0;
    if (spec->type == HOP_CFG_STRING) {
        const char *sa = (const char *)a + spec->offset;
        const char *sb = (const char *)b + spec->offset;
        return strncmp(sa, sb, spec->size) == 0;
    }
    return read_int_field(a, spec) == read_int_field(b, spec);
}

int hop_config_validate(const hop_config *c, int strict_runtime,
                        char *err, size_t err_cap) {
    if (!c) {
        set_err(err, err_cap, "config is null");
        return 0;
    }
    for (size_t i = 0; i < k_spec_count; ++i) {
        const hop_config_spec *s = &k_specs[i];
        if (s->type == HOP_CFG_STRING) {
            const char *txt = (const char *)c + s->offset;
            if (!valid_text_buf(txt, s->size)) {
                if (err && err_cap) snprintf(err, err_cap, "%s contains invalid text", s->key);
                return 0;
            }
        } else {
            int v = read_int_field(c, s);
            if (v < s->min_val || v > s->max_val) {
                if (err && err_cap)
                    snprintf(err, err_cap, "%s (%d) out of range %d..%d",
                             s->key, v, s->min_val, s->max_val);
                return 0;
            }
        }
    }
    if (c->disp_shared) {
        static const char prefix[] = "\\\\.\\DISPLAY";
        const char *suffix = c->disp_device + sizeof(prefix) - 1;
        int valid = strncmp(c->disp_device, prefix, sizeof(prefix) - 1) == 0;
        if (valid) {
            valid = *suffix >= '1' && *suffix <= '9';
            for (; *suffix; ++suffix) if (*suffix < '0' || *suffix > '9') valid = 0;
        }
        if (!valid) {
            set_err(err, err_cap, "disp_shared requires a Windows secondary disp_device (e.g. \\\\.\\DISPLAY2)");
            return 0;
        }
        if (c->disp_win_input == c->disp_mac_input) {
            set_err(err, err_cap, "shared display Windows and Mac inputs must be distinct");
            return 0;
        }
    }
    if (c->port == c->data_port || c->port == c->audio_port || c->data_port == c->audio_port) {
        set_err(err, err_cap, "port, data_port, and audio_port must all be distinct");
        return 0;
    }
    if (c->net_heartbeat_timeout_ms <= c->net_heartbeat_interval_ms) {
        set_err(err, err_cap, "net_heartbeat_timeout_ms must be greater than net_heartbeat_interval_ms");
        return 0;
    }
    if (c->net_ctrl_queue_hard < c->net_ctrl_queue_soft) {
        set_err(err, err_cap, "net_ctrl_queue_hard must be >= net_ctrl_queue_soft");
        return 0;
    }
    if (c->audio_peer_timeout_ms <= c->audio_keepalive_ms) {
        set_err(err, err_cap, "audio_peer_timeout_ms must be greater than audio_keepalive_ms");
        return 0;
    }
    if (c->xfer_max_image_temp_mb < c->xfer_max_image_mb) {
        set_err(err, err_cap, "xfer_max_image_temp_mb must be >= xfer_max_image_mb");
        return 0;
    }
    if (strict_runtime) {
        if (!c->device_name[0]) {
            set_err(err, err_cap, "name (device_name) must not be empty");
            return 0;
        }
        if (!c->token[0]) {
            set_err(err, err_cap, "token must not be empty");
            return 0;
        }
        if (c->role == HOP_ROLE_CLIENT && !c->peer_host[0]) {
            set_err(err, err_cap, "host (peer_host) must not be empty when role=client");
            return 0;
        }
    }
    return 1;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

#if defined(_WIN32)
static int config_utf8_path(const char *src, wchar_t *dst, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, cap) > 0;
}
#endif

int hop_config_load_ex(hop_config *c, const char *path,
                       int *file_missing, char *err, size_t err_cap) {
    if (file_missing) *file_missing = 0;
    if (!c || !path || !path[0]) {
        set_err(err, err_cap, "invalid config path");
        return 0;
    }
#if defined(_WIN32)
    wchar_t wide[1024];
    if (!config_utf8_path(path, wide, 1024)) {
        set_err(err, err_cap, "invalid UTF-8 config path");
        return 0;
    }
    FILE *f = _wfopen(wide, L"r");
#else
    FILE *f = fopen(path, "r");
#endif
    if (!f) {
        if (file_missing) *file_missing = 1;
        set_err(err, err_cap, "config file not found");
        return 0;
    }

    hop_config candidate = *c;
    int recognized = 0, invalid = 0, saw_role = 0, saw_input_mode = 0;
    int line_no = 0;
    char line[1024];
    char first_err[192] = {0};

    while (fgets(line, sizeof(line), f)) {
        ++line_no;
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);
        char kv_err[128] = {0};
        int result = hop_config_set_kv(&candidate, k, v, kv_err, sizeof(kv_err));
        if (result < 0) {
            invalid = 1;
            if (!first_err[0])
                snprintf(first_err, sizeof(first_err), "line %d: %s", line_no, kv_err);
        } else {
            recognized += result;
            if (strcmp(k, "role") == 0) saw_role = 1;
            else if (strcmp(k, "input_mode") == 0) saw_input_mode = 1;
        }
    }
    if (saw_role && !saw_input_mode)
        candidate.input_mode = candidate.role == HOP_ROLE_SERVER ? HOP_INPUT_MODES : 0;

    int io_err = ferror(f) != 0;
    if (fclose(f) != 0) io_err = 1;

    if (io_err) {
        set_err(err, err_cap, "I/O error reading config file");
        return 0;
    }
    if (invalid) {
        set_err(err, err_cap, first_err[0] ? first_err : "invalid config value");
        return 0;
    }
    if (recognized == 0) {
        set_err(err, err_cap, "no recognized config keys in file");
        return 0;
    }
    if (!hop_config_validate(&candidate, 0, err, err_cap))
        return 0;

    *c = candidate;
    return 1;
}

int hop_config_load(hop_config *c, const char *path) {
    return hop_config_load_ex(c, path, NULL, NULL, 0);
}

int hop_config_save_atomic(const hop_config *c, const char *path) {
    if (!c || !path || !path[0] || strlen(path) > 900 ||
        !hop_config_validate(c, 0, NULL, 0))
        return 0;

    char temp[1024];
    int temp_len = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (temp_len <= 0 || (size_t)temp_len >= sizeof(temp)) return 0;

#if defined(_WIN32)
    wchar_t wtemp[1024], wpath[1024];
    if (!config_utf8_path(temp, wtemp, 1024) || !config_utf8_path(path, wpath, 1024)) return 0;
    FILE *f = _wfopen(wtemp, L"wb");
#else
    snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (fd >= 0 && !f) { close(fd); unlink(temp); }
#endif
    if (!f) return 0;

    int ok = 1;
    if (fprintf(f, "# HopDesk configuration — managed by HopDesk UI / CLI\n") < 0) ok = 0;
    for (size_t i = 0; i < k_spec_count && ok; ++i) {
        char formatted[512];
        if (!hop_config_format_value(c, &k_specs[i], formatted, sizeof(formatted))) {
            ok = 0;
            break;
        }
        if (fprintf(f, "%s=%s\n", k_specs[i].key, formatted) < 0) ok = 0;
    }
    if (fflush(f) != 0) ok = 0;
    if (ok) {
#if defined(_WIN32)
        if (_commit(_fileno(f)) != 0) ok = 0;
#else
        if (fsync(fileno(f)) != 0) ok = 0;
#endif
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
#if defined(_WIN32)
        _wremove(wtemp);
#else
        unlink(temp);
#endif
        return 0;
    }
#if defined(_WIN32)
    if (!MoveFileExW(wtemp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        _wremove(wtemp);
        return 0;
    }
#else
    if (rename(temp, path) != 0) {
        unlink(temp);
        return 0;
    }
    (void)chmod(path, 0600);
#endif
    return 1;
}
