#ifndef HOP_XFER_RX_CORE_H
#define HOP_XFER_RX_CORE_H

#include <stdint.h>
#include <wchar.h>
#include "hop.h"
#include "xfer_status.h"
#include "xfer_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- DONE Idempotency Receipt ----
 * Receiver-thread-owned receipt for one completed batch, valid only for the
 * control session that committed it. */
typedef struct {
    int valid;
    hop_session_id session;
    hop_xfer_done_msg done;
    uint8_t status;
} hop_xfer_rx_done_receipt;

void hop_xfer_rx_done_receipt_clear(hop_xfer_rx_done_receipt *receipt);
const hop_xfer_rx_done_receipt *hop_xfer_rx_done_receipt_find(
    const hop_xfer_rx_done_receipt *receipt,
    const hop_session_id *session, uint32_t batch);
int hop_xfer_rx_done_receipt_matches_payload(
    const hop_xfer_rx_done_receipt *receipt, const hop_xfer_done_msg *done);
void hop_xfer_rx_done_receipt_record(
    hop_xfer_rx_done_receipt *receipt, const hop_session_id *session,
    const hop_xfer_done_msg *done, uint8_t status);

/* ---- Session & Batch Tombstone Lifecycle ---- */
typedef struct {
    hop_session_id session;
    uint32_t dead_batch;
    uint8_t dead_reason;
} hop_xfer_rx_lifecycle;

void hop_xfer_rx_lifecycle_init(hop_xfer_rx_lifecycle *lifecycle);
const hop_session_id *hop_xfer_rx_lifecycle_session(
    const hop_xfer_rx_lifecycle *lifecycle);
/* Returns nonzero when the bound session changes; a change clears its tombstone. */
int hop_xfer_rx_lifecycle_bind_session(
    hop_xfer_rx_lifecycle *lifecycle, const hop_session_id *session);
void hop_xfer_rx_lifecycle_mark_dead(
    hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t reason);
int hop_xfer_rx_lifecycle_dead_reason(
    const hop_xfer_rx_lifecycle *lifecycle, uint32_t batch, uint8_t *reason);

/* ---- Batch Transition Rules ---- */
typedef enum {
    HOP_RX_BATCH_BEGIN_INVALID = 0,
    HOP_RX_BATCH_BEGIN_IGNORE_COMMITTED,
    HOP_RX_BATCH_BEGIN_REJECT_DEAD,
    HOP_RX_BATCH_BEGIN_RESUME,
    HOP_RX_BATCH_BEGIN_START
} hop_xfer_rx_batch_begin_action;

typedef enum {
    HOP_RX_BATCH_ABORT_IGNORE = 0,
    HOP_RX_BATCH_ABORT_REPLY_ONLY,
    HOP_RX_BATCH_ABORT_TERMINATE_ACTIVE
} hop_xfer_rx_batch_abort_action;

typedef enum {
    HOP_RX_BATCH_FINISH_STALE = 0,
    HOP_RX_BATCH_FINISH_CANCELLED,
    HOP_RX_BATCH_FINISH_COMPLETE,
    HOP_RX_BATCH_FINISH_PARTIAL,
    HOP_RX_BATCH_FINISH_IMAGE_FAILED
} hop_xfer_rx_batch_finish_action;

hop_xfer_rx_batch_begin_action hop_xfer_rx_batch_begin_decide(
    uint32_t batch, int committed, uint8_t dead_reason,
    int active, uint32_t active_batch, uint8_t *reason);
hop_xfer_rx_batch_abort_action hop_xfer_rx_batch_abort_decide(
    uint32_t batch, int active, uint32_t active_batch, int notify_peer);
int hop_xfer_rx_batch_is_active(uint32_t batch, int active, uint32_t active_batch);
hop_xfer_rx_batch_finish_action hop_xfer_rx_batch_finish_decide(
    uint32_t batch, int active, uint32_t active_batch,
    int completion_claimed, uint32_t ok, uint32_t total,
    int is_image, int image_valid);

/* ---- Active Batch Identity ---- */
typedef struct {
    uint32_t batch;
    uint32_t generation;
    int active;
} hop_xfer_rx_batch_state;

