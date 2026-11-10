#include "xfer_sender_engine.h"

#include "xfer_ack_window.h"
#include "xfer_status.h"
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
