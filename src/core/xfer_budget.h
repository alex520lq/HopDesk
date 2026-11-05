#ifndef HOP_XFER_BUDGET_H
#define HOP_XFER_BUDGET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t max_jobs,max_entries; uint64_t max_pending_bytes,max_image_bytes,max_single_image_bytes; } hop_xfer_budget_limits;
typedef struct {
 uint32_t jobs,entries; uint64_t pending_bytes,image_bytes;
 uint32_t jobs_high_water,entries_high_water;
 uint64_t pending_bytes_high_water,image_bytes_high_water;
 uint64_t rejected_jobs,rejected_entries,rejected_bytes,rejected_images;
} hop_xfer_budget_snapshot;
typedef struct {
 uint64_t task_id;
 uint64_t handle_id;
 uint32_t jobs,entries;
 uint64_t pending_bytes,image_bytes;
 int active;
} hop_xfer_reservation_handle;
int hop_xfer_budget_reservation_init(hop_xfer_reservation_handle*,uint64_t task_id);
int hop_xfer_budget_reserve_job(hop_xfer_reservation_handle*);
int hop_xfer_budget_reserve_entry(hop_xfer_reservation_handle*,uint64_t);
int hop_xfer_budget_reserve_image(hop_xfer_reservation_handle*,uint64_t);
void hop_xfer_budget_release(hop_xfer_reservation_handle*);
void hop_xfer_budget_get_snapshot(hop_xfer_budget_snapshot*);
int hop_xfer_budget_set_limits_for_test(const hop_xfer_budget_limits*);
#ifdef __cplusplus
}
#endif
#endif
