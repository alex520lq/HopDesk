#include "hop.h"
#include "xfer_rx_context.h"
#include "xfer_rx_batch_rules.h"
#include "xfer_rx_message_handlers.h"
#include "xfer_rx_ack_handler.h"
#include "xfer_tracking.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rx_reserve_entry_budget(void *context, uint64_t bytes) {
    return hop_xfer_task_reserve_entry((hop_xfer_task *)context, bytes);
}

static uint64_t rx_now_ns(xfer_rx_context *rx) {
    return rx && rx->services.now_ns
        ? rx->services.now_ns(rx->services.context) : 0;
}
static void rx_abort_send(void *context, uint32_t batch, uint8_t reason) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.send_abort)
        rx->services.send_abort(rx->services.context, batch, reason);
}
static void rx_abort_cancel_clipboard(void *context, uint32_t batch) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.cancel_clipboard)
        rx->services.cancel_clipboard(rx->services.context, batch);
}
static void rx_abort_close_file(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.close_file)
        rx->services.close_file(rx->services.context);
}
static void rx_abort_discard_staging(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.discard_staging_directory)
        rx->services.discard_staging_directory(rx->services.context);
}
static void rx_abort_mark_dead(void *context, uint32_t batch, uint8_t reason) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->lifecycle)
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, reason);
}
static void rx_abort_finish_task(void *context, int cancelled) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx) (void)hop_xfer_task_finish(&rx->task,
        cancelled ? HOP_XFER_TASK_CANCELLED : HOP_XFER_TASK_FAILED);
}
static void rx_abort_clear_state(void *context) {
    hop_xfer_rx_reset_batch_state(context);
}
static void rx_abort_status_end(void *context, uint32_t batch, int cancelled) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (rx && rx->services.status_end)
        rx->services.status_end(rx->services.context, batch,
            cancelled ? HOP_XFER_CANCELLED : HOP_XFER_FAILED,
            cancelled ? "Receive cancelled" : "Receive aborted");
}

void hop_xfer_rx_terminate_batch(void *context, uint32_t batch,
                                 uint8_t reason, int notify_peer) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    hop_xfer_rx_batch_abort_action action = hop_xfer_rx_batch_abort_decide(
        batch, rx->batch_state.active, rx->batch_state.batch, notify_peer);
    const hop_xfer_rx_abort_ops ops = {
        rx, rx_abort_send, rx_abort_cancel_clipboard, rx_abort_close_file,
        rx_abort_discard_staging, rx_abort_mark_dead, rx_abort_finish_task,
        rx_abort_clear_state, rx_abort_status_end
    };
    (void)hop_xfer_rx_abort_execute(action, batch, reason, notify_peer,
        (batch & HOP_XFER_PASTE_BATCH_FLAG) != 0, &ops);
}

void hop_xfer_rx_reset_batch_state(void *context) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    free(rx->cur_path);
    rx->cur_path = NULL;
    hop_xfer_rx_image_buffer_dispose(&rx->image_buffer);
    hop_xfer_rx_image_state_reset(&rx->image_state);
    for (int i = 0; i < rx->ntop; ++i) free(rx->tops[i]);
    free(rx->tops);
    rx->tops = NULL;
    rx->ntop = 0;
    hop_xfer_rx_batch_state_reset(&rx->batch_state);
    hop_xfer_rx_file_state_reset(&rx->file_state);
    hop_xfer_rx_file_state_set_kind(&rx->file_state, 0, 0);
    hop_xfer_rx_entry_reservations_reset(&rx->entry_reservations);
    hop_xfer_task_dispose(&rx->task);
    rx->dir[0] = 0;
}

static void rx_handler_terminate(xfer_rx_context *rx, uint32_t batch,
                                 uint8_t reason, int notify_peer) {
    hop_xfer_rx_terminate_batch(rx, batch, reason, notify_peer);
}
static void rx_handler_begin_batch(xfer_rx_context *rx, uint32_t batch) {
    hop_xfer_rx_begin_batch(rx, batch);
}
static int rx_handler_finish_batch(xfer_rx_context *rx, uint32_t batch,
                                   uint32_t ok, uint32_t total) {
    return hop_xfer_rx_finish_batch(rx, batch, ok, total);
}

