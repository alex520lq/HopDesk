#ifndef HOP_WIN_UPDATE_H
#define HOP_WIN_UPDATE_H

#include <windows.h>
#include "../core/update_controller.h"

/* Thread-safe snapshot of the three-stage in-app updater state. */
void update_get_snapshot(hop_update_snapshot *out);

/* Stage 1: fetch and evaluate the selected channel's update manifest asynchronously. */
void update_check_async(HWND owner, int target_channel);

/* Stage 2: download and SHA-256 verify the candidate package in the background. */
void update_download_async(HWND owner);

/* Stage 3: acquire install admission, hand off verified archive to helper, and quit to restart. */
void update_apply_async(HWND owner);

/* Clean up any staged archive on normal shutdown if not applied. */
void update_shutdown(void);

#endif
