#include "xfer_sender_engine.h"

#include "xfer_ack_window.h"
#include "xfer_status.h"
#include "xfer_tracking.h"
#include "hop.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const void *entry;
    int index;
    uint32_t id;
    unsigned attempts;
    int acknowledged;
    uint64_t deadline_ns;
} sender_window_item;

static void sender_log(const hop_xfer_sender_engine_ops *ops, int level,
                       const char *message) {
    if (ops->log_message) ops->log_message(ops->user, level, message);
}

static void sender_file_log(const hop_xfer_sender_file_ops *ops, int level,
                            const char *message);

static int sender_cancelled(const hop_xfer_sender_engine_ops *ops,
                            uint32_t batch) {
    return ops->cancel_requested(ops->user, batch);
}

static int sender_aborted(const hop_xfer_sender_engine_ops *ops,
                          uint32_t batch) {
    return ops->peer_aborted(ops->user, batch);
}

static int sender_interrupted_result(const hop_xfer_sender_engine_ops *ops,
                                     uint32_t batch) {
    if (sender_cancelled(ops, batch)) return HOP_XFER_SENDER_JOB_CANCELLED;
    if (sender_aborted(ops, batch)) return HOP_XFER_SENDER_JOB_PEER_ABORT;
    return HOP_XFER_SENDER_JOB_NET_DOWN;
}

static int sender_wait_retry(const hop_xfer_sender_engine_ops *ops,
                             uint32_t batch, unsigned attempt,
                             const char *path, int ack_retry, int status) {
    unsigned delay = ops->retry_delay_ms(ops->user, attempt);
    char message[512];
    if (ack_retry) {
        snprintf(message, sizeof(message),
                 "xfer: retry %u for %s in %u ms (ACK status %d)",
                 attempt, path ? path : "(unknown)", delay, status);
    } else {
        snprintf(message, sizeof(message),
                 "xfer: retry %u for %s in %u ms (send status %d)",
                 attempt, path ? path : "(unknown)", delay, status);
    }
    sender_log(ops, 1, message);
    if (!ops->wait_ms(ops->user, delay)) return 0;
    return !sender_cancelled(ops, batch) && !sender_aborted(ops, batch);
}

static void sender_unregister(const hop_xfer_sender_engine_ops *ops,
                              uint32_t batch) {
    ops->unregister_batch(ops->user, batch);
}

