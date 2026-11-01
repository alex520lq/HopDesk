#ifndef HOP_UPDATE_STATE_H
#define HOP_UPDATE_STATE_H
/* Short-lived reservations cover submission/scanning and receive dispatch.
 * Installation closes admission atomically, then inspects the existing queues.
 * Never hold a reservation for the lifetime of a network connection. */
int hop_update_work_enter(void);
/* Receive dispatch waits through the busy probe, so a rejected installation
 * cannot spuriously drop an active transfer. Same work_leave on success. */
int hop_update_receive_enter(void);
void hop_update_work_leave(void);
int hop_update_install_begin(int (*busy)(void));
void hop_update_install_end(void);
/* Called by the initialized UI event loop, not by the process entry point.
 * Emits a version/nonce/PID receipt for the detached installer, if requested. */
void hop_update_report_ready(const char *version);
#endif
