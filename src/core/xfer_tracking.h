#ifndef HOP_XFER_TRACKING_H
#define HOP_XFER_TRACKING_H
/* Caller serializes access. One tracker per direction; batch IDs are not
 * globally unique across the two endpoints. No networking or GUI dependencies. */
#include "xfer_status.h"
#include <stdint.h>

typedef enum {
    HOP_XFER_TIME_SCAN = 0,
    HOP_XFER_TIME_READ,
    HOP_XFER_TIME_CRC,
    HOP_XFER_TIME_SEND,
    HOP_XFER_TIME_RECEIVE,
    HOP_XFER_TIME_WRITE,
    HOP_XFER_TIME_CLIPBOARD_DISPATCH,
    HOP_XFER_TIME_COUNT
} hop_xfer_timing_stage;

typedef struct {
    hop_xfer_status status;
    hop_xfer_file_status files[HOP_XFER_FILE_LIMIT];
    int count;
    uint64_t revision;
    uint64_t sample_ms;
    uint64_t sample_bytes;
    /* Per-batch cumulative wall time by stage; durations may overlap across
     * receive parsing and the asynchronous disk writer. */
    uint64_t stage_ns[HOP_XFER_TIME_COUNT];
} hop_xfer_tracking;

#ifdef __cplusplus
extern "C" {
#endif

uint32_t hop_xfer_job_total(int image, int entries);

int hop_xfer_tracking_matches(const hop_xfer_tracking *t, uint32_t batch);

void hop_xfer_tracking_add_time(hop_xfer_tracking *t, uint32_t batch,
        int stage, uint64_t elapsed_ns);

void hop_xfer_tracking_begin(hop_xfer_tracking *t, uint32_t batch,
        int direction, uint32_t files, uint64_t bytes, const char *name, int phase);

void hop_xfer_tracking_file(hop_xfer_tracking *t, uint32_t batch,
        uint32_t id, const char *path, uint64_t size, int directory, int phase);

void hop_xfer_tracking_chunk(hop_xfer_tracking *t, uint32_t batch,
        uint32_t id, uint64_t off, uint32_t size, uint64_t now_ms);

void hop_xfer_tracking_phase(hop_xfer_tracking *t, uint32_t batch,
        int index, int phase, uint32_t attempts);

int hop_xfer_tracking_end(hop_xfer_tracking *t, uint32_t batch,
        int phase, const char *detail);

hop_xfer_tracking *hop_xfer_tracking_visible(hop_xfer_tracking *send,
        hop_xfer_tracking *receive);

void hop_xfer_tracking_snapshot_pair(const hop_xfer_tracking *send,
        const hop_xfer_tracking *receive, uint32_t queued, hop_xfer_status_pair *out);

#ifdef __cplusplus
}
#endif
#endif
