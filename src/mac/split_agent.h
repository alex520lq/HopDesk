#ifndef HOPDESK_MAC_SPLIT_AGENT_H
#define HOPDESK_MAC_SPLIT_AGENT_H

/* Start after Core/xfer initialization; owns the IPC listener until run exits. */
int split_agent_start(void);
void split_agent_set_open_on_launch(int open);
void split_agent_run(void);
int split_agent_is_active(void);
void split_agent_set_status(int connected, int sharing);
int split_agent_stop_if_idle(void);   /* main thread */
void split_agent_stop_for_update(void); /* main thread, after update admission closed */

#endif
