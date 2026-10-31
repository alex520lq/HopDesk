#ifndef HOP_XFER_STATUS_H
#define HOP_XFER_STATUS_H
#include <stdint.h>
#define HOP_XFER_STATUS_NAME 260
#define HOP_XFER_STATUS_DETAIL 160
typedef enum {
    HOP_XFER_IDLE = 0,
    HOP_XFER_QUEUED,
    HOP_XFER_SCANNING,
    HOP_XFER_SENDING,
    HOP_XFER_RECEIVING,
    HOP_XFER_VERIFYING,
    HOP_XFER_COMPLETED,
    HOP_XFER_PARTIAL,
    HOP_XFER_FAILED,
    HOP_XFER_CANCELLED
} hop_xfer_phase;
typedef enum { HOP_XFER_DIR_NONE=0, HOP_XFER_DIR_SEND, HOP_XFER_DIR_RECEIVE } hop_xfer_direction;
typedef struct {
    uint32_t batch;
    int active;
    int direction;
    int phase;
    uint32_t files_done;
    uint32_t files_total;
    uint64_t bytes_done;
    uint64_t bytes_total;
    char name[HOP_XFER_STATUS_NAME];
    char detail[HOP_XFER_STATUS_DETAIL];
} hop_xfer_status;
#endif
