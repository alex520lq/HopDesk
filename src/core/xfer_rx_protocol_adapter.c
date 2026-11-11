#include "xfer_rx_protocol_adapter.h"

#include <string.h>

int hop_xfer_rx_protocol_adapter_init(
    hop_xfer_rx_protocol_adapter *adapter,
    const hop_xfer_rx_dispatch_handlers *handlers,
    void *context) {
    if (!adapter || !handlers) return 0;
    adapter->handlers = *handlers;
    adapter->context = context;
    adapter->initialized = 1;
    return 1;
}

int hop_xfer_rx_protocol_adapter_dispatch(
    hop_xfer_rx_protocol_adapter *adapter,
    uint8_t message_type,
    const uint8_t *payload,
    uint32_t payload_length) {
    if (!adapter || !adapter->initialized) return 0;
    return hop_xfer_rx_dispatch_message(
        message_type, payload, payload_length, &adapter->handlers, adapter->context);
}
