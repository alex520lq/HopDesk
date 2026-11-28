/* =====================================================================
 *  HopDesk - Core Runtime Config Application (Platform-Independent)
 *  Propagates hop_config tunables into Core subsystems without coupling
 *  standalone config unit tests to log/keymap/protocol/budget modules.
 * ===================================================================== */
#include "hop.h"
#include "xfer_budget.h"
#include "xfer_rx_core.h"

void hop_runtime_apply_config(const hop_config *c) {
    if (!c) return;

    /* 1. Logging & UI language */
    hop_log_configure((hop_log_level)c->log_level,
                      (long)c->log_rotate_mb * 1024L * 1024L);
    hop_i18n_set_lang(c->ui_lang);

    /* 2. Keymap modifiers */
    hop_keymap_configure(c->mod_ctrl, c->mod_alt, c->mod_win);

    /* 3. Control protocol heartbeat, handshake & queue thresholds */
    hop_protocol_configure(
        c->net_heartbeat_interval_ms > 0 ? (uint32_t)c->net_heartbeat_interval_ms : HOP_HEARTBEAT_INTERVAL_MS,
        c->net_heartbeat_timeout_ms  > 0 ? (uint32_t)c->net_heartbeat_timeout_ms  : HOP_HEARTBEAT_TIMEOUT_MS,
        c->net_handshake_timeout_ms  > 0 ? (uint32_t)c->net_handshake_timeout_ms  : HOP_HANDSHAKE_TIMEOUT_MS,
        c->net_ctrl_queue_soft       > 0 ? c->net_ctrl_queue_soft                 : HOP_CTRL_QUEUE_SOFT,
        c->net_ctrl_queue_hard       > 0 ? c->net_ctrl_queue_hard                 : HOP_CTRL_QUEUE_HARD,
        c->net_ctrl_queue_mb         > 0 ? (size_t)c->net_ctrl_queue_mb * 1024u * 1024u : HOP_CTRL_QUEUE_BYTES);

    /* 4. Receiver staging root & disconnect grace budget */
    hop_xfer_rx_configure(
        c->xfer_rx_dir,
        c->xfer_rx_disconnect_budget_ms >= 5000
            ? (uint64_t)c->xfer_rx_disconnect_budget_ms
            : (uint64_t)HOP_XFER_RX_DISCONNECT_BUDGET_MS);

    /* 4. Transfer global budget limits */
    hop_xfer_budget_limits limits;
    limits.max_jobs = c->xfer_max_jobs > 0 ? (uint32_t)c->xfer_max_jobs : HOP_XFER_DEFAULT_MAX_JOBS;
    limits.max_entries = c->xfer_max_entries > 0 ? (uint32_t)c->xfer_max_entries : HOP_XFER_DEFAULT_MAX_ENTRIES;
    limits.max_pending_bytes = c->xfer_max_pending_gib > 0
        ? (uint64_t)c->xfer_max_pending_gib * 1024ull * 1024ull * 1024ull
        : HOP_XFER_DEFAULT_MAX_PENDING_BYTES;
    limits.max_image_bytes = c->xfer_max_image_mb > 0
        ? (uint64_t)c->xfer_max_image_mb * 1024ull * 1024ull
        : HOP_XFER_DEFAULT_MAX_IMAGE_BYTES;
    limits.max_single_image_bytes = limits.max_image_bytes;
    limits.max_image_temp_bytes = c->xfer_max_image_temp_mb > 0
        ? (uint64_t)c->xfer_max_image_temp_mb * 1024ull * 1024ull
        : HOP_XFER_DEFAULT_MAX_IMAGE_TEMP_BYTES;
    if (limits.max_image_temp_bytes < limits.max_image_bytes)
        limits.max_image_temp_bytes = limits.max_image_bytes;
    limits.max_rx_queue_slots = c->xfer_rx_queue_slots > 0
        ? (uint32_t)c->xfer_rx_queue_slots : HOP_XFER_DEFAULT_RX_QUEUE_SLOTS;
    limits.max_rx_queue_bytes = c->xfer_rx_queue_mb > 0
        ? (uint64_t)c->xfer_rx_queue_mb * 1024ull * 1024ull
        : HOP_XFER_DEFAULT_RX_QUEUE_BYTES;
    (void)hop_xfer_budget_apply_limits(&limits);
}
