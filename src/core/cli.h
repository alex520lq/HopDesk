/* =====================================================================
 *  HopDesk - Headless CLI Engine (Platform-Independent, Pure C)
 *  Provides machine-readable (--json) and human-readable configuration,
 *  validation, status, and runtime action commands without opening GUI.
 * ===================================================================== */
#ifndef HOP_CLI_H
#define HOP_CLI_H

#include "hop.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      running;
    int      agent_pid;
    int      connected;
    int      sharing;
    int      data_connected;
    int      manually_disconnected;
    int      active_tasks;
    unsigned input_mode;
    unsigned peer_input_mode;
    unsigned audio_requested;
    unsigned audio_active;
    unsigned audio_unavailable;
    unsigned peer_audio_mode;
    int      peer_capabilities_known;
    hop_role role;
    char     peer_label[128];
} hop_cli_status;

typedef enum {
    HOP_CLI_ACTION_RECONNECT = 1,
    HOP_CLI_ACTION_DISCONNECT,
    HOP_CLI_ACTION_DISPLAY_MAC,
    HOP_CLI_ACTION_DISPLAY_WIN,
    HOP_CLI_ACTION_DISPLAY_WAKE,
    HOP_CLI_ACTION_DISPLAY_SLEEP,
    HOP_CLI_ACTION_RELOAD
} hop_cli_action;

typedef struct {
    const char *default_config_path;
    const char *default_log_path;
    const char *default_device_name;
    void (*write_out)(void *ctx, const char *data, size_t len);
    void (*write_err)(void *ctx, const char *data, size_t len);
    void *io_ctx;
    /* Live agent IPC callbacks (optional). Return:
     *   1  = agent reachable and operation succeeded
     *   0  = agent not running / unreachable
     *  -1  = agent reachable but rejected the request */
    int (*agent_get_status)(void *ctx, hop_cli_status *out);
    int (*agent_get_config)(void *ctx, hop_config *out);
    int (*agent_save_config)(void *ctx, const hop_config *cfg);
    int (*agent_action)(void *ctx, hop_cli_action action);
} hop_cli_ops;

/* Returns 1 if argv represents a CLI subcommand invocation rather than GUI/agent startup. */
int hop_cli_is_invocation(int argc, const char *const *argv);

/* Extracts the value of `--config <path>` or `--config=<path>` from argv, or NULL. */
const char *hop_cli_extract_config_flag(int argc, const char *const *argv);

/* Runs the CLI command and returns a process exit code (0 = success, 1 = error, 2 = usage). */
int hop_cli_run(int argc, const char *const *argv, const hop_cli_ops *ops);

#ifdef __cplusplus
}
#endif
#endif /* HOP_CLI_H */