void hop_xfer_rx_bind_session(void *context, const hop_session_id *session) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !rx->lifecycle || !session) return;

    const hop_session_id *current = hop_xfer_rx_lifecycle_session(rx->lifecycle);
    if (hop_session_id_equal(current, session)) return;

    hop_xfer_rx_done_receipt_clear(rx->done_receipt);
    int was_active = rx->batch_state.active;
    int terminated = 0;

    if (was_active && !hop_xfer_task_begin_completion(&rx->task) &&
        hop_xfer_task_cancel_requested(rx->task.id)) {
        hop_xfer_rx_terminate_batch(rx, rx->batch_state.batch, HOP_ABORT_CANCEL, 0);
        terminated = !rx->batch_state.active;
    }

    if (!terminated) {
        if (was_active && rx->batch_state.active && rx->services.status_end)
            rx->services.status_end(rx->services.context, rx->batch_state.batch,
                HOP_XFER_FAILED, "Receive interrupted");
        if (rx->services.close_file)
            rx->services.close_file(rx->services.context);
        if (was_active && rx->batch_state.active) {
            if (rx->services.discard_staging_directory)
                rx->services.discard_staging_directory(rx->services.context);
            if ((rx->batch_state.batch & HOP_XFER_PASTE_BATCH_FLAG) &&
                rx->services.cancel_clipboard)
                rx->services.cancel_clipboard(rx->services.context, rx->batch_state.batch);
        }
        hop_xfer_rx_reset_batch_state(rx);
    }

    if (rx->services.status_reset)
        rx->services.status_reset(rx->services.context);
    (void)hop_xfer_rx_lifecycle_bind_session(rx->lifecycle, session);
}

void hop_xfer_rx_begin_batch(void *context, uint32_t batch) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx || !batch || !rx->lifecycle || !rx->done_receipt) return;

    uint8_t dead_reason = 0;
    const hop_session_id *session = hop_xfer_rx_lifecycle_session(rx->lifecycle);
    int committed = hop_xfer_rx_done_receipt_find(
        rx->done_receipt, session, batch) != NULL;
    (void)hop_xfer_rx_lifecycle_dead_reason(rx->lifecycle, batch, &dead_reason);

    hop_xfer_rx_batch_begin_action action = hop_xfer_rx_batch_begin_decide(
        batch, committed, dead_reason, rx->batch_state.active,
        rx->batch_state.batch, &dead_reason);
    switch (action) {
        case HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED:
        case HOP_RX_BATCH_BEGIN_RESUME:
        case HOP_RX_BATCH_BEGIN_INVALID:
            return;
        case HOP_RX_BATCH_BEGIN_REJECT_DEAD:
            hop_xfer_rx_terminate_batch(rx, batch, dead_reason, 1);
            return;
        case HOP_RX_BATCH_BEGIN_START:
            break;
        default:
            return;
    }

    if (rx->services.clear_cancel_request)
        rx->services.clear_cancel_request(rx->services.context);
    if (rx->batch_state.active) {
        hop_xfer_rx_terminate_batch(rx, rx->batch_state.batch, HOP_ABORT_DISK, 0);
    } else {
        if (rx->services.close_file)
            rx->services.close_file(rx->services.context);
        hop_xfer_rx_reset_batch_state(rx);
    }

    if (!hop_xfer_task_init_classified(&rx->task, 0,
            (batch & HOP_XFER_PASTE_BATCH_FLAG) ? batch : 0,
            batch, HOP_XFER_DIR_RECEIVE, HOP_TASK_KIND_UNSPECIFIED)) {
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, HOP_ABORT_DISK);
        if (rx->services.send_abort)
            rx->services.send_abort(rx->services.context, batch, HOP_ABORT_DISK);
        return;
    }
    if (session)
        (void)hop_xfer_task_bind_session(&rx->task, session->bytes);
    if (!hop_xfer_task_reserve_job(&rx->task)) {
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, HOP_ABORT_TOO_LARGE);
        hop_xfer_task_finish(&rx->task, HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&rx->task);
        if (rx->services.send_abort)
            rx->services.send_abort(rx->services.context, batch, HOP_ABORT_TOO_LARGE);
        return;
    }
    if (!hop_xfer_task_set_state(&rx->task, HOP_XFER_TASK_ACTIVE)) {
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, HOP_ABORT_DISK);
        hop_xfer_task_finish(&rx->task, HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&rx->task);
        if (rx->services.send_abort)
            rx->services.send_abort(rx->services.context, batch, HOP_ABORT_DISK);
        return;
    }
    if (!hop_xfer_rx_batch_state_begin(&rx->batch_state, batch)) {
        hop_log(HOP_LOG_ERROR, "xfer: receiver batch state rejected begin for %u", batch);
        hop_xfer_task_finish(&rx->task, HOP_XFER_TASK_FAILED);
        hop_xfer_task_dispose(&rx->task);
        hop_xfer_rx_lifecycle_mark_dead(rx->lifecycle, batch, HOP_ABORT_DISK);
        if (rx->services.send_abort)
            rx->services.send_abort(rx->services.context, batch, HOP_ABORT_DISK);
        return;
    }

    if (rx->services.status_begin)
        rx->services.status_begin(rx->services.context, batch, 0, 0,
            "Incoming files", HOP_XFER_RECEIVING);

    char session_hex[33] = {0};
    if (session) hop_session_hex(session, session_hex);
    if (!rx->services.create_staging_directory ||
        !rx->services.create_staging_directory(
            rx->services.context, session_hex, batch)) {
        hop_xfer_rx_terminate_batch(rx, batch, HOP_ABORT_DISK, 1);
    }
}

