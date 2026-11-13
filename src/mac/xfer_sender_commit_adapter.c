#include "xfer_sender_internal.h"
#include "../core/xfer_sender_ack_wait.h"
#include "../core/xfer_sender_engine.h"
#include "../core/hop.h"
#include <string.h>
#include <time.h>

static int sender_commit_send_done(void *user, intptr_t socket_handle,
                                   uint64_t epoch, uint32_t batch,
                                   uint32_t ok_count, uint32_t total_count) {
    const xf_job *job = (const xf_job *)user;
    if (!job) return 0;
    uint8_t payload[20];
    hop_put_be32(payload, batch);
    hop_put_be32(payload + 4, ok_count);
    hop_put_be32(payload + 8, total_count);
    hop_put_be64(payload + 12, 0);
    s_send_epoch = (unsigned long)epoch;
    return send_small_with_context(&job->sender_context, (int)socket_handle,
                                   HOP_MSG_XFER_DONE, payload,
                                   sizeof(payload), batch);
}

static uint64_t sender_commit_now_ms(void *user) {
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static uint64_t sender_commit_now_ns(void *user) {
    (void)user;
    return rate_now_ns();
}

static int sender_commit_is_stopping(void *user) {
    const xf_job *job = (const xf_job *)user;
    return !job || !job->sender_context.is_stopping ||
           job->sender_context.is_stopping(job->sender_context.user);
}

static int sender_commit_session_matches(void *user, const uint8_t *session) {
    (void)user;
    hop_session_id id;
    if (!session) return 0;
    memcpy(id.bytes, session, sizeof(id.bytes));
    return hop_session_matches(&id);
}

static int sender_commit_current_connection(void *user, intptr_t *socket_handle,
                                            uint64_t *epoch, uint8_t *session,
                                            size_t session_capacity) {
    xf_job *job = (xf_job *)user;
    if (!job || !socket_handle || !epoch || !session ||
        session_capacity < HOP_SESSION_BYTES) return 0;
    pthread_mutex_lock(&s_x.slock);
    int ready = s_x.peer_ready && s_x.sock >= 0;
    if (ready) {
        *socket_handle = (intptr_t)s_x.sock;
        *epoch = s_x.epoch;
        memcpy(session, s_x.session.bytes, HOP_SESSION_BYTES);
        job->commit_socket = *socket_handle;
        job->commit_epoch = *epoch;
    }
    pthread_mutex_unlock(&s_x.slock);
    return ready;
}

static hop_xfer_sender_commit_ack_result sender_commit_wait_ack(
    void *user, uint32_t batch, uint32_t slot, uint64_t deadline_ns) {
    const xf_job *job = (const xf_job *)user;
    if (!job) return HOP_XFER_SENDER_COMMIT_ACK_OTHER;
    int result = hop_xfer_sender_ack_wait(&job->sender_context,
        job->commit_socket, job->commit_epoch, batch, job->task.id, slot,
        deadline_ns, 100u, NULL);
    if (result == HOP_DONE_ACK_COMPLETE) return HOP_XFER_SENDER_COMMIT_ACK_COMPLETE;
    if (result == HOP_DONE_ACK_PARTIAL) return HOP_XFER_SENDER_COMMIT_ACK_PARTIAL;
    if (result == HOP_DONE_ACK_FAILED) return HOP_XFER_SENDER_COMMIT_ACK_FAILED;
    if (result == HOP_XFER_SENDER_ACK_WAIT_PEER_ABORT) return HOP_XFER_SENDER_COMMIT_ACK_PEER_ABORT;
    if (result == HOP_XFER_SENDER_ACK_WAIT_TIMEOUT) return HOP_XFER_SENDER_COMMIT_ACK_TIMEOUT;
    if (result == HOP_XFER_SENDER_ACK_WAIT_SEND_DEAD) return HOP_XFER_SENDER_COMMIT_ACK_SEND_DEAD;
    return HOP_XFER_SENDER_COMMIT_ACK_OTHER;
}

static int sender_commit_wait_ms(void *user, uint32_t duration_ms) {
    const xf_job *job = (const xf_job *)user;
    return job && job->sender_context.wait_ns &&
           job->sender_context.wait_ns(job->sender_context.user,
                                       (uint64_t)duration_ms * 1000000ull);
}

hop_xfer_sender_job_result sender_commit_adapter_run(const xf_job *job) {
    if (!job) return HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN;
    hop_xfer_sender_commit_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = (void *)job;
    ops.now_ms = sender_commit_now_ms;
    ops.now_ns = sender_commit_now_ns;
    ops.is_stopping = sender_commit_is_stopping;
    ops.session_matches = sender_commit_session_matches;
    ops.current_connection = sender_commit_current_connection;
    ops.send_done = sender_commit_send_done;
    ops.ack_coordinator = job->sender_context.ack_coordinator;
    ops.wait_ack = sender_commit_wait_ack;
    ops.wait_ms = sender_commit_wait_ms;

    hop_xfer_sender_commit_result result = hop_xfer_sender_run_commit(
        &ops, job->session.bytes, job->batch, job->is_image, job->n, job->ok,
        HOP_DONE_ACK_SLOT, HOP_XFER_ACK_TIMEOUT_MS,
        HOP_XFER_RECONNECT_BUDGET_MS, 100);
    if (result == HOP_XFER_SENDER_COMMIT_DONE) return HOP_XFER_SENDER_JOB_DONE;
    if (result == HOP_XFER_SENDER_COMMIT_REJECTED) return HOP_XFER_SENDER_JOB_COMMIT_REJECTED;
    if (result == HOP_XFER_SENDER_COMMIT_PEER_ABORT) return HOP_XFER_SENDER_JOB_PEER_ABORT;
    return HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN;
}
