#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <time.h>
#include "xfer.h"
#include "../core/xfer_tracking.h"
#include "xfer_rx_fs.h"
#include "../core/xfer_rx_context.h"
#include "../core/xfer_rx_message_handlers.h"
#include "../core/xfer_rx_platform_dispatch.h"

static int rx_reserve_entry_budget(void *context, uint64_t bytes) {
    return hop_xfer_task_reserve_entry((hop_xfer_task *)context, bytes);
}

static void rx_handler_terminate(xfer_rx_context *rx, uint32_t batch, uint8_t reason, int notify_peer) {
    if (rx && rx->services.terminate_batch)
        rx->services.terminate_batch(rx->services.context, batch, reason, notify_peer);
}

static void rx_handler_begin_batch(xfer_rx_context *rx, uint32_t batch) {
    if (rx && rx->services.begin_batch)
        rx->services.begin_batch(rx->services.context, batch);
}

static void rx_dispatch_file_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_file_begin_msg msg;
            if (!hop_xfer_decode_file_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
                if (rx->batch_state.active) rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                if (rx->services.shutdown_data_channel)
                    rx->services.shutdown_data_channel(rx->services.context);
                return;
            }
            uint32_t batch = msg.batch;
            uint32_t id = msg.id;
            uint64_t size = msg.size;
            uint32_t flags = msg.flags;
            uint32_t plen = msg.relpath_len;

            rx_handler_begin_batch(rx, batch);
            if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
            (void)hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_FILES);
            if (id == 0 || id > HOP_XFER_FILE_LIMIT) {
                rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            /* 先登记当前条目，确保坏路径也能在 FILE_END 时回 ACK。 */
            free(rx->cur_path);
            rx->cur_path     = NULL;
            hop_xfer_rx_file_state_begin(&rx->file_state, id);
            hop_xfer_rx_file_state_set_kind(&rx->file_state, 0, (flags & HOP_FILE_FLAG_DIR) != 0);
            if (rx->services.reset_file_error) rx->services.reset_file_error(rx->services.context);

            char rel[HOP_PATH_LEN_MAX + 1];
            if (!hop_xfer_relpath_bytes_safe(msg.relpath, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe relative path rejected");
                hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_BADPATH);
                return;
            }
            memcpy(rel, msg.relpath, plen);
            rel[plen] = '\0';


            rx->cur_path = hop_xfer_rx_fs_path_for(rx->dir, rel);
            char *path = rx->cur_path;
            hop_xfer_rx_file_state_set_size(&rx->file_state, size);
            if (!rx->cur_path) { hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK); return; }
            if (rx->services.remove_top_path) rx->services.remove_top_path(rx->services.context, rx->cur_path);
            hop_xfer_rx_entry_reserve_result reservation =
                hop_xfer_rx_entry_reservations_reserve(&rx->entry_reservations, id, size,
                    rx_reserve_entry_budget, &rx->task);
            if (reservation != HOP_RX_ENTRY_RESERVE_NEW &&
                reservation != HOP_RX_ENTRY_RESERVE_ALREADY_HELD) {
                rx_handler_terminate(rx, batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            if (rx->services.status_add_received_file) rx->services.status_add_received_file(rx->services.context,batch,id,rel,size,rx->file_state.is_directory);

            if (rx->file_state.is_directory) {
                if (hop_xfer_rx_fs_mkdir_p(path) != 0) {
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
                    return;
                }
                /* 顶层目录才进剪贴板清单 */
                if (!strchr(rel, '/') && !(rx->services.add_top_path && rx->services.add_top_path(rx->services.context, path)))
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
            } else {
                /* 建父目录；失败时不能把后续文件错误地 ACK 成功。 */
                char *sep = strrchr(path, '/');
                if (sep) {
                    *sep = '\0';
                    int parent_ok = hop_xfer_rx_fs_mkdir_p(path) == 0;
                    *sep = '/';
                    if (!parent_ok) {
                        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
                        return;
                    }
                }

                /* 打开交给写线程 —— 描述符归它独占 */
                if (!rx->services.open_file ||
                    !rx->services.open_file(rx->services.context, path))
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);

                /* 不在这里进剪贴板清单 —— 此刻文件还没写成功。
                 * 写盘失败 / 校验失败的文件绝不能出现在剪贴板里，
                 * 否则用户粘贴到的是一份截断的、看起来完全正常的文件。
                 * 由 FILE_END 的成功分支决定是否登记。 */
            }
            return;
}