int hop_xfer_rx_finish_batch(void *context, uint32_t batch, uint32_t ok, uint32_t total) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return -1;

    int active = hop_xfer_rx_batch_is_active(
        batch, rx->batch_state.active, rx->batch_state.batch);
    if (!active) {
        hop_log(HOP_LOG_WARN, "xfer: stale DONE for batch %u ignored", batch);
        return -1;
    }

    /* Completion and cancellation compete through the Task Manager. No
     * clipboard publication or writer teardown may happen before this claim. */
    int claimed = hop_xfer_task_begin_completion(&rx->task);
    hop_xfer_rx_batch_finish_action action = hop_xfer_rx_batch_finish_decide(
        batch, rx->batch_state.active, rx->batch_state.batch, claimed,
        ok, total, rx->file_state.is_image, 0);
    if (action == HOP_RX_BATCH_FINISH_CANCELLED) {
        hop_xfer_rx_terminate_batch(rx, batch, HOP_ABORT_CANCEL, 1);
        return -1;
    }
    if (action == HOP_RX_BATCH_FINISH_STALE) return -1;

    if (rx->services.close_file)
        rx->services.close_file(rx->services.context);

    int image_valid = rx->image_buffer.data &&
        rx->image_state.received == rx->image_state.expected;
    if (ok == total && rx->file_state.is_image && !image_valid)
        hop_log(HOP_LOG_ERROR, "xfer: incomplete image rejected (%llu/%llu bytes)",
            (unsigned long long)rx->image_state.received,
            (unsigned long long)rx->image_state.expected);

    if (rx->services.finish_clipboard)
        rx->services.finish_clipboard(rx->services.context, batch,
            rx->image_buffer.data, rx->image_state.width,
            rx->image_state.height, rx->image_state.stride, image_valid,
            (const void *const *)rx->tops, rx->ntop, ok == total);

    if (rx->services.status_set_files_total)
        rx->services.status_set_files_total(rx->services.context, total);
    if (rx->file_state.is_image && ok == total && image_valid &&
        rx->services.status_file_phase)
        rx->services.status_file_phase(rx->services.context, batch, 0,
            HOP_XFER_FILE_COMPLETED, 1);

    action = hop_xfer_rx_batch_finish_decide(
        batch, rx->batch_state.active, rx->batch_state.batch, 1,
        ok, total, rx->file_state.is_image, image_valid);
    int complete = action == HOP_RX_BATCH_FINISH_COMPLETE;
    int image_failed = action == HOP_RX_BATCH_FINISH_IMAGE_FAILED;
    hop_xfer_task_finish(&rx->task, complete ? HOP_XFER_TASK_COMPLETED :
        (image_failed ? HOP_XFER_TASK_FAILED : HOP_XFER_TASK_PARTIAL));

    if (rx->services.status_end)
        rx->services.status_end(rx->services.context, batch,
            complete ? HOP_XFER_COMPLETED :
                (image_failed ? HOP_XFER_FAILED : HOP_XFER_PARTIAL),
            complete ? "Receive completed" :
                "Receive incomplete; clipboard not updated");

    int result = complete ? HOP_DONE_ACK_COMPLETE :
        (image_failed ? HOP_DONE_ACK_FAILED : HOP_DONE_ACK_PARTIAL);

    /* The writer was drained/closed above. Clear the batch state directly:
     * rx_reset would close it a second time and publish an "interrupted" status
     * after the terminal result. */
    hop_xfer_rx_reset_batch_state(rx);
    return result;
}