void hop_xfer_rx_batch_state_init(hop_xfer_rx_batch_state *state);
int hop_xfer_rx_batch_state_begin(hop_xfer_rx_batch_state *state, uint32_t batch);
int hop_xfer_rx_batch_state_end(hop_xfer_rx_batch_state *state, uint32_t batch);
void hop_xfer_rx_batch_state_reset(hop_xfer_rx_batch_state *state);
int hop_xfer_rx_batch_state_matches(const hop_xfer_rx_batch_state *state, uint32_t batch);

/* ---- Entry Budget Reservations ---- */
typedef struct {
    uint8_t reserved[HOP_XFER_FILE_LIMIT];
    uint64_t sizes[HOP_XFER_FILE_LIMIT];
} hop_xfer_rx_entry_reservations;

typedef enum {
    HOP_RX_ENTRY_RESERVE_INVALID = 0,
    HOP_RX_ENTRY_RESERVE_NEW,
    HOP_RX_ENTRY_RESERVE_ALREADY_HELD,
    HOP_RX_ENTRY_RESERVE_SIZE_CONFLICT,
    HOP_RX_ENTRY_RESERVE_BUDGET_REJECTED
} hop_xfer_rx_entry_reserve_result;

typedef int (*hop_xfer_rx_entry_budget_reserve_fn)(void *context, uint64_t bytes);

void hop_xfer_rx_entry_reservations_reset(hop_xfer_rx_entry_reservations *entries);
hop_xfer_rx_entry_reserve_result hop_xfer_rx_entry_reservations_reserve(
    hop_xfer_rx_entry_reservations *entries, uint32_t id, uint64_t size,
    hop_xfer_rx_entry_budget_reserve_fn reserve_budget, void *context);

/* ---- Active File State ---- */
typedef struct {
    uint32_t id;
    int is_image;
    int is_directory;
    int failure; /* -1 while valid; otherwise FILE_ACK status */
    uint64_t declared_size;
    uint64_t received;
    uint32_t crc;
} hop_xfer_rx_file_state;

typedef enum {
    HOP_RX_FILE_CHUNK_INVALID = 0,
    HOP_RX_FILE_CHUNK_ACCEPT,
    HOP_RX_FILE_CHUNK_ALREADY_FAILED
} hop_xfer_rx_file_chunk_check;

typedef enum {
    HOP_RX_FILE_END_STORED_FAILURE = 0,
    HOP_RX_FILE_END_WRITER_ERROR,
    HOP_RX_FILE_END_PEER_REJECTED,
    HOP_RX_FILE_END_INCOMPLETE,
    HOP_RX_FILE_END_CRC_MISMATCH,
    HOP_RX_FILE_END_SUCCESS
} hop_xfer_rx_file_end_reason;

typedef struct {
    uint8_t ack_status;
    hop_xfer_rx_file_end_reason reason;
} hop_xfer_rx_file_end_result;

void hop_xfer_rx_file_state_reset(hop_xfer_rx_file_state *state);
void hop_xfer_rx_file_state_begin(hop_xfer_rx_file_state *state, uint32_t id);
void hop_xfer_rx_file_state_set_kind(hop_xfer_rx_file_state *state, int is_image, int is_directory);
void hop_xfer_rx_file_state_set_size(hop_xfer_rx_file_state *state, uint64_t size);
int hop_xfer_rx_file_state_prepare_entry(hop_xfer_rx_file_state *state,
    uint32_t id, uint64_t size, int is_directory,
    const uint8_t *relative_path, uint32_t relative_path_length);
void hop_xfer_rx_file_state_fail(hop_xfer_rx_file_state *state, uint8_t ack_status);
int hop_xfer_rx_file_state_matches(const hop_xfer_rx_file_state *state, uint32_t id);
hop_xfer_rx_file_chunk_check hop_xfer_rx_file_state_check_chunk(
    const hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length);
int hop_xfer_rx_file_state_record_chunk(
    hop_xfer_rx_file_state *state, uint64_t offset, uint32_t length,
    uint32_t chunk_crc);
hop_xfer_rx_file_end_result hop_xfer_rx_file_state_evaluate_end(
    const hop_xfer_rx_file_state *state, uint8_t peer_status,
    int writer_error, uint32_t expected_file_crc);
uint8_t hop_xfer_rx_file_state_ack(const hop_xfer_rx_file_state *state,
                                   uint8_t peer_status, int writer_error,
                                   uint32_t expected_file_crc);