int hop_xfer_sender_run_files(const hop_xfer_sender_engine_ops *ops,
                              uint32_t batch, int entry_count,
                              int *next_index, int *ok_count,
                              unsigned max_attempts) {
    if (!ops || !next_index || !ok_count || !batch || entry_count < 0 ||
        *next_index < 0 || *next_index > entry_count || *ok_count < 0 ||
        !max_attempts || !ops->cancel_requested || !ops->peer_aborted ||
        !ops->entry_at || !ops->entry_path || !ops->send_entry ||
        !ops->wait_ack || !ops->unregister_batch || !ops->status_file_phase ||
        !ops->retry_delay_ms || !ops->wait_ms) {
        return HOP_XFER_SENDER_JOB_NET_DOWN;
    }

    sender_unregister(ops, batch);
    if (sender_cancelled(ops, batch)) return HOP_XFER_SENDER_JOB_CANCELLED;
    if (sender_aborted(ops, batch)) return HOP_XFER_SENDER_JOB_PEER_ABORT;

    while (*next_index < entry_count) {
        sender_window_item window[HOP_XFER_ACK_WINDOW_CAPACITY];
        memset(window, 0, sizeof(window));
        uint32_t window_count = 0;
        int send_index = *next_index;
        int fill_result = HOP_XFER_SENDER_JOB_DONE;

        /* Fill using a speculative cursor. The durable checkpoint is not
         * changed until the corresponding remote ACK has been consumed. */
        while (window_count < HOP_XFER_ACK_WINDOW_CAPACITY &&
               send_index < entry_count) {
            if (sender_cancelled(ops, batch)) {
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_CANCELLED;
            }
            if (sender_aborted(ops, batch)) {
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_PEER_ABORT;
            }

            int index = send_index;
            const void *entry = ops->entry_at(ops->user, index);
            const char *path = entry ? ops->entry_path(ops->user, entry) : NULL;
            if (!entry) {
                sender_log(ops, 2, "xfer: sender entry snapshot is missing");
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_NET_DOWN;
            }

            sender_window_item *item = &window[window_count];
            item->entry = entry;
            item->id = (uint32_t)(index + 1);
            item->index = index;
            ops->status_file_phase(ops->user, batch, index,
                                   HOP_XFER_FILE_SENDING, 0);

            hop_xfer_sender_send_result send_result = HOP_XFER_SENDER_SEND_RETRYABLE;
            unsigned attempts = 0;
            uint64_t deadline_ns = 0;
            for (;;) {
                ++attempts;
                send_result = ops->send_entry(ops->user, batch, item->id,
                                              entry, &deadline_ns);
                if (send_result == HOP_XFER_SENDER_SEND_PENDING ||
                    send_result == HOP_XFER_SENDER_SEND_DEAD ||
                    send_result == HOP_XFER_SENDER_SEND_PEER_ABORT ||
                    send_result == HOP_XFER_SENDER_SEND_CANCELLED ||
                    attempts >= max_attempts) {
                    break;
                }
                if (!sender_wait_retry(ops, batch, attempts, path, 0,
                                       (int)send_result)) {
                    int interrupted = sender_interrupted_result(ops, batch);
                    sender_unregister(ops, batch);
                    return interrupted;
                }
            }

            if (send_result == HOP_XFER_SENDER_SEND_DEAD) {
                fill_result = HOP_XFER_SENDER_JOB_NET_DOWN;
                break;
            }
            if (send_result == HOP_XFER_SENDER_SEND_PEER_ABORT) {
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_PEER_ABORT;
            }
            if (send_result == HOP_XFER_SENDER_SEND_CANCELLED) {
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_CANCELLED;
            }
            if (send_result != HOP_XFER_SENDER_SEND_PENDING) {
                ops->status_file_phase(ops->user, batch, index,
                                       HOP_XFER_FILE_FAILED, attempts);
                char message[512];
                snprintf(message, sizeof(message),
                         "xfer: no ACK for %s after %u attempt(s); keep checkpoint",
                         path ? path : "(unknown)", attempts);
                sender_log(ops, 2, message);
                fill_result = HOP_XFER_SENDER_JOB_NET_DOWN;
                break;
            }

            item->attempts = attempts;
            item->deadline_ns = deadline_ns;
            ++window_count;
            ++send_index;
        }

        /* Drain in file order, committing only the contiguous acknowledged
         * prefix. A later ACK cannot skip an unresolved earlier entry. */
        for (uint32_t wi = 0; wi < window_count; ++wi) {
            sender_window_item *item = &window[wi];
            uint64_t rtt_us = 0;
            hop_xfer_sender_ack_result ack = ops->wait_ack(
                ops->user, batch, item->id, item->deadline_ns, &rtt_us);
            if (ack == HOP_XFER_SENDER_ACK_OK ||
                ack == HOP_XFER_SENDER_ACK_BADPATH ||
                ack == HOP_XFER_SENDER_ACK_OTHER_REMOTE) {
                item->acknowledged = 1;
            }
            if (rtt_us && ops->rate_feedback)
                ops->rate_feedback(ops->user, rtt_us);

            unsigned attempts = item->attempts;
            while (ack != HOP_XFER_SENDER_ACK_OK &&
                   ack != HOP_XFER_SENDER_ACK_BADPATH) {
                if (ack == HOP_XFER_SENDER_ACK_TIMEOUT) {
                    char message[512];
                    snprintf(message, sizeof(message),
                             "xfer: ACK timeout for %s; reconnect before retrying entry %d",
                             ops->entry_path(ops->user, item->entry) ?
                                 ops->entry_path(ops->user, item->entry) : "(unknown)",
                             item->index);
                    sender_log(ops, 1, message);
                    sender_unregister(ops, batch);
                    if (ops->request_reconnect) ops->request_reconnect(ops->user);
                    return HOP_XFER_SENDER_JOB_NET_DOWN;
                }
                if (ack == HOP_XFER_SENDER_ACK_SEND_DEAD) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_NET_DOWN;
                }
                if (ack == HOP_XFER_SENDER_ACK_PEER_ABORT) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_PEER_ABORT;
                }
                if (ack == HOP_XFER_SENDER_ACK_CANCELLED) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_CANCELLED;
                }
                if (attempts >= max_attempts) break;

                const char *path = ops->entry_path(ops->user, item->entry);
                if (!sender_wait_retry(ops, batch, attempts, path, 1,
                                       (int)ack)) {
                    int interrupted = sender_interrupted_result(ops, batch);
                    sender_unregister(ops, batch);
                    return interrupted;
                }

                ++attempts;
                uint64_t retry_deadline_ns = 0;
                hop_xfer_sender_send_result send_result = ops->send_entry(
                    ops->user, batch, item->id, item->entry, &retry_deadline_ns);
                if (send_result == HOP_XFER_SENDER_SEND_DEAD) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_NET_DOWN;
                }
                if (send_result == HOP_XFER_SENDER_SEND_PEER_ABORT) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_PEER_ABORT;
                }
                if (send_result == HOP_XFER_SENDER_SEND_CANCELLED) {
                    sender_unregister(ops, batch);
                    return HOP_XFER_SENDER_JOB_CANCELLED;
                }
                if (send_result == HOP_XFER_SENDER_SEND_PENDING) {
                    rtt_us = 0;
                    ack = ops->wait_ack(ops->user, batch, item->id,
                                        retry_deadline_ns, &rtt_us);
                    if (ack == HOP_XFER_SENDER_ACK_OK ||
                        ack == HOP_XFER_SENDER_ACK_BADPATH ||
                        ack == HOP_XFER_SENDER_ACK_OTHER_REMOTE) {
                        item->acknowledged = 1;
                    }
                    if (rtt_us && ops->rate_feedback)
                        ops->rate_feedback(ops->user, rtt_us);
                } else {
                    ack = HOP_XFER_SENDER_ACK_LOCAL_FAILURE;
                }
            }

            if (ack == HOP_XFER_SENDER_ACK_OK) {
                ++*ok_count;
                ops->status_file_phase(ops->user, batch, item->index,
                                       HOP_XFER_FILE_COMPLETED, attempts);
            } else {
                ops->status_file_phase(ops->user, batch, item->index,
                                       HOP_XFER_FILE_FAILED, attempts);
                char message[512];
                snprintf(message, sizeof(message),
                         "xfer: giving up on %s after %u attempt(s) (ACK result %d)",
                         ops->entry_path(ops->user, item->entry) ?
                             ops->entry_path(ops->user, item->entry) : "(unknown)",
                         attempts, (int)ack);
                sender_log(ops, 2, message);
            }

            if (!item->acknowledged) {
                char message[256];
                snprintf(message, sizeof(message),
                         "xfer: entry %d has no remote ACK; checkpoint remains %d",
                         item->index, *next_index);
                sender_log(ops, 2, message);
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_NET_DOWN;
            }
            if (!hop_xfer_ack_window_advance_checkpoint(next_index,
                                                        item->index, 1)) {
                char message[256];
                snprintf(message, sizeof(message),
                         "xfer: non-contiguous checkpoint batch=%u current=%d resolved=%d",
                         batch, *next_index, item->index);
                sender_log(ops, 2, message);
                sender_unregister(ops, batch);
                return HOP_XFER_SENDER_JOB_NET_DOWN;
            }
        }

        if (fill_result != HOP_XFER_SENDER_JOB_DONE) {
            sender_unregister(ops, batch);
            return fill_result;
        }
    }

    sender_unregister(ops, batch);
    return HOP_XFER_SENDER_JOB_DONE;
}
 