void hop_xfer_rx_handle_file_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    if (rx->batch_state.active && hop_xfer_task_cancel_requested(rx->task.id)) {
        rx_handler_terminate(rx, rx->batch_state.batch, HOP_ABORT_CANCEL, 1);
        return;
    }

    hop_xfer_file_begin_msg msg;
    if (!hop_xfer_decode_file_begin(p, len, &msg)) {
        hop_log(HOP_LOG_WARN, "xfer: malformed FILE_BEGIN rejected");
        if (rx->batch_state.active)
            rx_handler_terminate(rx, rx->batch_state.batch, HOP_ABORT_TOO_LARGE, 1);
        if (rx->services.shutdown_data_channel)
            rx->services.shutdown_data_channel(rx->services.context);
        return;
    }

    const uint32_t batch = msg.batch;
    const uint32_t id = msg.id;
    const uint64_t size = msg.size;
    const int is_directory = (msg.flags & HOP_FILE_FLAG_DIR) != 0;
    rx_handler_begin_batch(rx, batch);
    if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
    if (!hop_xfer_task_set_kind(&rx->task, HOP_TASK_KIND_FILES) ||
        hop_xfer_task_cancel_requested(rx->task.id)) {
        rx_handler_terminate(rx, batch, HOP_ABORT_CANCEL, 1);
        return;
    }
    if (!id || id > HOP_XFER_FILE_LIMIT) {
        rx_handler_terminate(rx, batch, HOP_ABORT_TOO_LARGE, 1);
        return;
    }

    /* Shared validation and file-state transition. A failed candidate must not
     * leave the previous entry path attached to this new FILE_END. */
    if (!hop_xfer_rx_file_state_prepare_entry(&rx->file_state, id, size,
            is_directory, msg.relpath, msg.relpath_len)) {
        free(rx->cur_path);
        rx->cur_path = NULL;
        hop_log(HOP_LOG_WARN, "xfer: unsafe relative path rejected");
        return;
    }

    char relative[HOP_PATH_LEN_MAX + 1];
    memcpy(relative, msg.relpath, msg.relpath_len);
    relative[msg.relpath_len] = '\0';
    if (!rx->services.build_path || !rx->services.prepare_file_path) {
        free(rx->cur_path);
        rx->cur_path = NULL;
        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
        return;
    }

    void *candidate = rx->services.build_path(
        rx->services.context, relative, msg.relpath_len);
    if (!candidate) {
        free(rx->cur_path);
        rx->cur_path = NULL;
        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_BADPATH);
        return;
    }

    hop_xfer_rx_entry_reserve_result reservation =
        hop_xfer_rx_entry_reservations_reserve(&rx->entry_reservations, id, size,
            rx_reserve_entry_budget, &rx->task);
    if (reservation != HOP_RX_ENTRY_RESERVE_NEW &&
        reservation != HOP_RX_ENTRY_RESERVE_ALREADY_HELD) {
        free(candidate);
        rx_handler_terminate(rx, batch, HOP_ABORT_TOO_LARGE, 1);
        return;
    }

    /* Admission is the commit point: only now publish the candidate and allow
     * writer/filesystem/clipboard/status side effects. */
    free(rx->cur_path);
    rx->cur_path = (hop_xfer_rx_path_char *)candidate;
    if (rx->services.reset_file_error)
        rx->services.reset_file_error(rx->services.context);
    if (rx->services.remove_top_path)
        rx->services.remove_top_path(rx->services.context, rx->cur_path);
    if (rx->services.status_add_received_file)
        rx->services.status_add_received_file(rx->services.context, batch, id,
            relative, size, is_directory);

    if (!rx->services.prepare_file_path(
            rx->services.context, rx->cur_path, is_directory)) {
        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
        return;
    }

    if (is_directory) {
        if (rx->services.is_top_level_path &&
            rx->services.is_top_level_path(rx->services.context, rx->cur_path) &&
            (!rx->services.add_top_path ||
             !rx->services.add_top_path(rx->services.context, rx->cur_path)))
            hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
        return;
    }

    if (!rx->services.open_file ||
        !rx->services.open_file(rx->services.context, rx->cur_path))
        hop_xfer_rx_file_state_fail(&rx->file_state, HOP_FILE_ACK_DISK);
}