static void rx_dispatch_file_end(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_file_end_msg end_msg;
            if (!hop_xfer_decode_file_end(p, len, &end_msg)) return;
            uint32_t id = end_msg.id;
            if (!rx->batch_state.active || !hop_xfer_rx_file_state_matches(&rx->file_state, id)) return;
            uint32_t want = end_msg.file_crc;
            uint8_t status = end_msg.status;

            /* 图片整批走，没有逐文件回执：发送端不会等 */
            if (rx->file_state.is_image) return;

            if (rx->services.status_file_phase) rx->services.status_file_phase(rx->services.context,rx->batch_state.batch,(int)id-1,HOP_XFER_FILE_VERIFYING,0);
            /* Always drain and close before deciding the per-file ACK. */
            if (!rx->file_state.is_directory) if (rx->services.close_file) rx->services.close_file(rx->services.context);

            int writer_error = rx->services.file_error ? rx->services.file_error(rx->services.context) : 1;
            int ack = hop_xfer_rx_file_state_ack(
                &rx->file_state, status, writer_error, want);
            if (rx->file_state.failure < 0) {
                if (writer_error) {
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %s incomplete",
                            rx->cur_path ? rx->cur_path : "?");
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %s",
                            (unsigned)status, rx->cur_path ? rx->cur_path : "?");
                } else if (ack == HOP_FILE_ACK_CRC &&
                           rx->file_state.received != rx->file_state.declared_size) {
                    hop_log(HOP_LOG_ERROR, "xfer: incomplete file rejected (%llu/%llu bytes)",
                            (unsigned long long)rx->file_state.received,
                            (unsigned long long)rx->file_state.declared_size);
                } else if (ack == HOP_FILE_ACK_CRC) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %s (expect %08x got %08x)",
                            rx->cur_path ? rx->cur_path : "?", want, rx->file_state.crc);
                }
            }
            if (ack == HOP_FILE_ACK_OK && rx->cur_path && !rx->file_state.is_directory) {
                /* Only verified top-level files are exposed through clipboard. */
                const char *tail = rx->cur_path + strlen(rx->dir) + 1;
                if (!strchr(tail, '/') && !(rx->services.add_top_path && rx->services.add_top_path(rx->services.context, rx->cur_path)))
                    ack = HOP_FILE_ACK_DISK;
            }


            if (ack != HOP_FILE_ACK_OK && !rx->file_state.is_directory && rx->cur_path) {
                if (hop_xfer_rx_fs_remove_file(rx->cur_path) != 0 && errno != ENOENT)
                    hop_log(HOP_LOG_WARN, "xfer: failed to remove incomplete file %s (%s)",
                            rx->cur_path, strerror(errno));
            }

            if (rx->services.status_file_phase) rx->services.status_file_phase(rx->services.context,rx->batch_state.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            if (rx->services.send_file_ack)
                rx->services.send_file_ack(rx->services.context, rx->batch_state.batch, id, ack);
            return;
}

const hop_xfer_rx_dispatch_handlers hop_xfer_rx_platform_dispatch_handlers = {
    rx_dispatch_file_begin,
    hop_xfer_rx_handle_image_begin,
    hop_xfer_rx_handle_file_chunk,
    rx_dispatch_file_end,
    hop_xfer_rx_dispatch_file_ack_message,
    hop_xfer_rx_handle_abort,
    hop_xfer_rx_handle_done,
    hop_xfer_rx_dispatch_done_ack_message
};
