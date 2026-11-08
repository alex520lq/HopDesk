#ifndef HOP_XFER_RX_IMAGE_BUFFER_H
#define HOP_XFER_RX_IMAGE_BUFFER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t *data; uint64_t capacity; } hop_xfer_rx_image_buffer;
/* Allocate once for the declared image size. Existing storage is reused only
 * when it is already large enough; this never grows an unreserved buffer. */
int hop_xfer_rx_image_buffer_ensure(hop_xfer_rx_image_buffer *buffer, uint64_t expected);
void hop_xfer_rx_image_buffer_dispose(hop_xfer_rx_image_buffer *buffer);
#ifdef __cplusplus
}
#endif
#endif