/* ---- Active Image State & Buffer ---- */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t expected;
    uint64_t received;
    uint64_t capacity;
    uint32_t crc;
} hop_xfer_rx_image_state;

typedef enum {
    HOP_RX_IMAGE_CHUNK_INVALID = 0,
    HOP_RX_IMAGE_CHUNK_ACCEPT
} hop_xfer_rx_image_chunk_check;

void hop_xfer_rx_image_state_reset(hop_xfer_rx_image_state *state);
int hop_xfer_rx_image_state_begin(hop_xfer_rx_image_state *state,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint64_t expected);
void hop_xfer_rx_image_state_set_capacity(hop_xfer_rx_image_state *state,
                                          uint64_t capacity);
hop_xfer_rx_image_chunk_check hop_xfer_rx_image_state_check_chunk(
    const hop_xfer_rx_image_state *state, uint64_t offset, uint32_t length);
int hop_xfer_rx_image_state_record_chunk(hop_xfer_rx_image_state *state,
                                         uint64_t offset, uint32_t length);

typedef struct {
    uint8_t *data;
    uint64_t capacity;
} hop_xfer_rx_image_buffer;

int hop_xfer_rx_image_buffer_ensure(hop_xfer_rx_image_buffer *buffer, uint64_t expected);
void hop_xfer_rx_image_buffer_dispose(hop_xfer_rx_image_buffer *buffer);

/* ---- Frame Gate ---- */
typedef enum {
    HOP_RX_FRAME_DROP = 0,
    HOP_RX_FRAME_REJECT_PEER = 1,
    HOP_RX_FRAME_HELLO_ACCEPT = 2,
    HOP_RX_FRAME_HELLO_REJECT = 3,
    HOP_RX_FRAME_DISPATCH = 4
} hop_rx_frame_action;

typedef struct {
    hop_session_id expected_session;
    uint32_t active_batch; /* 0 when no receive batch is active */
} hop_rx_frame_gate_input;

typedef struct {
    hop_rx_frame_action action;
    uint32_t batch;
    const uint8_t *body; /* points into the caller's payload */
    uint32_t body_length;
} hop_rx_frame_gate_result;

hop_rx_frame_gate_result hop_xfer_rx_gate_frame(
    const hop_rx_frame_gate_input *input,
    const char *token,
    uint8_t type,
    const uint8_t *payload,
    uint32_t payload_length);

/* ---- Message Dispatch ---- */
typedef enum {
    HOP_RX_DISPATCH_UNKNOWN = 0,
    HOP_RX_DISPATCH_FILE_BEGIN,
    HOP_RX_DISPATCH_IMAGE_BEGIN,
    HOP_RX_DISPATCH_FILE_CHUNK,
    HOP_RX_DISPATCH_FILE_END,
    HOP_RX_DISPATCH_FILE_ACK,
    HOP_RX_DISPATCH_ABORT,
    HOP_RX_DISPATCH_DONE,
    HOP_RX_DISPATCH_DONE_ACK
} hop_xfer_rx_dispatch_action;

typedef void (*hop_xfer_rx_dispatch_handler)(
    void *context, const uint8_t *payload, uint32_t payload_length);

typedef struct {
    hop_xfer_rx_dispatch_handler file_begin;
    hop_xfer_rx_dispatch_handler image_begin;
    hop_xfer_rx_dispatch_handler file_chunk;
    hop_xfer_rx_dispatch_handler file_end;
    hop_xfer_rx_dispatch_handler file_ack;
    hop_xfer_rx_dispatch_handler abort_message;
    hop_xfer_rx_dispatch_handler done;
    hop_xfer_rx_dispatch_handler done_ack;
} hop_xfer_rx_dispatch_handlers;

hop_xfer_rx_dispatch_action hop_xfer_rx_dispatch_classify(uint8_t message_type);
int hop_xfer_rx_dispatch_message(
    uint8_t message_type, const uint8_t *payload, uint32_t payload_length,
    const hop_xfer_rx_dispatch_handlers *handlers, void *context);

/* ---- Inbound ACK Payload Validation ---- */
typedef void (*hop_xfer_rx_file_ack_callback)(
    void *context, uint32_t batch, uint32_t file_id, uint8_t status);
typedef void (*hop_xfer_rx_done_ack_callback)(
    void *context, uint32_t batch, uint8_t status);

