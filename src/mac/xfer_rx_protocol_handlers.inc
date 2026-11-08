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
            status_add_received_file(batch,id,rel,size,rx->file_state.is_directory);

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

static void rx_dispatch_image_begin(void *context, const uint8_t *p, uint32_t len) {
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
            (void)hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_IMAGE);

            int w = (int)msg.width;
            int h = (int)msg.height;
            int stride = (int)msg.stride;

            /* 对端声称的尺寸必须自己验：不然 w=h=100000 就能把内存吃光。
             * 这正是"避免内存不断增长"的边界。 */
            if (!hop_xfer_image_valid(w, h, stride)) {
                hop_log(HOP_LOG_ERROR,
                        "xfer: image %dx%d stride %d rejected (over cap)", w, h, stride);
                rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
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
            status_image(batch,HOP_XFER_DIR_RECEIVE,rx->image_state.expected);
            return;
}

static void rx_dispatch_file_chunk(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
if (!rx->batch_state.active) return; /* late data after termination */
            uint64_t receive_started_ns=rate_now_ns();
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
            uint64_t crc_started_ns=rate_now_ns();
            int crc_ok=(hop_crc32(0, data, n) == crc);
            uint64_t crc_elapsed_ns=rate_now_ns()-crc_started_ns;
            if (!crc_ok) {
                status_stage_time(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,HOP_XFER_TIME_CRC,crc_elapsed_ns);
                status_stage_time(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,HOP_XFER_TIME_RECEIVE,
                                  rate_now_ns()-receive_started_ns);
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
                uint64_t receive_elapsed_ns=rate_now_ns()-receive_started_ns;
                status_chunk_timed(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,rx->file_state.id,off,n,
                                   0,crc_elapsed_ns,0,receive_elapsed_ns);
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
            uint64_t receive_elapsed_ns=rate_now_ns()-receive_started_ns;
            status_chunk_timed(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,rx->file_state.id,off,n,
                               0,crc_elapsed_ns,0,receive_elapsed_ns);
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

            status_file_phase(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,(int)id-1,HOP_XFER_FILE_VERIFYING,0);
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

            status_file_phase(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            if (rx->services.send_file_ack)
                rx->services.send_file_ack(rx->services.context, rx->batch_state.batch, id, ack);
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

static void rx_dispatch_file_ack(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    (void)hop_xfer_rx_handle_file_ack(p, len, rx_ack_deliver_file, rx);
}

static void rx_dispatch_abort_message(void *context, const uint8_t *p, uint32_t len) {
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

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。 */
            if (abort_msg.target == HOP_ABORT_TO_RECEIVER) rx_handler_terminate(rx, batch,reason,0);
            return;
}

static void rx_dispatch_done(void *context, const uint8_t *p, uint32_t len) {
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

static void rx_dispatch_done_ack(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    (void)hop_xfer_rx_handle_done_ack(p, len, rx_ack_deliver_done, rx);
}

static const hop_xfer_rx_dispatch_handlers s_rx_dispatch_handlers = {
    rx_dispatch_file_begin,
    rx_dispatch_image_begin,
    rx_dispatch_file_chunk,
    rx_dispatch_file_end,
    rx_dispatch_file_ack,
    rx_dispatch_abort_message,
    rx_dispatch_done,
    rx_dispatch_done_ack
};
