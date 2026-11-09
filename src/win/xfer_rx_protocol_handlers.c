#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "xfer.h"
#include "../core/xfer_tracking.h"
#include "xfer_rx_fs.h"
#include "../core/xfer_rx_context.h"
#include "../core/xfer_rx_message_handlers.h"
#include "../core/xfer_rx_platform_dispatch.h"

const hop_xfer_rx_dispatch_handlers hop_xfer_rx_platform_dispatch_handlers = {
    hop_xfer_rx_handle_file_begin,
    hop_xfer_rx_handle_image_begin,
    hop_xfer_rx_handle_file_chunk,
    hop_xfer_rx_handle_file_end,
    hop_xfer_rx_dispatch_file_ack_message,
    hop_xfer_rx_handle_abort,
    hop_xfer_rx_handle_done,
    hop_xfer_rx_dispatch_done_ack_message
};
