#include "hop.h"
#include "xfer_rx_context.h"
#include "xfer_rx_message_handlers.h"
#include "xfer_rx_ack_handler.h"
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

static void rx_handler_begin_batch(xfer_rx_context *rx, uint32_t batch) {
    if (rx && rx->services.begin_batch)
        rx->services.begin_batch(rx->services.context, batch);
}


static int rx_handler_finish_batch(xfer_rx_context *rx, uint32_t batch, uint32_t ok, uint32_t total) {
    if (!rx || !rx->services.finish_batch) return -1;
    return rx->services.finish_batch(rx->services.context, batch, ok, total);
}

void hop_xfer_rx_handle_image_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_image_begin_msg msg;
            if (!hop_xfer_decode_image_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed IMAGE_BEGIN rejected");
                if (rx->batch_state.active) rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint32_t batch = msg.batch;
            rx_handler_begin_batch(rx, batch);
            if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
            if (!hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_IMAGE)) {
                rx_handler_terminate(rx, batch, HOP_ABORT_CANCEL, 1);
                return;
            }

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
            if (!rx->services.write_file_chunk || !rx->services.write_file_chunk(rx->services.context, &rx->task, off, data, n)) {
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


static void rx_ack_deliver_file(void *context, uint32_t batch, uint32_t id, uint8_t status) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.deliver_ack)
        rx->services.deliver_ack(rx->services.context, batch, id, status);
}

static void rx_ack_deliver_done(void *context, uint32_t batch, uint32_t status) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.deliver_ack)
        rx->services.deliver_ack(rx->services.context, batch, HOP_DONE_ACK_SLOT, (int)status);
}

void hop_xfer_rx_dispatch_file_ack_message(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    (void)hop_xfer_rx_handle_file_ack(p, len, rx_ack_deliver_file, rx);
}

void hop_xfer_rx_handle_abort(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_abort_msg abort_msg;
            if (!hop_xfer_decode_abort(p, len, &abort_msg)) return;
            uint32_t batch = abort_msg.batch;
            uint8_t reason = abort_msg.reason;

            if (abort_msg.target == HOP_ABORT_TO_SENDER && rx->services.mark_sender_aborted)
                rx->services.mark_sender_aborted(rx->services.context, batch, reason);

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。
             * 墓碑必须立 —— 否则对端再发一帧 FILE_BEGIN 就把目录又建回来了。 */
            if (abort_msg.target == HOP_ABORT_TO_RECEIVER) rx_handler_terminate(rx, batch,reason,0);
            return;
}

void hop_xfer_rx_handle_done(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_done_msg done;
            if (!hop_xfer_decode_done(p, len, &done)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed XFER_DONE ignored");
                return;
            }
            if (!done.batch || !done.total || done.total > HOP_XFER_FILE_LIMIT || done.ok > done.total) return;
            int result;
            const hop_xfer_rx_done_receipt *receipt =
                hop_xfer_rx_done_receipt_find(rx->done_receipt, hop_xfer_rx_lifecycle_session(rx->lifecycle), done.batch);
            if (receipt) {
                if (!hop_xfer_rx_done_receipt_matches_payload(receipt, &done)) return;
                result=receipt->status; /* repeat ACK without repeating clipboard effects */
            } else {
                result=rx_handler_finish_batch(rx, done.batch, done.ok, done.total);
                if (result<0) return;
                hop_xfer_rx_done_receipt_record(rx->done_receipt, hop_xfer_rx_lifecycle_session(rx->lifecycle),
                                                &done, (uint8_t)result);
            }
            uint8_t reply[5]; hop_put_be32(reply,done.batch); reply[4]=(uint8_t)result;
            if (rx->services.send_done_ack)
                rx->services.send_done_ack(rx->services.context, done.batch, reply, sizeof(reply));
            return;
}

void hop_xfer_rx_dispatch_done_ack_message(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    (void)hop_xfer_rx_handle_done_ack(p, len, rx_ack_deliver_done, rx);
}
