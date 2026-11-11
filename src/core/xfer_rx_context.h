#ifndef HOP_XFER_RX_CONTEXT_H
#define HOP_XFER_RX_CONTEXT_H

#include "xfer_rx_batch_state.h"
#include "xfer_rx_entry_reservations.h"
#include "xfer_rx_file_state.h"
#include "xfer_rx_image_buffer.h"
#include "xfer_rx_image_state.h"
#include "xfer_rx_lifecycle.h"
#include "xfer_rx_receipt.h"
#include "xfer_rx_services.h"
#include "xfer_task.h"

#include <wchar.h>

#if defined(_WIN32) || defined(FIXTURE_WINDOWS)
#define HOP_XFER_RX_CONTEXT_PATH_CAPACITY 260
typedef wchar_t hop_xfer_rx_path_char;
#else
#define HOP_XFER_RX_CONTEXT_PATH_CAPACITY 4096
typedef char hop_xfer_rx_path_char;
#endif

/* Receiver-owned mutable state shared by the protocol adapter and lifecycle
 * layer. Platform differences are limited to the native staging path type;
 * operating-system actions remain behind hop_xfer_rx_services. */
typedef struct xfer_rx_context {
    hop_xfer_rx_batch_state batch_state;
    hop_xfer_task task;
    hop_xfer_rx_entry_reservations entry_reservations;
    hop_xfer_rx_path_char dir[HOP_XFER_RX_CONTEXT_PATH_CAPACITY];
    int staging_cleanup_pending; /* Keep dir until a failed staging deletion is retried successfully. */
    hop_xfer_rx_path_char *cur_path;
    hop_xfer_rx_file_state file_state;
    hop_xfer_rx_image_buffer image_buffer;
    hop_xfer_rx_image_state image_state;
    hop_xfer_rx_path_char **tops;
    int ntop;
    void *writer; /* injected platform writer queue; owned by the platform runtime */
    hop_xfer_rx_lifecycle *lifecycle;
    hop_xfer_rx_done_receipt *done_receipt;
    hop_xfer_rx_services services;
} xfer_rx_context;

#endif
