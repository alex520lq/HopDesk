#ifndef HOP_XFER_SENDER_CHUNK_H
#define HOP_XFER_SENDER_CHUNK_H

#include <stdint.h>
#include "xfer_rate.h"
#include "xfer_sender_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const hop_xfer_sender_context *context;
    hop_xfer_rate_limiter *rate;
    intptr_t socket_handle;
    uint8_t *tx_buffer;
    uint32_t tx_capacity;
    void (*rate_feedback)(uint64_t ack_rtt_us);
} hop_xfer_sender_chunk_ops;

/* Shared FILE_CHUNK frame construction, token-bucket pacing, serialized send
 * and stage telemetry. tx_buffer must hold the chunk header and, when data is
 * tx_buffer + header, the entire chunk. */
int hop_xfer_sender_send_chunk(const hop_xfer_sender_chunk_ops *ops,
                               uint32_t batch, uint32_t id, uint64_t offset,
                               const uint8_t *data, uint32_t length,
                               uint32_t crc, uint64_t read_ns,
                               uint64_t crc_ns);

#ifdef __cplusplus
}
#endif
#endif
