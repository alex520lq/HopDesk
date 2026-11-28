#ifndef HOP_UPDATE_CONTROLLER_H
#define HOP_UPDATE_CONTROLLER_H

#include <stdint.h>
#include "update_manifest.h"

typedef enum {
    HOP_UPDATE_IDLE        = 0,
    HOP_UPDATE_CHECKING    = 1,
    HOP_UPDATE_UP_TO_DATE  = 2,
    HOP_UPDATE_AVAILABLE   = 3,
    HOP_UPDATE_DOWNLOADING = 4,
    HOP_UPDATE_READY       = 5,
    HOP_UPDATE_APPLYING    = 6,
    HOP_UPDATE_ERROR       = 7
} hop_update_phase;

enum {
    HOP_UPDATE_ACTION_CHECK    = 1,
    HOP_UPDATE_ACTION_DOWNLOAD = 2,
    HOP_UPDATE_ACTION_APPLY    = 3
};

typedef struct {
    int32_t  phase;
    int32_t  target_channel;    /* HOP_UPDATE_CHANNEL_STABLE or HOP_UPDATE_CHANNEL_DEV */
    int32_t  is_channel_switch; /* 1 when switching between stable and dev */
    int32_t  protocol_changed;  /* 1 when target ctrl_proto or xfer_proto differs */
    int32_t  target_ctrl_proto;
    int32_t  target_xfer_proto;
    uint64_t bytes_done;
    uint64_t bytes_total;
    char     target_version[HOP_UPDATE_VERSION_MAX];
    char     target_tag[HOP_UPDATE_TAG_MAX];
    char     notes_url[HOP_UPDATE_URL_MAX];
    char     summary[HOP_UPDATE_SUMMARY_MAX];
    char     message[256];
} hop_update_snapshot;

void hop_update_snapshot_init(hop_update_snapshot *s, int target_channel);
void hop_update_snapshot_set_checking(hop_update_snapshot *s, int target_channel);
void hop_update_snapshot_set_manifest_result(hop_update_snapshot *s,
                                             const char *current_version,
                                             int current_channel,
                                             int current_ctrl_proto,
                                             int current_xfer_proto,
                                             int target_channel,
                                             const hop_update_manifest *manifest);
void hop_update_snapshot_set_downloading(hop_update_snapshot *s,
                                         uint64_t bytes_done,
                                         uint64_t bytes_total);
void hop_update_snapshot_set_ready(hop_update_snapshot *s, uint64_t total_bytes);
void hop_update_snapshot_set_applying(hop_update_snapshot *s);
void hop_update_snapshot_set_error(hop_update_snapshot *s,
                                   int target_channel,
                                   const char *message);

#endif
