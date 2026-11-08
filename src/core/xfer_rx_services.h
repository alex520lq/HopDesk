#ifndef HOP_XFER_RX_SERVICES_H
#define HOP_XFER_RX_SERVICES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform boundary used by receiver protocol handlers. All callbacks are
 * invoked on the receiver dispatch thread unless explicitly documented by
 * the owning adapter. The context remains owned by the platform adapter. */
typedef struct {
    void *context;
    void (*shutdown_data_channel)(void *context);
    void (*mark_sender_aborted)(void *context, uint32_t batch, uint8_t reason);
    void (*send_file_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*send_done_ack)(void *context, uint32_t batch,
                          const uint8_t *payload, uint32_t length);
    void (*deliver_ack)(void *context, uint32_t batch, uint32_t id, int status);
} hop_xfer_rx_services;

#ifdef __cplusplus
}
#endif
#endif