static void sender_file_log(const hop_xfer_sender_file_ops *ops, int level,
                            const char *message) {
    if (ops->log_message) ops->log_message(ops->user, level, message);
}

static void sender_file_unregister(const hop_xfer_sender_file_ops *ops,
                                   uint32_t batch, uint32_t id) {
    ops->ack_unregister(ops->user, batch, id);
}

hop_xfer_sender_send_result hop_xfer_sender_send_file_entry(
    const hop_xfer_sender_file_ops *ops, uint32_t batch, uint32_t id,
    const hop_xfer_sender_file_entry *entry, uint8_t *begin_buffer,
    uint8_t *chunk_buffer, uint32_t chunk_size, uint64_t ack_timeout_ns,
    uint32_t directory_flag, uint8_t file_ok_status,
    uint8_t file_io_error_status, uint8_t file_begin_message,
    uint8_t file_end_message, uint64_t *ack_deadline_ns) {
    if (!ops || !entry || !entry->platform_entry || !entry->name ||
        !entry->relative_path || !begin_buffer || !chunk_buffer ||
        !chunk_size || !ack_timeout_ns || !ack_deadline_ns ||
        !ops->cancel_requested || !ops->peer_aborted || !ops->open_file ||
        !ops->read_file || !ops->close_file || !ops->send_message ||
        !ops->send_chunk || !ops->ack_register || !ops->ack_mark_sent ||
        !ops->ack_set_deadline || !ops->ack_unregister || !ops->now_ns) {
        return HOP_XFER_SENDER_SEND_RETRYABLE;
    }
    *ack_deadline_ns = 0;

    size_t name_length = strlen(entry->name);
    size_t path_length = strlen(entry->relative_path);
    if (name_length > UINT16_MAX || path_length > UINT16_MAX)
        return HOP_XFER_SENDER_SEND_RETRYABLE;
    size_t begin_length = 4u + 4u + 8u + 8u + 1u + 2u +
                          name_length + 2u + path_length;
    if (begin_length > chunk_size || begin_length > UINT32_MAX)
        return HOP_XFER_SENDER_SEND_RETRYABLE;

    if (!ops->ack_register(ops->user, batch, id))
        return HOP_XFER_SENDER_SEND_RETRYABLE;

    uint32_t offset = 0;
    hop_put_be32(begin_buffer + offset, batch); offset += 4;
    hop_put_be32(begin_buffer + offset, id); offset += 4;
    hop_put_be64(begin_buffer + offset, entry->size); offset += 8;
    hop_put_be64(begin_buffer + offset, entry->mtime); offset += 8;
    begin_buffer[offset++] = (uint8_t)entry->flags;
    hop_put_be16(begin_buffer + offset, (uint16_t)name_length); offset += 2;
    memcpy(begin_buffer + offset, entry->name, name_length); offset += (uint32_t)name_length;
    hop_put_be16(begin_buffer + offset, (uint16_t)path_length); offset += 2;
    memcpy(begin_buffer + offset, entry->relative_path, path_length);

    if (!ops->send_message(ops->user, file_begin_message, begin_buffer,
                           (uint32_t)begin_length, batch)) {
        sender_file_unregister(ops, batch, id);
        return HOP_XFER_SENDER_SEND_DEAD;
    }

    uint32_t file_crc = 0;
    uint8_t end_status = file_ok_status;
    int local_failure = 0;
    if (!(entry->flags & directory_flag)) {
        intptr_t file_handle = (intptr_t)-1;
        uint32_t system_error = 0;
        if (!ops->open_file(ops->user, entry->platform_entry,
                            &file_handle, &system_error)) {
            char message[512];
            snprintf(message, sizeof(message),
                     "xfer: open failed, %s (system error %lu)",
                     entry->relative_path, (unsigned long)system_error);
            sender_file_log(ops, 1, message);
            end_status = file_io_error_status;
            local_failure = 1;
        } else {
            uint64_t file_offset = 0;
            while (file_offset < entry->size) {
                if (ops->cancel_requested(ops->user, batch)) {
                    ops->close_file(ops->user, file_handle);
                    sender_file_unregister(ops, batch, id);
                    return HOP_XFER_SENDER_SEND_CANCELLED;
                }
                if (ops->peer_aborted(ops->user, batch)) {
                    ops->close_file(ops->user, file_handle);
                    sender_file_unregister(ops, batch, id);
                    return HOP_XFER_SENDER_SEND_PEER_ABORT;
                }

                uint64_t remaining = entry->size - file_offset;
                uint32_t request = (uint32_t)(remaining < chunk_size ?
                                              remaining : chunk_size);
                uint32_t bytes_read = 0;
                system_error = 0;
                uint64_t read_started_ns = ops->now_ns(ops->user);
                hop_xfer_sender_read_result read_result = ops->read_file(
                    ops->user, file_handle, chunk_buffer, request,
                    &bytes_read, &system_error);
                uint64_t read_finished_ns = ops->now_ns(ops->user);
                uint64_t read_elapsed_ns = read_finished_ns >= read_started_ns
                    ? read_finished_ns - read_started_ns : 0;

                if (read_result == HOP_XFER_SENDER_READ_ERROR) {
                    if (ops->status_stage_time)
                        ops->status_stage_time(ops->user, batch,
                                               HOP_XFER_TIME_READ, read_elapsed_ns);
                    char message[512];
                    snprintf(message, sizeof(message),
                             "xfer: read failed, %s (system error %lu)",
                             entry->relative_path, (unsigned long)system_error);
                    sender_file_log(ops, 1, message);
                    end_status = file_io_error_status;
                    local_failure = 1;
                    break;
                }
                if (read_result != HOP_XFER_SENDER_READ_OK || !bytes_read ||
                    bytes_read > request) {
                    if (ops->status_stage_time)
                        ops->status_stage_time(ops->user, batch,
                                               HOP_XFER_TIME_READ, read_elapsed_ns);
                    if (bytes_read > request) {
                        char message[512];
                        snprintf(message, sizeof(message),
                                 "xfer: invalid read length for %s",
                                 entry->relative_path);
                        sender_file_log(ops, 1, message);
                    } else {
                        char message[512];
                        snprintf(message, sizeof(message),
                                 "xfer: unexpected EOF, %s (%llu/%llu bytes)",
                                 entry->relative_path,
                                 (unsigned long long)file_offset,
                                 (unsigned long long)entry->size);
                        sender_file_log(ops, 1, message);
                    }
                    end_status = file_io_error_status;
                    local_failure = 1;
                    break;
                }

                uint64_t crc_started_ns = ops->now_ns(ops->user);
                uint32_t chunk_crc = hop_crc32(0, chunk_buffer, bytes_read);
                uint64_t crc_finished_ns = ops->now_ns(ops->user);
                uint64_t crc_elapsed_ns = crc_finished_ns >= crc_started_ns
                    ? crc_finished_ns - crc_started_ns : 0;
                if (!ops->send_chunk(ops->user, batch, id, file_offset,
                                     chunk_buffer, bytes_read, chunk_crc,
                                     read_elapsed_ns, crc_elapsed_ns)) {
                    ops->close_file(ops->user, file_handle);
                    sender_file_unregister(ops, batch, id);
                    if (ops->cancel_requested(ops->user, batch))
                        return HOP_XFER_SENDER_SEND_CANCELLED;
                    if (ops->peer_aborted(ops->user, batch))
                        return HOP_XFER_SENDER_SEND_PEER_ABORT;
                    return HOP_XFER_SENDER_SEND_DEAD;
                }
                file_crc = hop_xfer_file_crc_step(file_crc, file_offset, chunk_crc);
                file_offset += bytes_read;
                if (ops->yield_cpu) ops->yield_cpu(ops->user);
                if (ops->cancel_requested(ops->user, batch)) {
                    ops->close_file(ops->user, file_handle);
                    sender_file_unregister(ops, batch, id);
                    return HOP_XFER_SENDER_SEND_CANCELLED;
                }
                if (ops->peer_aborted(ops->user, batch)) {
                    ops->close_file(ops->user, file_handle);
                    sender_file_unregister(ops, batch, id);
                    return HOP_XFER_SENDER_SEND_PEER_ABORT;
                }
            }

            if (!local_failure && file_offset == entry->size) {
                uint8_t extra = 0;
                uint32_t extra_read = 0;
                system_error = 0;
                hop_xfer_sender_read_result probe = ops->read_file(
                    ops->user, file_handle, &extra, 1, &extra_read, &system_error);
                if (probe != HOP_XFER_SENDER_READ_EOF || extra_read != 0) {
                    char message[512];
                    if (probe == HOP_XFER_SENDER_READ_ERROR) {
                        snprintf(message, sizeof(message),
                                 "xfer: source probe failed, %s (system error %lu)",
                                 entry->relative_path, (unsigned long)system_error);
                    } else {
                        snprintf(message, sizeof(message),
                                 "xfer: source changed while queued, %s",
                                 entry->relative_path);
                    }
                    sender_file_log(ops, 1, message);
                    end_status = file_io_error_status;
                    local_failure = 1;
                }
            }
            ops->close_file(ops->user, file_handle);
        }
    }

    uint8_t end_payload[9];
    hop_put_be32(end_payload, id);
    end_payload[4] = end_status;
    hop_put_be32(end_payload + 5, file_crc);
    if (!ops->ack_mark_sent(ops->user, batch, id)) {
        sender_file_unregister(ops, batch, id);
        return HOP_XFER_SENDER_SEND_RETRYABLE;
    }
    if (!ops->send_message(ops->user, file_end_message, end_payload,
                           sizeof(end_payload), batch)) {
        sender_file_unregister(ops, batch, id);
        return HOP_XFER_SENDER_SEND_DEAD;
    }

    uint64_t now = ops->now_ns(ops->user);
    uint64_t deadline = ack_timeout_ns > UINT64_MAX - now
        ? UINT64_MAX : now + ack_timeout_ns;
    *ack_deadline_ns = deadline;
    if (!ops->ack_set_deadline(ops->user, batch, id, deadline)) {
        sender_file_unregister(ops, batch, id);
        return HOP_XFER_SENDER_SEND_RETRYABLE;
    }
    (void)local_failure;
    return HOP_XFER_SENDER_SEND_PENDING;
}

