#ifndef HOPDESK_WIN_SPLIT_UI_H
#define HOPDESK_WIN_SPLIT_UI_H
#include "app.h"
#include "../ui/hopdesk_ui.h"
#include "../core/cli.h"

/* Same executable, separate roles. Only the agent owns the engine. */
int win_split_agent_start(HWND owner);
void win_split_agent_stop(void);
void win_split_agent_open(int page);
void win_split_agent_reap(void);
void win_split_agent_dispatch(void);
int win_split_client_init(DWORD agent_pid);
void win_split_client_close(void);
int win_split_client_active(void);
int win_split_client_alive(void);
int win_split_snapshot(hopdesk_ui_model *model, hop_xfer_task_id task,
                       uint32_t offset, int *page);
int win_split_save(const hop_config *config); /* 1 saved + audio applied; 2 saved only */
int win_split_cancel(hop_xfer_task_id task);
int win_split_retry(uint32_t batch, int index);
int win_split_reconnect(void);
int win_split_disconnect(void);
int win_split_display(int action);
int win_split_update(void);
int win_split_set_theme(int ui_theme);
int win_split_set_input_mode(unsigned input_mode);

/* Headless local CLI -> running agent IPC (same-user ACL control pipe). */
int win_cli_agent_get_status(void *ctx, hop_cli_status *out);
int win_cli_agent_get_config(void *ctx, hop_config *out);
int win_cli_agent_save_config(void *ctx, const hop_config *cfg);
int win_cli_agent_action(void *ctx, hop_cli_action action);
#endif
