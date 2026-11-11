#include "xfer_buffer_pool.h"

#include <stdlib.h>
#include <string.h>

void hop_xfer_buffer_pool_init(hop_xfer_buffer_pool *pool) {
    if (pool) memset(pool, 0, sizeof(*pool));
}

/* Caller holds the writer-queue lock. */
int hop_xfer_buffer_pool_acquire_locked(hop_xfer_buffer_pool *pool,
                                        uint32_t len, int *slot_out,
                                        uint8_t **data_out) {
    if (!pool || !len || !slot_out || !data_out ||
        len > HOP_XFER_DEFAULT_RX_QUEUE_BYTES) return 0;

    int chosen = -1;
    for (int i = 0; i < (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS; ++i) {
        hop_xfer_buffer_slot *slot = &pool->slots[i];
        if (slot->in_use || !slot->data || slot->capacity < len) continue;
        if (chosen < 0 || slot->capacity < pool->slots[chosen].capacity)
            chosen = i;
    }
    if (chosen < 0) {
        for (int i = 0; i < (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS; ++i) {
            hop_xfer_buffer_slot *slot = &pool->slots[i];
            if (slot->in_use) continue;
            if (chosen < 0 || slot->capacity < pool->slots[chosen].capacity)
                chosen = i;
        }
    }
    if (chosen < 0) return 0;

    hop_xfer_buffer_slot *target = &pool->slots[chosen];
    if (target->capacity < len) {
        for (int i = 0; i < (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS; ++i) {
            hop_xfer_buffer_slot *slot = &pool->slots[i];
            if (i == chosen || slot->in_use || !slot->data) continue;
            free(slot->data);
            pool->capacity -= slot->capacity;
            slot->data = NULL;
            slot->capacity = 0;
        }
        (void)hop_xfer_budget_set_rx_pool_bytes(pool->capacity);

        if (pool->capacity - target->capacity + len >
            HOP_XFER_DEFAULT_RX_QUEUE_BYTES)
            return 0;

        if (target->data) {
            free(target->data);
            pool->capacity -= target->capacity;
            target->data = NULL;
            target->capacity = 0;
            (void)hop_xfer_budget_set_rx_pool_bytes(pool->capacity);
        }

        target->data = (uint8_t *)malloc(len);
        if (!target->data) return -1;
        target->capacity = len;
        pool->capacity += len;
        (void)hop_xfer_budget_set_rx_pool_bytes(pool->capacity);
    }

    target->in_use = 1;
    *slot_out = chosen;
    *data_out = target->data;
    return 1;
}

void hop_xfer_buffer_pool_release_locked(hop_xfer_buffer_pool *pool, int slot) {
    if (!pool || slot < 0 || slot >= (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS) return;
    if (!pool->slots[slot].in_use) return;
    pool->slots[slot].in_use = 0;
}

void hop_xfer_buffer_pool_dispose(hop_xfer_buffer_pool *pool) {
    if (!pool) return;
    for (int i = 0; i < (int)HOP_XFER_DEFAULT_RX_QUEUE_SLOTS; ++i) {
        free(pool->slots[i].data);
        pool->slots[i].data = NULL;
        pool->slots[i].capacity = 0;
        pool->slots[i].in_use = 0;
    }
    pool->capacity = 0;
    (void)hop_xfer_budget_set_rx_pool_bytes(0);
}
