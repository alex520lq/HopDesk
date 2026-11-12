/* =====================================================================
 *  HopDesk - Windows 数据通道接收端
 *
 *  Receiver-owned production unit: writer queue integration, protocol dispatch,
 *  cancellation policy and staging state. Native data transport belongs to xfer.c.
 * ===================================================================== */
#include "xfer.h"
#include "app.h"
#include "xfer_rx_fs.h"
#include "xfer_writer_queue.h"
#include "../core/xfer_tracking.h"
#include "xfer_receive.h"
#include "../core/hop.h"
#include "../core/xfer_rx_receipt.h"
#include "../core/xfer_rx_lifecycle.h"
#include "../core/xfer_rx_batch_rules.h"
#include "../core/xfer_rx_entry_reservations.h"
#include "../core/xfer_rx_file_state.h"
#include "../core/xfer_rx_dispatch.h"
#include "../core/xfer_rx_platform_dispatch.h"
#include "../core/xfer_rx_services.h"
#include "../core/xfer_rx_context.h"
#include "../core/xfer_rx_message_handlers.h"
#include "../core/xfer_rx_ack_handler.h"
#include "../core/xfer_rx_protocol_adapter.h"
#include "../core/xfer_rx_frame_gate.h"
#include "../core/xfer_rx_batch_state.h"
#include "../core/xfer_rx_image_state.h"
#include "../core/xfer_rx_image_buffer.h"
#include "../core/xfer_rx_queue.h"
#include "../core/update_state.h"
#include "../core/xfer_budget.h"
#include "../core/xfer_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define XFER_RX_ROOT L"HopDesk"

/* ---------------- 写盘队列 ----------------
 *
 * 网络线程只做"收包 + 校验 + 入队"，写盘交给独立线程。
 *
 * 队列有界（RX_Q_SLOTS）。满了网络线程就阻塞在 wq_push，
 * recv 随之停止 → TCP 接收窗口收缩 → 发送端自动减速。
 * 这就是显式背压：内存上界 = RX_Q_SLOTS × chunk，不随传输时长增长。
 * 文件句柄由写线程独占，避免"网络线程关文件、写线程还在写"的竞态。
 */
static volatile LONG s_receive_cancel_requested;
static hop_writer_queue *s_wq;
static xfer_rx_context s_rx;
static int rx_discard_dir(xfer_rx_context *rx);
static void rx_reset(xfer_rx_context *rx);
static void rx_context_configure(void);
static hop_xfer_rx_lifecycle s_rx_lifecycle;
static int s_rx_initialized;
static hop_xfer_rx_protocol_adapter s_rx_protocol_adapter;
static hop_xfer_rx_done_receipt s_done_receipt;

static int wq_reserve_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    return hop_xfer_task_reserve_rx_buffer((hop_xfer_task *)owner, bytes);
}
static void wq_release_task_rx(void *context, void *owner, uint32_t bytes) {
    (void)context;
    (void)hop_xfer_task_release_rx_buffer((hop_xfer_task *)owner, bytes);
}
static int wq_task_cancelled(void *context, void *owner) {
    (void)context;
    return owner && hop_xfer_task_cancel_requested(((hop_xfer_task *)owner)->id);
}
static int wq_watchdog_begin(void *context, const char *label, uint32_t deadline) {
    (void)context;
    return hop_watchdog_begin(label, deadline);
}
static void wq_watchdog_end(void *context, int guard) {
    (void)context;
    hop_watchdog_end(guard);
}
static uint64_t wq_now_ns(void *context) {
    (void)context;
    return rate_now_ns();
}
static void wq_write_time(void *context, void *owner, uint64_t elapsed_ns) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    hop_xfer_task_snapshot snapshot;
    if (rx && owner && hop_xfer_task_get_snapshot(((hop_xfer_task *)owner)->id, &snapshot) &&
        rx->services.status_stage_time)
        rx->services.status_stage_time(rx->services.context, snapshot.transfer_id,
                                       HOP_XFER_TIME_WRITE, elapsed_ns);
}
static void wq_open_error(void *context, const wchar_t *path, unsigned long error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: cannot create %ls (err %lu)", path, error);
}
static void wq_write_error(void *context, uint64_t offset, unsigned long error) {
    (void)context;
    hop_log(HOP_LOG_WARN, "xfer: write failed at %llu (err %lu)",
            (unsigned long long)offset, error);
}

