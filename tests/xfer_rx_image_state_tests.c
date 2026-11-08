#include "xfer_rx_image_state.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    hop_xfer_rx_image_state s = {0};
    assert(!hop_xfer_rx_image_state_begin(&s,0,2,8,16));
    assert(hop_xfer_rx_image_state_begin(&s,2,2,8,16));
    assert(s.width==2 && s.height==2 && s.stride==8 && s.expected==16 && s.received==0);
    assert(hop_xfer_rx_image_state_check_chunk(&s,0,8)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,0,8));
    assert(s.received==8);
    assert(hop_xfer_rx_image_state_check_chunk(&s,0,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,9,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,9)==HOP_RX_IMAGE_CHUNK_INVALID);
    hop_xfer_rx_image_state_set_capacity(&s,12);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,5)==HOP_RX_IMAGE_CHUNK_INVALID);
    assert(hop_xfer_rx_image_state_check_chunk(&s,8,4)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,8,4));
    assert(s.received==12);
    assert(hop_xfer_rx_image_state_check_chunk(&s,12,4)==HOP_RX_IMAGE_CHUNK_ACCEPT);
    assert(hop_xfer_rx_image_state_record_chunk(&s,12,4));
    assert(s.received==16);
    assert(hop_xfer_rx_image_state_check_chunk(&s,16,1)==HOP_RX_IMAGE_CHUNK_INVALID);
    hop_xfer_rx_image_state_reset(&s);
    assert(!s.expected && !s.received && !s.capacity && !s.width);
    puts("PASS: receiver image metadata, contiguous chunk and capacity rules");
    return 0;
}
