#ifndef HOP_XFER_TASK_HISTORY_H
#define HOP_XFER_TASK_HISTORY_H

#include <stdint.h>
#include "xfer_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One durable terminal task record = the Task Manager's terminal snapshot plus
 * its user-facing summary. record_id is monotonic and stable across restarts;
 * task_id is process-local (0 for records migrated from the legacy history).
 * transfer_id keeps the legacy batch number so old rows keep their context. */
typedef struct {
    uint64_t record_id;
    hop_xfer_task_id task_id;
    uint32_t request_id, transfer_id;
    int direction;   /* HOP_XFER_DIR_SEND / HOP_XFER_DIR_RECEIVE */
    int state;       /* terminal hop_xfer_task_state */
    uint32_t files_done, files_total;
    uint64_t bytes_done, bytes_total;
    uint64_t finished_at; /* wall-clock ms since 1970; migrated rows are second-granular */
    char name[HOP_XFER_STATUS_NAME];
    char detail[HOP_XFER_STATUS_DETAIL];
} hop_xfer_task_history_record;

/* Opens the durable unified history (idempotent; returns 1 if already open).
 * path is a UTF-8 path for the v2 store file. If that file is missing or
 * invalid and legacy_path is given, one-time migration imports the legacy
 * HOPXHST1 history (transfer-history.bin) and immediately persists the new
 * store so the legacy file is never imported twice. The legacy file itself is
 * left untouched as a backup. On success the store registers itself as the
 * Task Manager terminal sink: every later user-facing terminal task is
 * appended and persisted automatically. IO failures never change task
 * lifecycle results. */
int hop_xfer_task_history_store_open(const char *path, const char *legacy_path);
int hop_xfer_task_history_store_is_open(void);

/* Terminal records in finish order. direction NONE and state EMPTY mean no
 * filter. newest_first must be 0 or 1. Returns copied count, or -1 on invalid
 * arguments; total receives the matching retained count even when truncated. */
int hop_xfer_task_history_store_list(hop_xfer_task_history_record *out, int capacity,
                                     int direction, int state, int newest_first, int *total);

/* Test/shutdown helper: unregisters the sink and drops in-memory state.
 * The on-disk file is kept as-is. */
void hop_xfer_task_history_store_close(void);

#ifdef __cplusplus
}
#endif
#endif