static int wq_start(void) {
    if (s_wq) {
        s_rx.writer = s_wq;
        return 0;
    }
    const hop_writer_queue_callbacks callbacks = {
        HOP_IO_DEADLINE_MS,
        wq_reserve_task_rx, wq_release_task_rx, wq_task_cancelled,
        wq_watchdog_begin, wq_watchdog_end, wq_now_ns,
        wq_write_time, wq_open_error, wq_write_error
    };
    s_wq = hop_writer_queue_create(&s_rx, &callbacks);
    if (s_wq) s_rx.writer = s_wq;
    return s_wq != NULL;
}

static int wq_stop(void) {
    if (!s_wq) {
        s_rx.writer = NULL;
        return 1;
    }
    if (!hop_writer_queue_stop(s_wq)) {
        hop_log(HOP_LOG_WARN, "xfer: writer worker did not exit, retain queue state");
        return 0;
    }
    /* Receiver services may outlive a successful queue stop when the
     * subsequent Task/Reservation audit asks Shutdown to be retried. Detach
     * the context before freeing the queue so a retry cannot drain a stale
     * writer pointer. */
    hop_writer_queue *stopped = s_wq;
    s_wq = NULL;
    s_rx.writer = NULL;
    hop_writer_queue_destroy(stopped);
    return 1;
}

/* The data thread is joined before stop. Keep its Task and staging state
 * alive until the writer has released its reservations and closed the file. */
int xfer_receive_start(void) {
    if (s_rx_initialized) return wq_start();
    hop_xfer_rx_lifecycle_init(&s_rx_lifecycle);
    hop_xfer_rx_done_receipt_clear(&s_done_receipt);
    if (!wq_start()) return 0;
    rx_context_configure();
    s_rx_initialized = 1;
    return 1;
}

int xfer_receive_stop(void) {
    if (!wq_stop()) return 0;
    rx_reset(&s_rx);
    s_rx_initialized = 0;
    return 1;
}

int xfer_receive_owner_clean(void) {
    static const hop_xfer_rx_entry_reservations empty_entry_reservations = {0};
    return s_wq == NULL && s_rx.writer == NULL &&
        !s_rx.task.initialized && !s_rx.task.id &&
        !s_rx.batch_state.active && !s_rx.cur_path &&
        memcmp(&s_rx.entry_reservations, &empty_entry_reservations,
               sizeof(empty_entry_reservations)) == 0 &&
        !s_rx.image_buffer.data && !s_rx.tops && s_rx.ntop == 0;
}

/* ---------------- 接收 ---------------- */


static void rx_context_shutdown_data(void *context) {
    (void)context;
    xfer_data_connection_shutdown();
}

static int rx_context_discard_staging_directory(void *context) {
    return rx_discard_dir((xfer_rx_context *)context);
}

static void rx_context_clear_cancel_request(void *context) {
    (void)context;
    InterlockedExchange(&s_receive_cancel_requested, 0);
}

static int rx_context_create_staging_directory(void *context, const char *session_hex, uint32_t batch) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !session_hex) return 0;
    /* Never overwrite the only record of a staging tree that failed cleanup. */
    if (rx->dir[0] && !rx_discard_dir(rx)) return 0;
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return 0;

    wchar_t sub[80], number[16];
    wcscpy(sub, XFER_RX_ROOT);
    wcscat(sub, L"\\");
    for (int i = 0; i < 32; ++i) {
        size_t at = wcslen(sub);
        sub[at] = (wchar_t)session_hex[i];
        sub[at + 1] = L'\0';
    }
    wcscat(sub, L"\\");
    if (swprintf(number, 16, L"%u", batch) <= 0) return 0;
    wcscat(sub, number);

    size_t sub_length = wcslen(sub);
    if ((size_t)n + sub_length + 1u > HOP_XFER_RX_CONTEXT_PATH_CAPACITY)
        return 0;
    wcscpy(rx->dir, tmp);
    wcscat(rx->dir, sub);
    if (!hop_xfer_rx_fs_ensure_dirs(rx->dir) ||
        !hop_xfer_rx_fs_create_directory_if_missing(rx->dir)) {
        rx->staging_cleanup_pending = 1;
        (void)rx_discard_dir(rx);
        return 0;
    }

    wchar_t full[MAX_PATH];
    DWORD full_length = GetLongPathNameW(rx->dir, full, MAX_PATH);
    if (full_length > 0 && full_length < MAX_PATH)
        wcscpy(rx->dir, full);
    return 1;
}

static void rx_context_send_abort(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    xfer_data_send_abort(batch, reason);
}

