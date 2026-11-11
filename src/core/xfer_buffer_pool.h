#ifndef HOP_XFER_BUFFER_POOL_H
#define HOP_XFER_BUFFER_POOL_H

#include <stdint.h>
#include "xfer_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    uint32_t capacity;
    int in_use;
} hop_xfer_buffer_slot;

typedef struct {
    hop_xfer_buffer_slot slots[HOP_XFER_DEFAULT_RX_QUEUE_SLOTS];
    uint64_t capacity;
} hop_xfer_buffer_pool;

/* The caller must serialize every operation with its writer-queue lock.
 * acquire returns 1 on success, 0 when temporarily unavailable/over budget,
 * and -1 on allocation failure. The returned slot remains pinned until release. */
void hop_xfer_buffer_pool_init(hop_xfer_buffer_pool *pool);
int hop_xfer_buffer_pool_acquire_locked(hop_xfer_buffer_pool *pool,
                                        uint32_t len, int *slot_out,
                                        uint8_t **data_out);
void hop_xfer_buffer_pool_release_locked(hop_xfer_buffer_pool *pool, int slot);
/* Call only after the writer queue has drained and all slots have been released. */
void hop_xfer_buffer_pool_dispose(hop_xfer_buffer_pool *pool);

#ifdef __cplusplus
}
#endif
#endif
