#include "../src/core/xfer_sender_lifecycle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(hop_xfer_sender_job_result result, int peer_cancelled,
                   int complete, hop_xfer_task_state task_state,
                   int phase, const char *detail) {
    hop_xfer_sender_terminal_decision decision;
    assert(hop_xfer_sender_get_terminal_decision(
        result, peer_cancelled, complete, &decision));
    assert(decision.task_state == task_state);
    assert(decision.status_phase == phase);
    assert(strcmp(decision.detail, detail) == 0);
}

int main(void) {
    expect(HOP_XFER_SENDER_JOB_DONE, 0, 1,
           HOP_XFER_TASK_COMPLETED, HOP_XFER_COMPLETED, "Transfer completed");
    expect(HOP_XFER_SENDER_JOB_DONE, 0, 0,
           HOP_XFER_TASK_PARTIAL, HOP_XFER_PARTIAL, "Some files failed");
    expect(HOP_XFER_SENDER_JOB_CANCELLED, 0, 0,
           HOP_XFER_TASK_CANCELLED, HOP_XFER_CANCELLED, "Cancelled");
    expect(HOP_XFER_SENDER_JOB_PEER_ABORT, 1, 0,
           HOP_XFER_TASK_CANCELLED, HOP_XFER_CANCELLED, "Cancelled by receiver");
    expect(HOP_XFER_SENDER_JOB_PEER_ABORT, 0, 0,
           HOP_XFER_TASK_FAILED, HOP_XFER_FAILED, "Peer aborted transfer");
    expect(HOP_XFER_SENDER_JOB_COMMIT_REJECTED, 0, 0,
           HOP_XFER_TASK_FAILED, HOP_XFER_FAILED, "Receiver rejected completion");
    expect(HOP_XFER_SENDER_JOB_COMMIT_UNKNOWN, 0, 0,
           HOP_XFER_TASK_INTERRUPTED, HOP_XFER_FAILED,
           "Completion delivery uncertain; peer may have received it");
    expect(HOP_XFER_SENDER_JOB_NET_DOWN, 0, 0,
           HOP_XFER_TASK_FAILED, HOP_XFER_FAILED, "Reconnect attempts exhausted");

    hop_xfer_sender_terminal_decision invalid;
    assert(!hop_xfer_sender_get_terminal_decision(
        (hop_xfer_sender_job_result)99, 0, 0, &invalid));
    assert(!hop_xfer_sender_get_terminal_decision(
        HOP_XFER_SENDER_JOB_DONE, 0, 1, NULL));

    puts("PASS: shared Sender terminal Task/status mapping");
    return 0;
}
