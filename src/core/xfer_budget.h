#ifndef HOP_XFER_BUDGET_H
#define HOP_XFER_BUDGET_H
#include <stdint.h>

#define HOP_XFER_DEFAULT_RX_QUEUE_SLOTS 8u
#define HOP_XFER_DEFAULT_RX_QUEUE_BYTES (8ull * 1024ull * 1024ull)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t max_jobs, max_entries;
    uint64_t max_pending_bytes, max_image_bytes, max_single_image_bytes;
    uint32_t max_rx_queue_slots;
    uint64_t max_rx_queue_bytes;
} hop_xfer_budget_limits;

typedef struct {
    uint32_t jobs, entries, rx_queue_slots;
    uint64_t pending_bytes, image_bytes, rx_queue_bytes;
    uint32_t jobs_high_water, entries_high_water, rx_queue_slots_high_water;
    uint64_t pending_bytes_high_water, image_bytes_high_water, rx_queue_bytes_high_water;
    uint64_t rejected_jobs, rejected_entries, rejected_bytes, rejected_images;
    uint64_t rejected_rx_buffers;
    uint64_t released_reservations, released_rx_buffers, accounting_errors;
} hop_xfer_budget_snapshot;

typedef struct {
    uint64_t task_id;
    uint64_t handle_id;
    uint32_t jobs, entries, rx_queue_slots;
    uint64_t pending_bytes, image_bytes, rx_queue_bytes;
    int active;
} hop_xfer_reservation_handle;

int hop_xfer_budget_reservation_init(hop_xfer_reservation_handle*, uint64_t task_id);
int hop_xfer_budget_reserve_job(hop_xfer_reservation_handle*);
int hop_xfer_budget_reserve_entry(hop_xfer_reservation_handle*, uint64_t bytes);
int hop_xfer_budget_reserve_image(hop_xfer_reservation_handle*, uint64_t bytes);
int hop_xfer_budget_reserve_rx_buffer(hop_xfer_reservation_handle*, uint64_t bytes);
int hop_xfer_budget_release_rx_buffer(hop_xfer_reservation_handle*, uint64_t bytes);
void hop_xfer_budget_release(hop_xfer_reservation_handle*);
void hop_xfer_budget_get_snapshot(hop_xfer_budget_snapshot*);
int hop_xfer_budget_set_limits_for_test(const hop_xfer_budget_limits*);

#ifdef __cplusplus
}
#endif
#endif
