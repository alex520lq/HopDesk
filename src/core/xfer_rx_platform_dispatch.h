#ifndef HOP_XFER_RX_PLATFORM_DISPATCH_H
#define HOP_XFER_RX_PLATFORM_DISPATCH_H

#include "xfer_rx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by the platform-specific receiver protocol translation unit. */
extern const hop_xfer_rx_dispatch_handlers hop_xfer_rx_platform_dispatch_handlers;

#ifdef __cplusplus
}
#endif
#endif
