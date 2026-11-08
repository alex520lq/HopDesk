#include "hop.h"
#include "xfer_rx_context.h"
#include "xfer_tracking.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rx_now_ns(xfer_rx_context *rx) {
    return rx && rx->services.now_ns
        ? rx->services.now_ns(rx->services.context) : 0;
}
static void rx_handler_terminate(xfer_rx_context *rx, uint32_t batch, uint8_t reason, int notify_peer) {
    if (rx && rx->services.terminate_batch)
        rx->services.terminate_batch(rx->services.context, batch, reason, notify_peer);
}

void hop_xfer_rx_handle_image_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_image_begin_msg msg;
            if (!hop_xfer_decode_image_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed IMAGE_BEGIN rejected");
                if (rx->batch_state.active) rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint32_t batch = msg.batch;
            rx_handler_begin_batch(rx, batch);
            if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
            (void)hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_IMAGE);

            int w = (int)msg.width;
            int h = (int)msg.height;
            int stride = (int)msg.stride;

            /* 对端声称的尺寸必须自己验：不然 w=h=100000 就能把内存吃光。
             * 这正是"避免内存不断增长"的边界。 */
            if (!hop_xfer_image_valid(w, h, stride)) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            size_t expected = (size_t)stride * (size_t)h;
            if (!rx->file_state.is_image) {
                if (!hop_xfer_task_reserve_image(&rx->task,(uint64_t)expected)) {
                    rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
            } else if (rx->image_state.expected != expected) {
                rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            hop_xfer_rx_file_state_begin(&rx->file_state, msg.id);
            hop_xfer_rx_file_state_set_kind(&rx->file_state, 1, 0);
            if (!hop_xfer_rx_image_state_begin(&rx->image_state,
                    (uint32_t)w, (uint32_t)h, (uint32_t)stride, (uint64_t)expected)) {
                rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            if (rx->image_buffer.data) hop_xfer_rx_image_state_set_capacity(
                &rx->image_state, rx->image_buffer.capacity);
            if (rx->services.status_image) rx->services.status_image(rx->services.context,batch,rx->image_state.expected);
            return;
}

void hop_xfer_rx_handle_file_chunk(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
if (!rx->batch_state.active) return; /* late data after termination */
            uint64_t receive_started_ns=rx_now_ns(rx);
            hop_xfer_file_chunk_msg chunk;
            if (!hop_xfer_decode_file_chunk(p, len,
                    rx->file_state.is_image ? rx->image_state.expected :
                        (rx->file_state.failure >= 0 ? UINT64_MAX : rx->file_state.declared_size),
                    rx->file_state.is_image, &chunk)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed or out-of-range chunk; aborting receive");
                rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint64_t off = chunk.offset;
            uint32_t n = chunk.length;
            uint32_t crc = chunk.crc;
            if (chunk.id != rx->file_state.id) {
                rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_CRC,1);
                return;
            }
            const uint8_t *data = chunk.data;

            /* 逐块校验。TCP 的 16 位校验和 + 网卡 offload 有漏检可能，
             * 漏检一次就是静默写入损坏数据。这里宁可报错也不要写坏文件。
             *
             * 坏一块只作废这一个文件，不再 ABORT 整批：
             * 发送端收到 ACK_CRC 会单独重发它，其余文件照常落地。 */
            uint64_t crc_started_ns=rx_now_ns(rx);
            int crc_ok=(hop_crc32(0, data, n) == crc);
            uint64_t crc_elapsed_ns=rx_now_ns(rx)-crc_started_ns;
            if (!crc_ok) {
                if (rx->services.status_stage_time) rx->services.status_stage_time(rx->services.context,rx->batch_state.batch,HOP_XFER_TIME_CRC,crc_elapsed_ns);
                if (rx->services.status_stage_time) rx->services.status_stage_time(rx->services.context,rx->batch_state.batch,HOP_XFER_TIME_RECEIVE,
                                  rx_now_ns(rx)-receive_started_ns);
                hop_log(HOP_LOG_ERROR,
                        "xfer: CHUNK CRC MISMATCH off=%llu len=%u (expect %08x)",
                        (unsigned long long)off, n, crc);
                if (rx->file_state.is_image) {
                    rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_CRC,1);
                } else {
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_CRC);
                }
                return;
            }

            if (rx->file_state.is_image) {
                if (hop_xfer_rx_image_state_check_chunk(
                        &rx->image_state, off, n) != HOP_RX_IMAGE_CHUNK_ACCEPT) {
                    hop_log(HOP_LOG_ERROR, "xfer: image chunk gap/overlap or length overflow");
                    rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                size_t need = (size_t)(off + n);
                /* IMAGE_BEGIN already reserved img_expected bytes in the
                 * task ledger. Allocate that exact amount once: geometric growth
                 * would make the real capacity exceed the reservation and may
                 * transiently keep both old and new buffers alive during realloc. */
                if (!hop_xfer_rx_image_buffer_ensure(&rx->image_buffer,
                        (uint64_t)rx->image_state.expected)) {
                    rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_DISK,1);
                    return;
                }
                hop_xfer_rx_image_state_set_capacity(
                    &rx->image_state, rx->image_buffer.capacity);
                if (need > rx->image_state.capacity) {
                    hop_log(HOP_LOG_ERROR, "xfer: image buffer capacity exceeded");
                    rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                memcpy(rx->image_buffer.data + off, data, n);
                if (!hop_xfer_rx_image_state_record_chunk(&rx->image_state, off, n)) {
                    rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                uint64_t receive_elapsed_ns=rx_now_ns(rx)-receive_started_ns;
                if (rx->services.status_chunk_received) rx->services.status_chunk_received(rx->services.context,rx->batch_state.batch,rx->file_state.id,off,n,crc_elapsed_ns,receive_elapsed_ns);
                return;
            }

            /* TCP is ordered: reject gaps, overlaps and chunks after a prior
             * file failure instead of creating sparse or truncated files. */
            hop_xfer_rx_file_chunk_check chunk_check =
                hop_xfer_rx_file_state_check_chunk(&rx->file_state, off, n);
            if (chunk_check == HOP_RX_FILE_CHUNK_ALREADY_FAILED) return;
            if (chunk_check != HOP_RX_FILE_CHUNK_ACCEPT) {
                hop_log(HOP_LOG_ERROR, "xfer: non-contiguous file chunk rejected (off=%llu received=%llu size=%llu)",
                        (unsigned long long)off, (unsigned long long)rx->file_state.received,
                        (unsigned long long)rx->file_state.declared_size);
                hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_CRC);
                return;
            }

            /* 校验通过才入队。拷贝一份交给写线程 —— 解码器的缓冲
             * 在回调返回后就被释放了，不能把指针递出去。 */
            if (!rx->services.write_file_chunk && rx->services.write_file_chunk(rx->services.context, &rx->task, off, data, n)) {
                hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
                return;
            }
            /* Queue admission succeeded; only now commit offset and file CRC. */
            if (!hop_xfer_rx_file_state_record_chunk(&rx->file_state, off, n, crc)) {
                hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_CRC);
                return;
            }
            uint64_t receive_elapsed_ns=rx_now_ns(rx)-receive_started_ns;
            if (rx->services.status_chunk_received) rx->services.status_chunk_received(rx->services.context,rx->batch_state.batch,rx->file_state.id,off,n,crc_elapsed_ns,receive_elapsed_ns);
            return;
}
