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
    uint32_t queued_count;
    uint64_t bytes_done;
    uint64_t bytes_total;
    uint64_t speed_bytes_per_sec;
    uint64_t last_progress_ms;
    uint64_t eta_seconds;
    char name[HOP_XFER_STATUS_NAME];
    char detail[HOP_XFER_STATUS_DETAIL];
} hop_xfer_status;

#define HOP_XFER_FILE_LIMIT 8192
typedef enum {
    HOP_XFER_FILE_QUEUED = 0,
    HOP_XFER_FILE_SENDING,
    HOP_XFER_FILE_RECEIVING,
    HOP_XFER_FILE_VERIFYING,
    HOP_XFER_FILE_COMPLETED,
    HOP_XFER_FILE_FAILED
} hop_xfer_file_phase;
typedef struct {
    uint32_t index;
    int phase;
    int is_directory;
    uint32_t attempts;
    uint64_t bytes_done;
    uint64_t bytes_total;
    char relative_path[1024];
} hop_xfer_file_status;

#define HOP_XFER_HISTORY_LIMIT 100
typedef struct {
    uint32_t batch;
    int direction;
    int phase;
    uint32_t files_done;
    uint32_t files_total;
    uint64_t bytes_done;
    uint64_t bytes_total;
    uint64_t finished_at;
    char name[HOP_XFER_STATUS_NAME];
    char detail[HOP_XFER_STATUS_DETAIL];
} hop_xfer_history_entry;
#endif
