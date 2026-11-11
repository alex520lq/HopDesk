#include "xfer_rx_image_buffer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    hop_xfer_rx_image_buffer buffer = {0};
    assert(!hop_xfer_rx_image_buffer_ensure(&buffer, 0));
    assert(hop_xfer_rx_image_buffer_ensure(&buffer, 16));
    assert(buffer.data && buffer.capacity == 16);
    uint8_t *original = buffer.data;
    assert(hop_xfer_rx_image_buffer_ensure(&buffer, 8));
    assert(buffer.data == original && buffer.capacity == 16);
    assert(!hop_xfer_rx_image_buffer_ensure(&buffer, 32));
    assert(buffer.data == original && buffer.capacity == 16);
    hop_xfer_rx_image_buffer_dispose(&buffer);
    assert(!buffer.data && !buffer.capacity);
    hop_xfer_rx_image_buffer_dispose(&buffer);
    hop_xfer_rx_image_buffer_dispose(NULL);
    puts("PASS: receiver image buffer exact allocation, bounded reuse and disposal");
    return 0;
}