static void rx_context_mark_sender_aborted(void *context, uint32_t batch, uint8_t reason) {
    (void)context;
    xfer_data_record_peer_abort(batch, reason);
}

static void rx_context_send_file_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    xfer_data_send_file_ack(batch, id, status);
}

static void rx_context_send_done_ack(void *context, uint32_t batch,
                                    const uint8_t *payload, uint32_t length) {
    (void)context;
    xfer_data_send_done_ack(batch, payload, length);
}

static void rx_context_deliver_ack(void *context, uint32_t batch, uint32_t id, int status) {
    (void)context;
    ack_deliver(batch, id, status);
}



/* 关掉当前文件（排空写队列后）。断线时也要调，但不能清批次状态 ——
 * 发送端会带着同一个 batch 号回来重发剩下的条目。 */
static void rx_close_file(xfer_rx_context *rx) {
    if (rx && rx->services.close_file)
        rx->services.close_file(rx->services.context);
}

static void rx_reset(xfer_rx_context *rx) {
    if (!rx) return;
    if (rx->batch_state.active)
        hop_xfer_rx_terminate_batch(rx, rx->batch_state.batch, HOP_ABORT_DISK, 0);
    else {
        rx_close_file(rx);
        if (rx->staging_cleanup_pending) (void)rx_discard_dir(rx);
        hop_xfer_rx_reset_batch_state(rx);
    }
    /* One final retry during orderly shutdown; retain the path if it still fails. */
    if (rx->staging_cleanup_pending) (void)rx_discard_dir(rx);
}

/* Receiver-owned lifecycle boundary. Only a data reconnect in the SAME control
 * session may retain ACKs/tombstones; a new control session starts cleanly. */
/* One FIFO sender batch can be awaiting DONE_ACK per session. This is only
 * wire idempotency metadata, not another task or reservation owner. */
static hop_xfer_rx_done_receipt s_done_receipt;

static void *rx_context_build_path(void *context, const char *relative, uint32_t length) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !relative || !length || length > HOP_PATH_LEN_MAX) return NULL;
    char *terminated = (char *)malloc((size_t)length + 1u);
    if (!terminated) return NULL;
    memcpy(terminated, relative, length);
    terminated[length] = '\0';
    wchar_t *path = hop_xfer_rx_fs_path_for(rx->dir, terminated);
    free(terminated);
    return path;
}

static int rx_context_prepare_file_path(void *context, const void *native_path, int is_directory) {
    (void)context;
    if (!native_path) return 0;
    const wchar_t *path = (const wchar_t *)native_path;
    size_t length = wcslen(path);
    wchar_t *copy = (wchar_t *)malloc((length + 1u) * sizeof(wchar_t));
    if (!copy) return 0;
    memcpy(copy, path, (length + 1u) * sizeof(wchar_t));

    int result = 0;
    if (is_directory) {
        result = hop_xfer_rx_fs_ensure_dirs(copy) &&
                 hop_xfer_rx_fs_create_directory_if_missing(path);
    } else {
        wchar_t *separator = wcsrchr(copy, L'\\');
        if (!separator) separator = wcsrchr(copy, L'/');
        if (separator) {
            *separator = L'\0';
            result = hop_xfer_rx_fs_ensure_dirs(copy) &&
                     hop_xfer_rx_fs_create_directory_if_missing(copy);
        }
    }
    free(copy);
    return result;
}

static int rx_context_is_top_level_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    const wchar_t *path = (const wchar_t *)native_path;
    if (!rx || !path) return 0;
    size_t root_length = wcslen(rx->dir);
    if (wcsncmp(path, rx->dir, root_length) != 0 ||
        (path[root_length] != L'\\' && path[root_length] != L'/')) return 0;
    const wchar_t *tail = path + root_length + 1u;
    return *tail && !wcschr(tail, L'\\') && !wcschr(tail, L'/');
}

static int rx_context_remove_file(void *context, const void *native_path) {
    (void)context;
    unsigned long error_code = ERROR_SUCCESS;
    if (hop_xfer_rx_fs_remove_file((const wchar_t *)native_path, &error_code)) return 1;
    return error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND;
}