int hop_xfer_rx_handle_file_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_file_ack_callback callback, void *context);
int hop_xfer_rx_handle_done_ack(
    const uint8_t *payload, uint32_t payload_length,
    hop_xfer_rx_done_ack_callback callback, void *context);

/* ---- Platform Services & Receiver Context ---- */
typedef struct {
    void *context;
    void (*shutdown_data_channel)(void *context);
    void (*mark_sender_aborted)(void *context, uint32_t batch, uint8_t reason);
    void (*send_abort)(void *context, uint32_t batch, uint8_t reason);
    void (*clear_cancel_request)(void *context);
    int (*create_staging_directory)(void *context, const char *session_hex, uint32_t batch);
    int (*discard_staging_directory)(void *context);
    void (*send_file_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*send_done_ack)(void *context, uint32_t batch,
                          const uint8_t *payload, uint32_t length);
    void (*deliver_ack)(void *context, uint32_t batch, uint32_t id, int status);
    void (*reset_file_error)(void *context);
    int (*open_file)(void *context, const void *native_path);
    int (*write_file_chunk)(void *context, hop_xfer_task *task,
                            uint64_t offset, const uint8_t *data, uint32_t length);
    void (*close_file)(void *context);
    int (*file_error)(void *context);
    void *(*build_path)(void *context, const char *relative_path, uint32_t length);
    int (*prepare_file_path)(void *context, const void *native_path, int is_directory);
    int (*is_top_level_path)(void *context, const void *native_path);
    int (*remove_file)(void *context, const void *native_path);
    int (*add_top_path)(void *context, const void *native_path);
    void (*remove_top_path)(void *context, const void *native_path);
    void (*finish_clipboard)(void *context, uint32_t batch,
                             const void *image_data, uint32_t width,
                             uint32_t height, uint32_t stride, int image_valid,
                             const void *const *top_paths, int top_count, int allow_publish);
    void (*cancel_clipboard)(void *context, uint32_t batch);
    uint64_t (*now_ns)(void *context);
    int (*status_begin)(void *context, uint32_t batch, uint32_t files,
                        uint64_t bytes, const char *name, int phase);
    void (*status_end)(void *context, uint32_t batch, int phase, const char *detail);
    void (*status_add_received_file)(void *context, uint32_t batch, uint32_t id,
                                     const char *path, uint64_t size, int directory);
    void (*status_image)(void *context, uint32_t batch, uint64_t size);
    void (*status_stage_time)(void *context, uint32_t batch, int stage, uint64_t elapsed_ns);
    void (*status_chunk_received)(void *context, uint32_t batch, uint32_t id,
                                  uint64_t offset, uint32_t length,
                                  uint64_t crc_ns, uint64_t receive_ns);
    void (*status_file_phase)(void *context, uint32_t batch, int index,
                              int phase, uint32_t attempts);
    void (*status_set_files_total)(void *context, uint32_t total);
    void (*status_reset)(void *context);
} hop_xfer_rx_services;

#if defined(_WIN32) || defined(FIXTURE_WINDOWS)
#define HOP_XFER_RX_CONTEXT_PATH_CAPACITY 260
typedef wchar_t hop_xfer_rx_path_char;
#else
#define HOP_XFER_RX_CONTEXT_PATH_CAPACITY 4096
typedef char hop_xfer_rx_path_char;
#endif

typedef struct xfer_rx_context {
    hop_xfer_rx_batch_state batch_state;
    hop_xfer_task task;
    hop_xfer_rx_entry_reservations entry_reservations;
    hop_xfer_rx_path_char dir[HOP_XFER_RX_CONTEXT_PATH_CAPACITY];
    int staging_cleanup_pending;
    hop_xfer_rx_path_char *cur_path;
    hop_xfer_rx_file_state file_state;
    hop_xfer_rx_image_buffer image_buffer;
    hop_xfer_rx_image_state image_state;
    hop_xfer_rx_path_char **tops;
    int ntop;
    void *writer;
    hop_xfer_rx_lifecycle *lifecycle;
    hop_xfer_rx_done_receipt *done_receipt;
    hop_xfer_rx_services services;
} xfer_rx_context;

void hop_xfer_rx_configure(const char *rx_dir, uint64_t disconnect_budget_ms);
const char *hop_xfer_rx_configured_dir(void);
uint64_t hop_xfer_rx_disconnect_budget_ms(void);

#ifdef __cplusplus
}
#endif
#endif
