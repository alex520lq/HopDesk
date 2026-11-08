#include "hop.h"
#include "xfer_status.h"
#include "xfer_rx_ack_handler.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { int calls; uint32_t batch, id, status; } capture;
static void file_ack(void *context, uint32_t batch, uint32_t id, uint8_t status) {
    capture *out = (capture *)context;
    out->calls++; out->batch = batch; out->id = id; out->status = status;
}
static void done_ack(void *context, uint32_t batch, uint32_t status) {
    capture *out = (capture *)context;
    out->calls++; out->batch = batch; out->id = HOP_DONE_ACK_SLOT; out->status = status;
}
int main(void) {
    capture out = {0};
    uint8_t file[9] = {0};
    hop_put_be32(file, 42); hop_put_be32(file + 4, 7); file[8] = 1;
    assert(hop_xfer_rx_handle_file_ack(file, sizeof(file), file_ack, &out));
    assert(out.calls == 1 && out.batch == 42 && out.id == 7 && out.status == 1);
    assert(!hop_xfer_rx_handle_file_ack(file, 8, file_ack, &out));
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), NULL, &out));
    hop_put_be32(file + 4, 0);
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), file_ack, &out));
    hop_put_be32(file + 4, HOP_XFER_FILE_LIMIT + 1u);
    assert(!hop_xfer_rx_handle_file_ack(file, sizeof(file), file_ack, &out));
    assert(out.calls == 1);

    uint8_t done[5] = {0};
    hop_put_be32(done, 99); done[4] = HOP_DONE_ACK_FAILED;
    assert(hop_xfer_rx_handle_done_ack(done, sizeof(done), done_ack, &out));
    assert(out.calls == 2 && out.batch == 99 && out.id == HOP_DONE_ACK_SLOT &&
           out.status == HOP_DONE_ACK_FAILED);
    assert(!hop_xfer_rx_handle_done_ack(NULL, sizeof(done), done_ack, &out));
    assert(!hop_xfer_rx_handle_done_ack(done, 4, done_ack, &out));
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), NULL, &out));
    hop_put_be32(done, 0);
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), done_ack, &out));
    hop_put_be32(done, 99); done[4] = HOP_DONE_ACK_FAILED + 1;
    assert(!hop_xfer_rx_handle_done_ack(done, sizeof(done), done_ack, &out));
    assert(out.calls == 2);
    puts("PASS: receiver file/done ACK decode, validation and callback dispatch");
    return 0;
}