void hop_xfer_rx_handle_file_end(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    if (rx->batch_state.active && hop_xfer_task_cancel_requested(rx->task.id)) {
        rx_handler_terminate(rx, rx->batch_state.batch, HOP_ABORT_CANCEL, 1);
        return;
    }

    hop_xfer_file_end_msg msg;
    if (!hop_xfer_decode_file_end(p, len, &msg)) return;
    if (!rx->batch_state.active ||
        !hop_xfer_rx_file_state_matches(&rx->file_state, msg.id)) return;
    if (rx->file_state.is_image) return; /* image batches have no per-file ACK */

    const uint32_t batch = rx->batch_state.batch;
    if (rx->services.status_file_phase)
        rx->services.status_file_phase(rx->services.context, batch,
            (int)msg.id - 1, HOP_XFER_FILE_VERIFYING, 0);

    if (!rx->file_state.is_directory && rx->services.close_file)
        rx->services.close_file(rx->services.context);

    const int writer_error = rx->services.file_error
        ? rx->services.file_error(rx->services.context) : 1;
    const hop_xfer_rx_file_end_result result =
        hop_xfer_rx_file_state_evaluate_end(
            &rx->file_state, msg.status, writer_error, msg.file_crc);
    int ack = result.ack_status;

    if (ack == HOP_FILE_ACK_OK && !rx->file_state.is_directory &&
        rx->cur_path && rx->services.is_top_level_path &&
        rx->services.is_top_level_path(rx->services.context, rx->cur_path)) {
        if (!rx->services.add_top_path ||
            !rx->services.add_top_path(rx->services.context, rx->cur_path))
            ack = HOP_FILE_ACK_DISK;
    }

    if (ack != HOP_FILE_ACK_OK && !rx->file_state.is_directory &&
        rx->cur_path && rx->services.remove_file &&
        !rx->services.remove_file(rx->services.context, rx->cur_path))
        hop_log(HOP_LOG_WARN, "xfer: failed to remove incomplete received file");

    if (result.reason == HOP_RX_FILE_END_WRITER_ERROR)
        hop_log(HOP_LOG_ERROR, "xfer: receiver writer reported an I/O error");
    else if (result.reason == HOP_RX_FILE_END_INCOMPLETE)
        hop_log(HOP_LOG_ERROR, "xfer: incomplete received file rejected");
    else if (result.reason == HOP_RX_FILE_END_CRC_MISMATCH)
        hop_log(HOP_LOG_ERROR, "xfer: received file CRC mismatch");

    if (rx->services.status_file_phase)
        rx->services.status_file_phase(rx->services.context, batch,
            (int)msg.id - 1,
            ack == HOP_FILE_ACK_OK ? HOP_XFER_FILE_COMPLETED : HOP_XFER_FILE_FAILED, 0);
    if (rx->services.send_file_ack)
        rx->services.send_file_ack(rx->services.context, batch, msg.id, ack);
}

void hop_xfer_rx_handle_image_begin(void *context, const uint8_t *p, uint32_t len) {
    xfer_rx_context *rx = (xfer_rx_context *)context;
    if (!rx) return;
    if (rx->batch_state.active && hop_xfer_task_cancel_requested(rx->task.id)) {
        rx_handler_terminate(rx, rx->batch_state.batch, HOP_ABORT_CANCEL, 1);
        return;
    }
    hop_xfer_image_begin_msg msg;
            if (!hop_xfer_decode_image_begin(p, len, &msg)) {
                hop_log(HOP_LOG_WARN, "xfer: malformed IMAGE_BEGIN rejected");
                if (rx->batch_state.active) rx_handler_terminate(rx, rx->batch_state.batch,HOP_ABORT_TOO_LARGE,1);
                return;
            }
            uint32_t batch = msg.batch;
            rx_handler_begin_batch(rx, batch);
            if (!rx->batch_state.active || rx->batch_state.batch != batch) return;
            if (!hop_xfer_task_set_kind(&rx->task,HOP_TASK_KIND_IMAGE) || hop_xfer_task_cancel_requested(rx->task.id)) {
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
            if (hop_xfer_task_cancel_requested(rx->task.id)) {
                rx_handler_terminate(rx, rx->batch_state.batch, HOP_ABORT_CANCEL, 1);
                return;
            }
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
