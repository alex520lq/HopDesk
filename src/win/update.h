#ifndef HOP_WIN_UPDATE_H
#define HOP_WIN_UPDATE_H

#include <windows.h>

/* Starts a background update check. Network I/O and download never block the UI loop. */
void update_check_async(HWND owner);

#endif
