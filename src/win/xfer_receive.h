#ifndef HOP_WIN_XFER_RECEIVE_H
#define HOP_WIN_XFER_RECEIVE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receiver-owned lifecycle boundaries. xfer.c starts/stops the writer queue
 * and data-channel thread, then uses the audit predicate before destroying
 * shared synchronization. Receiver context and wire dispatch stay private. */
int xfer_receive_start(void);
int xfer_receive_stop(void);
int xfer_receive_owner_clean(void);

DWORD WINAPI xfer_receive_listener_thread(LPVOID argument);
DWORD WINAPI xfer_receive_client_thread(LPVOID argument);

#ifdef __cplusplus
}
#endif
#endif
