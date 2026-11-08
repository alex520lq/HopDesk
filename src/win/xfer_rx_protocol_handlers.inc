static void rx_dispatch_file_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_file_begin_msg msg;
            if (!hop_xfer_decode_file_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
                if (rx->batch_state.active) rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                EnterCriticalSection(&s_x.lock);
                SOCKET active = s_x.client;
                LeaveCriticalSection(&s_x.lock);
                if (active != INVALID_SOCKET) shutdown(active, SD_BOTH);
                return;
            }
            uint32_t batch = msg.batch;
            uint32_t id = msg.id;
            uint64_t size = msg.size;
            uint32_t flags = msg.flags;
            uint32_t plen = msg.relpath_len;
            const uint8_t *rel = msg.relpath;

            rx_begin_batch(batch);
            if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
            (void)hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_FILES);
            if (id == 0 || id > HOP_XFER_FILE_LIMIT) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            /* 先把"当前条目"登记好，哪怕它是坏的：FILE_END 要靠 cur_id 回 ACK，
             * 漏回一次，发送端就要白等 10 秒超时再重试。 */
            free(rx->cur_path);
            rx->cur_path     = NULL;
            hop_xfer_rx_file_state_begin(&rx->file_state, id);
            hop_xfer_rx_file_state_set_kind(&rx->file_state, 0, (flags & HOP_FILE_FLAG_DIR) != 0);
            hop_writer_queue_error_reset(s_wq);

            char *relz = (char *)malloc((size_t)plen + 1);
            if (!relz) { hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK); return; }
            memcpy(relz, rel, plen);
            relz[plen] = '\0';

            if (!hop_xfer_relpath_bytes_safe(rel, plen)) {
                hop_log(HOP_LOG_WARN, "xfer: unsafe rel path rejected: %s", relz);
                free(relz);
                hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_BADPATH);
                return;
            }
            status_add_received_file(batch,id,relz,size,rx->file_state.is_directory);
            wchar_t *path = hop_xfer_rx_fs_path_for(rx->dir, relz);
            free(relz);
            if (!path) { hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_BADPATH); return; }

            rx->cur_path = path;
            rx_remove_top(rx->cur_path);
            hop_xfer_rx_file_state_set_size(&rx->file_state, size);
            hop_xfer_rx_entry_reserve_result reservation =
                hop_xfer_rx_entry_reservations_reserve(&rx->entry_reservations, id, size,
                    rx_reserve_entry_budget, &rx->task);
            if (reservation != HOP_RX_ENTRY_RESERVE_NEW &&
                reservation != HOP_RX_ENTRY_RESERVE_ALREADY_HELD) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            if (rx->file_state.is_directory) {
                if (!hop_xfer_rx_fs_ensure_dirs(path) || !hop_xfer_rx_fs_create_directory_if_missing(path)) {
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
                    return;
                }
                /* 顶层目录才进剪贴板清单 */
                if (!wcschr(path + wcslen(rx->dir) + 1, L'\\') &&
                    !rx_add_top(path))
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
            } else {
                /* 建父目录；失败时不能把后续文件错误地 ACK 成功。 */
                wchar_t *sep = wcsrchr(path, L'\\');
                if (sep) {
                    wchar_t save = *sep;
                    *sep = L'\0';
                    int parent_ok = hop_xfer_rx_fs_ensure_dirs(path) && hop_xfer_rx_fs_create_directory_if_missing(path);
                    *sep = save;
                    if (!parent_ok) {
                        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
                        return;
                    }
                }
                /* 打开交给写线程 —— 句柄归它独占 */
                rx_task t;
                memset(&t, 0, sizeof(t));
                t.kind = RX_OPEN;
                wcsncpy(t.path, path, MAX_PATH - 1);
                if (!wq_push(&t)) hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);

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
                if (rx->batch_state.active) rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint32_t batch = msg.batch;
            rx_begin_batch(batch);
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
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }

            size_t expected = (size_t)stride * (size_t)h;
            if (!rx->file_state.is_image) {
                if (!hop_xfer_task_reserve_image(&rx->task,(uint64_t)expected)) {
                    rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
            } else if (rx->image_state.expected != expected) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            hop_xfer_rx_file_state_begin(&rx->file_state, msg.id);
            hop_xfer_rx_file_state_set_kind(&rx->file_state, 1, 0);
            if (!hop_xfer_rx_image_state_begin(&rx->image_state,
                    (uint32_t)w, (uint32_t)h, (uint32_t)stride, (uint64_t)expected)) {
                rx_terminate(batch,HOP_ABORT_TOO_LARGE,1);
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
                rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint64_t off = chunk.offset;
            uint32_t n = chunk.length;
            uint32_t crc = chunk.crc;
            if (chunk.id != rx->file_state.id) {
                rx_terminate(rx->batch_state.batch,HOP_ABORT_CRC,1);
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
                    rx_terminate(rx->batch_state.batch,HOP_ABORT_CRC,1);
                } else {
                    hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_CRC);
                }
                return;
            }

            if (rx->file_state.is_image) {
                if (hop_xfer_rx_image_state_check_chunk(
                        &rx->image_state, off, n) != HOP_RX_IMAGE_CHUNK_ACCEPT) {
                    hop_log(HOP_LOG_ERROR, "xfer: image chunk gap/overlap or length overflow");
                    rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                size_t need = (size_t)(off + n);
                /* IMAGE_BEGIN already reserved img_expected bytes in the
                 * task ledger. Allocate that exact amount once: geometric growth
                 * would make the real capacity exceed the reservation and may
                 * transiently keep both old and new buffers alive during realloc. */
                if (!hop_xfer_rx_image_buffer_ensure(&rx->image_buffer,
                        (uint64_t)rx->image_state.expected)) {
                    rx_terminate(rx->batch_state.batch,HOP_ABORT_DISK,1);
                    return;
                }
                hop_xfer_rx_image_state_set_capacity(
                    &rx->image_state, rx->image_buffer.capacity);
                if (need > rx->image_state.capacity) {
                    hop_log(HOP_LOG_ERROR, "xfer: image buffer capacity exceeded");
                    rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                    return;
                }
                memcpy(rx->image_buffer.data + off, data, n);
                if (!hop_xfer_rx_image_state_record_chunk(&rx->image_state, off, n)) {
                    rx_terminate(rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
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
            if (!wq_push_data(&rx->task, off, data, n)) {
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
            if (!rx->file_state.is_directory) rx_close_file();

            int writer_error = wq_error_get();
            int ack = hop_xfer_rx_file_state_ack(
                &rx->file_state, status, writer_error, want);
            if (rx->file_state.failure < 0) {
                if (writer_error) {
                    hop_log(HOP_LOG_ERROR, "xfer: disk error, %ls incomplete",
                            rx->cur_path ? rx->cur_path : L"?");
                } else if (status != HOP_FILE_OK) {
                    hop_log(HOP_LOG_ERROR, "xfer: peer reported status %u for %ls",
                            (unsigned)status, rx->cur_path ? rx->cur_path : L"?");
                } else if (ack == HOP_FILE_ACK_CRC &&
                           rx->file_state.received != rx->file_state.declared_size) {
                    hop_log(HOP_LOG_ERROR, "xfer: incomplete file rejected (%llu/%llu bytes)",
                            (unsigned long long)rx->file_state.received,
                            (unsigned long long)rx->file_state.declared_size);
                } else if (ack == HOP_FILE_ACK_CRC) {
                    hop_log(HOP_LOG_ERROR,
                            "xfer: FILE CRC MISMATCH %ls (expect %08x got %08x)",
                            rx->cur_path ? rx->cur_path : L"?", want, rx->file_state.crc);
                }
            }
            if (ack == HOP_FILE_ACK_OK && rx->cur_path && !rx->file_state.is_directory) {
                /* Only verified top-level files are exposed through clipboard. */
                const wchar_t *tail = rx->cur_path + wcslen(rx->dir) + 1;
                if (!wcschr(tail, L'\\') && !rx_add_top(rx->cur_path))
                    ack = HOP_FILE_ACK_DISK;
            }


            if (ack != HOP_FILE_ACK_OK && !rx->file_state.is_directory && rx->cur_path) {
                DWORD error = ERROR_SUCCESS;
                if (!hop_xfer_rx_fs_remove_file(rx->cur_path, &error) &&
                    error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    hop_log(HOP_LOG_WARN, "xfer: failed to remove incomplete file %ls (err %lu)",
                            rx->cur_path, error);
            }

            status_file_phase(HOP_XFER_DIR_RECEIVE,rx->batch_state.batch,(int)id-1,ack==HOP_FILE_ACK_OK?HOP_XFER_FILE_COMPLETED:HOP_XFER_FILE_FAILED,0);

            /* 回执是发送端判断"这个文件成没成"的唯一依据。
             * 以前这里发的是批量级 ABORT，等于一个文件坏了就把整批判死。 */
            send_ack(s_x.client, rx->batch_state.batch, id, ack);
            return;
}

static void rx_dispatch_file_ack(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_file_ack_msg ack;
            if (hop_xfer_decode_file_ack(p, len, &ack) && ack.id >= 1 && ack.id <= HOP_XFER_FILE_LIMIT)
                ack_deliver(ack.batch, ack.id, ack.status);
            return;
}

static void rx_dispatch_abort_message(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
hop_xfer_abort_msg abort_msg;
            if (!hop_xfer_decode_abort(p, len, &abort_msg)) return;
            uint32_t batch = abort_msg.batch;
            uint8_t reason = abort_msg.reason;

            if (abort_msg.target == HOP_ABORT_TO_SENDER) {
                EnterCriticalSection(&s_x.lock);
                if (s_x.cur_batch == batch) { s_x.abort_batch = batch; s_x.abort_reason = reason; }
                LeaveCriticalSection(&s_x.lock);
                SetEvent(s_ack.ev);
            }

            hop_log(HOP_LOG_ERROR, "xfer: peer aborted batch %u (reason %u)",
                    batch, reason);

            /* 对端不要这一批了：关文件、删掉已落盘的半成品、立墓碑。
             * 墓碑必须立 —— 否则对端再发一帧 FILE_BEGIN 就把目录又建回来了。 */
            if (abort_msg.target == HOP_ABORT_TO_RECEIVER) rx_terminate(batch,reason,0);
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
                hop_xfer_rx_done_receipt_find(&s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle), done.batch);
            if (receipt) {
                if (!hop_xfer_rx_done_receipt_matches_payload(receipt, &done)) return;
                result=receipt->status; /* repeat ACK without repeating clipboard effects */
            } else {
                result=rx_finish_batch(done.batch,done.ok,done.total);
                if (result<0) return;
                hop_xfer_rx_done_receipt_record(&s_done_receipt, hop_xfer_rx_lifecycle_session(&s_rx_lifecycle),
                                                &done, (uint8_t)result);
            }
            uint8_t reply[5]; hop_put_be32(reply,done.batch); reply[4]=(uint8_t)result;
            EnterCriticalSection(&s_x.lock);
            SOCKET fd=s_x.client;
            LeaveCriticalSection(&s_x.lock);
            if (fd != INVALID_SOCKET) (void)send_small(fd,HOP_MSG_XFER_DONE_ACK,reply,sizeof(reply),done.batch);
            return;
}

static void rx_dispatch_done_ack(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
if (len==5 && hop_get_be32(p)!=0 && p[4]<=HOP_DONE_ACK_FAILED)
                ack_deliver(hop_get_be32(p),HOP_DONE_ACK_SLOT,p[4]);
            return;
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