static void rx_context_reset_file_error(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer) return;
    hop_writer_queue_error_reset((hop_writer_queue *)rx->writer);
}
static int rx_context_open_file(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer || !native_path) return 0;
    return hop_writer_queue_enqueue_open((hop_writer_queue *)rx->writer,
                                         (const wchar_t *)native_path);
}
static int rx_context_write_file_chunk(void *context, hop_xfer_task *task,
                                       uint64_t offset, const uint8_t *data, uint32_t length) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer || task != &rx->task) return 0;
    return hop_writer_queue_enqueue_write((hop_writer_queue *)rx->writer,
                                          task, offset, data, length);
}
static void rx_context_close_file(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->writer) return;
    hop_writer_queue *writer = (hop_writer_queue *)rx->writer;
    hop_writer_queue_drain(writer);
    (void)hop_writer_queue_enqueue_close(writer);
    hop_writer_queue_drain(writer);
}
static int rx_context_file_error(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    return rx && rx->writer ? hop_writer_queue_error((const hop_writer_queue *)rx->writer) : 1;
}

static int rx_add_top(xfer_rx_context *rx, const wchar_t *p);
static void rx_remove_top(xfer_rx_context *rx, const wchar_t *p);

static int rx_context_add_top_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    return rx && native_path ? rx_add_top(rx, (const wchar_t *)native_path) : 0;
}
static void rx_context_remove_top_path(void *context, const void *native_path) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && native_path) rx_remove_top(rx, (const wchar_t *)native_path);
}
static void rx_context_cancel_clipboard(void *context, uint32_t batch) {
    (void)context;
    clip_complete_remote_paste(batch, NULL, 0, 0);
}
static void rx_context_finish_clipboard(void *context, uint32_t batch,
        const void *image_data, uint32_t width, uint32_t height, uint32_t stride,
        int image_valid, const void *const *top_paths, int top_count, int allow_publish) {
    (void)context;
    if (allow_publish && image_valid && image_data) {
        uint64_t started = rate_now_ns();
        clip_apply_remote_image(image_data, (int)width, (int)height, (int)stride);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (allow_publish && top_count > 0) {
        uint64_t started = rate_now_ns();
        if (batch & HOP_XFER_PASTE_BATCH_FLAG)
            clip_complete_remote_paste(batch, (const wchar_t *const *)top_paths, top_count, 1);
        else
            clip_apply_remote_paths((const wchar_t *const *)top_paths, top_count);
        status_stage_time(HOP_XFER_DIR_RECEIVE, batch, HOP_XFER_TIME_CLIPBOARD_DISPATCH,
                          rate_now_ns() - started);
    } else if (batch & HOP_XFER_PASTE_BATCH_FLAG) {
        clip_complete_remote_paste(batch, NULL, 0, 0);
    }
}


static uint64_t rx_context_now_ns(void *context) {
    (void)context;
    return rate_now_ns();
}
static int rx_context_status_begin(void *context, uint32_t batch, uint32_t files,
        uint64_t bytes, const char *name, int phase) {
    (void)context;
    return status_begin(batch, HOP_XFER_DIR_RECEIVE, files, bytes, name, phase);
}
static void rx_context_status_end(void *context, uint32_t batch, int phase, const char *detail) {
    (void)context;
    status_end(HOP_XFER_DIR_RECEIVE, batch, phase, detail);
}
static void rx_context_status_add_file(void *context, uint32_t batch, uint32_t id,
        const char *path, uint64_t size, int directory) {
    (void)context;
    status_add_received_file(batch, id, path, size, directory);
}
static void rx_context_status_image(void *context, uint32_t batch, uint64_t size) {
    (void)context;
    status_image(batch, HOP_XFER_DIR_RECEIVE, size);
}
static void rx_context_status_stage(void *context, uint32_t batch, int stage, uint64_t elapsed_ns) {
    (void)context;
    status_stage_time(HOP_XFER_DIR_RECEIVE, batch, stage, elapsed_ns);
}
static void rx_context_status_chunk(void *context, uint32_t batch, uint32_t id,
        uint64_t offset, uint32_t length, uint64_t crc_ns, uint64_t receive_ns) {
    (void)context;
    status_chunk_timed(HOP_XFER_DIR_RECEIVE, batch, id, offset, length, 0, crc_ns, 0, receive_ns);
}
static void rx_context_status_file_phase(void *context, uint32_t batch, int index,
        int phase, uint32_t attempts) {
    (void)context;
    status_file_phase(HOP_XFER_DIR_RECEIVE, batch, index, phase, attempts);
}
static void rx_context_status_set_files_total(void *context, uint32_t total) {
    (void)context;
    status_receive_set_total(total);
}

static void rx_context_status_reset(void *context) {
    (void)context;
    status_receive_reset();
}

static void rx_context_configure(void) {
    s_rx.lifecycle = &s_rx_lifecycle;
    s_rx.done_receipt = &s_done_receipt;
    s_rx.writer = s_wq;
    s_rx.services.context = &s_rx;
    s_rx.services.shutdown_data_channel = rx_context_shutdown_data;
    s_rx.services.mark_sender_aborted = rx_context_mark_sender_aborted;
    s_rx.services.send_abort = rx_context_send_abort;
    s_rx.services.clear_cancel_request = rx_context_clear_cancel_request;
    s_rx.services.create_staging_directory = rx_context_create_staging_directory;
    s_rx.services.discard_staging_directory = rx_context_discard_staging_directory;
    s_rx.services.send_file_ack = rx_context_send_file_ack;
    s_rx.services.send_done_ack = rx_context_send_done_ack;
    s_rx.services.deliver_ack = rx_context_deliver_ack;
    s_rx.services.build_path = rx_context_build_path;
    s_rx.services.prepare_file_path = rx_context_prepare_file_path;
    s_rx.services.is_top_level_path = rx_context_is_top_level_path;
    s_rx.services.remove_file = rx_context_remove_file;
    s_rx.services.reset_file_error = rx_context_reset_file_error;
    s_rx.services.open_file = rx_context_open_file;
    s_rx.services.write_file_chunk = rx_context_write_file_chunk;
    s_rx.services.close_file = rx_context_close_file;
    s_rx.services.file_error = rx_context_file_error;
    s_rx.services.add_top_path = rx_context_add_top_path;
    s_rx.services.remove_top_path = rx_context_remove_top_path;
    s_rx.services.finish_clipboard = rx_context_finish_clipboard;
    s_rx.services.cancel_clipboard = rx_context_cancel_clipboard;
    s_rx.services.now_ns = rx_context_now_ns;
    s_rx.services.status_begin = rx_context_status_begin;
    s_rx.services.status_end = rx_context_status_end;
    s_rx.services.status_add_received_file = rx_context_status_add_file;
    s_rx.services.status_image = rx_context_status_image;
    s_rx.services.status_stage_time = rx_context_status_stage;
    s_rx.services.status_chunk_received = rx_context_status_chunk;
    s_rx.services.status_file_phase = rx_context_status_file_phase;
    s_rx.services.status_set_files_total = rx_context_status_set_files_total;
    s_rx.services.status_reset = rx_context_status_reset;
    (void)hop_xfer_rx_protocol_adapter_init(&s_rx_protocol_adapter,
        &hop_xfer_rx_platform_dispatch_handlers, &s_rx);
}
static int rx_discard_dir(xfer_rx_context *rx);
static void rx_bind_session(const hop_session_id *session) {
    hop_xfer_rx_bind_session(&s_rx, session);
}

/* The control session may vanish permanently before a new data connection is
 * established. Poll only on the receiver thread; keep a same-session data
 * reconnect resumable, but promptly retire an invalidated control session. */
static void rx_expire_stale_session(void) {
    if (hop_session_id_valid(hop_xfer_rx_lifecycle_session(&s_rx_lifecycle)) && !hop_session_matches(hop_xfer_rx_lifecycle_session(&s_rx_lifecycle))) {
        hop_session_id empty = {{0}};
        rx_bind_session(&empty);
    }
}

/* 递归删掉本批临时目录。
 * 只在接收批次被 ABORT 时调用；普通部分完成仍保留已送达的文件，
 * 成功批次的文件就是产物本身，都不能删。 */
static int rx_discard_dir(xfer_rx_context *rx) {
    if (!rx) return 0;
    if (!rx->dir[0]) { rx->staging_cleanup_pending = 0; return 1; }
    /* Staging paths are created as <temp>\\HopDesk\\<session>\\<batch>. */
    int result = hop_xfer_rx_fs_remove_tree(rx->dir, L"\\HopDesk\\");
    if (result == 0) {
        rx->dir[0] = L'\0';
        rx->staging_cleanup_pending = 0;
        return 1;
    }
    if (result == -2)
        hop_log(HOP_LOG_ERROR, "xfer: refuse to delete receiver staging directory");
    else
        hop_log(HOP_LOG_WARN, "xfer: failed to remove receiver staging directory (result %d); retaining path for retry", result);
    rx->staging_cleanup_pending = 1;
    return 0;
}

/* Create a directory, accepting ERROR_ALREADY_EXISTS only when the path
 * is actually a directory (a regular file at a parent path is an error). */


/* Create parent components without attempting to create a drive/UNC root. */


static int rx_add_top(xfer_rx_context *rx, const wchar_t *p) {
    if (!rx || !p) return 0;
    for (int i = 0; i < rx->ntop; ++i)
        if (wcscmp(rx->tops[i], p) == 0) return 1;
    wchar_t **nv = (wchar_t **)realloc(rx->tops, (size_t)(rx->ntop + 1) * sizeof(wchar_t *));
    if (!nv) return 0;
    rx->tops = nv;
    rx->tops[rx->ntop] = _wcsdup(p);
    if (!rx->tops[rx->ntop]) return 0;
    rx->ntop++;
    return 1;
}

/* Remove a previously successful top-level file before retrying it.
 * A retry opens with CREATE_ALWAYS; if it later fails, stale clipboard paths
 * must not continue advertising the now-incomplete file. */
static void rx_remove_top(xfer_rx_context *rx, const wchar_t *p) {
    if (!rx || !p) return;
    for (int i = 0; i < rx->ntop; ++i) {
        if (wcscmp(rx->tops[i], p) != 0) continue;
        free(rx->tops[i]);
        if (i + 1 < rx->ntop)
            memmove(&rx->tops[i], &rx->tops[i + 1],
                    (size_t)(rx->ntop - i - 1) * sizeof(*rx->tops));
        rx->ntop--;
        return;
    }
}


/* 把 UTF-8 相对路径拼到本批目录下，返回 malloc 的宽路径 */


/* Receiver-thread only. Wire notification has no status side effects. Close the
 * writer before freeing paths/buffers or deleting staging files. Terminal state
 * is idempotent; repeated/stale ABORTs cannot end an unrelated active batch. */



hop_rx_frame_action xfer_receive_process_frame(const hop_session_id *session,
    const char *token, int authenticated, uint8_t type,
    const uint8_t *payload, uint32_t length) {
    hop_rx_frame_gate_input input;
    input.expected_session = session ? *session : (hop_session_id){0};
    input.active_batch = s_rx.batch_state.active ? s_rx.batch_state.batch : 0;
    hop_rx_frame_gate_result gate =
        hop_xfer_rx_gate_frame(&input, token, type, payload, length);
    if (gate.action == HOP_RX_FRAME_DISPATCH) {
        if (!authenticated) return HOP_RX_FRAME_REJECT_PEER;
        static hop_xfer_rx_protocol_adapter adapter;
        hop_xfer_rx_protocol_adapter_init(&adapter,
            &hop_xfer_rx_platform_dispatch_handlers, &s_rx);
        (void)hop_xfer_rx_protocol_adapter_dispatch(&adapter, type,
                                                    gate.body, gate.body_length);
    }
    return gate.action;
}

/* 用户在本端取消接收：通知对端、删掉半成品、立墓碑。
 * 由 listen 线程执行 —— s_rx 只归它所有，别的线程直接碰就是竞态。 */
static void rx_cancel_local(void) {
    if (!s_rx.batch_state.active) return;
    uint32_t batch = s_rx.batch_state.batch;
    hop_xfer_rx_terminate_batch(&s_rx,batch,HOP_ABORT_CANCEL,1);
    hop_log(HOP_LOG_INFO, "xfer: batch %u cancelled locally", batch);
}
void xfer_receive_cancel_request(void) {
    InterlockedExchange(&s_receive_cancel_requested, 1);
}

void xfer_receive_bind_session(const hop_session_id *session) {
    if (session) rx_bind_session(session);
}

void xfer_receive_disconnected(void) { rx_close_file(&s_rx); }

void xfer_receive_poll_cancel(int connected) {
    rx_expire_stale_session();
    int requested = InterlockedExchange(&s_receive_cancel_requested, 0);
    if (requested || (s_rx.batch_state.active && hop_xfer_task_cancel_requested(s_rx.task.id))) rx_cancel_local();
    if (s_rx.batch_state.active && hop_xfer_task_rx_disconnect_expired(&s_rx.task,connected,hop_monotonic_ms(),HOP_XFER_RX_DISCONNECT_BUDGET_MS)) {
        hop_log(HOP_LOG_WARN,"xfer: receive batch %u expired while disconnected",s_rx.batch_state.batch);
        hop_xfer_rx_terminate_batch(&s_rx,s_rx.batch_state.batch,HOP_ABORT_TIMEOUT,0);
    }
}
