#ifndef HOPDESK_MAC_SPLIT_IPC_H
#define HOPDESK_MAC_SPLIT_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include "../core/xfer_task.h"
#include "../core/xfer_task_history.h"
#include "../core/xfer_status.h"
#include "../core/update_controller.h"
#include "xfer.h"

#define SPLIT_IPC_VERSION 8u
#define SPLIT_IPC_ERROR 0x80000000u
#define SPLIT_IPC_TIMEOUT_MS 200u
#define SPLIT_IPC_MAX_PAYLOAD (16u * 1024u * 1024u)

typedef struct {
    int32_t pid, control, data, sharing, post_permission, listen_permission;
    int32_t accessibility, busy;
    int32_t audio_requested, audio_active, audio_unavailable;
    int32_t disconnected;
    int32_t local_input, peer_input, peer_audio, peer_caps_known;
    int32_t protocol_mismatch;
    char peer_name[HOP_NAME_MAX];
    char display_status[192];
    hop_update_snapshot update;
} split_ipc_status;
typedef struct {
    uint64_t id;
    int32_t capacity, direction, state, newest, index;
    uint32_t batch;
    int32_t offset;
} split_ipc_query;
typedef struct { int32_t count, total; uint64_t revision; } split_ipc_list;
typedef struct {
    hop_xfer_status_pair status;
    uint64_t task_revision, history_revision;
} split_ipc_transfer_state;
typedef struct { int32_t result; } split_ipc_result;

static inline int split_ipc_socket_path(char *out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, out, capacity);
    if (!n || n >= capacity) return 0;
    static const char suffix[] = "hopdesk-agent.sock";
    size_t used = n - 1;
    if (used + sizeof(suffix) > capacity) return 0;
    memcpy(out + used, suffix, sizeof(suffix));
    return 1;
}

typedef enum {
    SPLIT_IPC_STATUS = 1,
    SPLIT_IPC_CONFIG_GET,
    SPLIT_IPC_XFER_STATUS,
    SPLIT_IPC_TASKS_GET,
    SPLIT_IPC_TASK_GET,
    SPLIT_IPC_HISTORY_GET,
    SPLIT_IPC_FILES_GET,
    SPLIT_IPC_RECONNECT,
    SPLIT_IPC_RETRY_FILE,
    SPLIT_IPC_CANCEL_TASK,
    SPLIT_IPC_STOP,
    SPLIT_IPC_AUDIO_MODE_SET,
    SPLIT_IPC_DISCONNECT,
    SPLIT_IPC_INPUT_MODE_SET,
    SPLIT_IPC_CONFIG_SAVE,
    SPLIT_IPC_CONFIG_RELOAD,
    SPLIT_IPC_DISPLAY_ACTION,
    SPLIT_IPC_UPDATE_ACTION
} split_ipc_operation;

/* The fixed-size header is in network byte order. Payloads are native structs
 * shared by the two modes of this bundle. Bump the version on a layout change. */
typedef struct {
    uint32_t version;
    uint32_t operation;
    uint32_t payload_size;
    uint32_t request_id;
} split_ipc_header;

/* Replies use the same header; operation has high bit set on errors. */
typedef struct {
    uint32_t version;
    uint32_t operation;
    uint32_t payload_size;
    uint32_t request_id;
} split_ipc_reply;

/* The server uses the per-user Darwin temp directory and
 * "hopdesk-agent.sock". Clients must use the same discovery rule. */
#endif