int hop_xfer_sender_run_image(const hop_xfer_sender_image_ops *ops,
                              uint32_t batch, const uint8_t *pixels,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t chunk_size,
                              uint8_t file_ok_status, int entry_count,
                              int *next_index, int *ok_count) {
    if (!ops || !batch || !pixels || !width || !height ||
        (uint64_t)stride < (uint64_t)width * 4ull || !chunk_size ||
        entry_count < 0 || !next_index || !ok_count ||
        *next_index < 0 || *next_index > entry_count ||
        !ops->cancel_requested || !ops->peer_aborted ||
        !ops->send_image_begin || !ops->send_image_chunk ||
        !ops->send_image_end || !ops->status_file_phase) {
        return HOP_XFER_SENDER_JOB_NET_DOWN;
    }

    if (ops->cancel_requested(ops->user, batch))
        return HOP_XFER_SENDER_JOB_CANCELLED;
    if (ops->peer_aborted(ops->user, batch))
        return HOP_XFER_SENDER_JOB_PEER_ABORT;

    if (!ops->send_image_begin(ops->user, batch, width, height, stride))
        return HOP_XFER_SENDER_JOB_NET_DOWN;

    const uint64_t total = (uint64_t)stride * (uint64_t)height;
    uint64_t offset = 0;
    while (offset < total) {
        if (ops->cancel_requested(ops->user, batch))
            return HOP_XFER_SENDER_JOB_CANCELLED;
        if (ops->peer_aborted(ops->user, batch))
            return HOP_XFER_SENDER_JOB_PEER_ABORT;

        uint64_t remaining = total - offset;
        uint32_t length = (uint32_t)(remaining < chunk_size ? remaining : chunk_size);
        uint64_t started_ns = ops->now_ns ? ops->now_ns(ops->user) : 0;
        uint32_t crc = hop_crc32(0, pixels + offset, length);
        uint64_t elapsed_ns = 0;
        if (ops->now_ns) {
            uint64_t finished_ns = ops->now_ns(ops->user);
            elapsed_ns = finished_ns >= started_ns ? finished_ns - started_ns : 0;
        }
        if (!ops->send_image_chunk(ops->user, batch, 1, offset,
                                   pixels + offset, length, crc, elapsed_ns))
            return HOP_XFER_SENDER_JOB_NET_DOWN;

        offset += length;
        if (ops->yield_cpu) ops->yield_cpu(ops->user);
        if (ops->cancel_requested(ops->user, batch))
            return HOP_XFER_SENDER_JOB_CANCELLED;
        if (ops->peer_aborted(ops->user, batch))
            return HOP_XFER_SENDER_JOB_PEER_ABORT;
    }

    if (!ops->send_image_end(ops->user, batch, 1, file_ok_status, 0))
        return HOP_XFER_SENDER_JOB_NET_DOWN;

    *ok_count = 1;
    *next_index = entry_count;
    if (ops->rate_feedback) ops->rate_feedback(ops->user, 0);
    ops->status_file_phase(ops->user, batch, 0, HOP_XFER_FILE_COMPLETED, 1);
    return HOP_XFER_SENDER_JOB_DONE;
}
