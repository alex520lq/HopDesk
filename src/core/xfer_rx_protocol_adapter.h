#ifndef HOP_XFER_RX_PROTOCOL_ADAPTER_H
#define HOP_XFER_RX_PROTOCOL_ADAPTER_H

#include "xfer_rx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit receiver protocol boundary. The adapter owns the handler table and
 * the platform context; protocol routing itself is platform independent.
 * The context must outlive every dispatch made through this adapter. */
typedef struct {
    hop_xfer_rx_dispatch_handlers handlers;
    void *context;
    int initialized;
} hop_xfer_rx_protocol_adapter;

int hop_xfer_rx_protocol_adapter_init(
    hop_xfer_rx_protocol_adapter *adapter,
    const hop_xfer_rx_dispatch_handlers *handlers,
    void *context);

int hop_xfer_rx_protocol_adapter_dispatch(
    hop_xfer_rx_protocol_adapter *adapter,
    uint8_t message_type,
    const uint8_t *payload,
    uint32_t payload_length);

#ifdef __cplusplus
}
#endif
#endif
