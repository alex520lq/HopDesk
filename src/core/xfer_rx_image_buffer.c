#include "xfer_rx_image_buffer.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
int hop_xfer_rx_image_buffer_ensure(hop_xfer_rx_image_buffer *buffer, uint64_t expected) {
    if (!buffer || !expected || expected > (uint64_t)SIZE_MAX) return 0;
    if (buffer->data) return buffer->capacity >= expected;
    uint8_t *data = (uint8_t *)malloc((size_t)expected);
    if (!data) return 0;
    buffer->data = data;
    buffer->capacity = expected;
    return 1;
}
void hop_xfer_rx_image_buffer_dispose(hop_xfer_rx_image_buffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}
