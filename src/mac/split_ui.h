#ifndef HOPDESK_MAC_SPLIT_UI_H
#define HOPDESK_MAC_SPLIT_UI_H

#include "xfer.h"
#include "xfer_task_history.h"
#include "../platform/audio_link.h"

/* Normal UI entry points dispatch to the in-process engine in single-process mode. */
int split_ui_agent_available(void);
int split_ui_run(int start_agent);
int split_ui_is_active(void);
void split_ui_refresh_connection(int *connected, int *sharing);
int split_ui_data_connected(void);
void split_ui_get_status_pair(hop_xfer_status_pair *out);
int split_ui_get_tasks(hop_xfer_task_snapshot *out, int capacity, int *total);
int split_ui_get_task(hop_xfer_task_id id, hop_xfer_task_snapshot *out);
int split_ui_get_history(hop_xfer_task_history_record *out, int capacity, int direction,
                         hop_xfer_task_state state, int newest, int *total);
int split_ui_get_files(int direction, uint32_t batch, hop_xfer_file_status *out, int capacity);
int split_ui_cancel_task(hop_xfer_task_id id);
int split_ui_retry_file(uint32_t batch, int index);
int split_ui_set_audio_mode(unsigned mode);
void split_ui_get_audio_status(hop_audio_status *out);
void split_ui_reconnect(void);

#endif
