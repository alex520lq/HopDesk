#include "xfer_budget.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_budget_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_budget_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_budget_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_budget_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_budget_lock)
#define UNLOCK() pthread_mutex_unlock(&s_budget_lock)
#endif

#define DEFAULT_MAX_JOBS 16u
#define DEFAULT_MAX_ENTRIES 32768u
#define DEFAULT_MAX_PENDING_BYTES (8ull * 1024ull * 1024ull * 1024ull)
#define DEFAULT_MAX_IMAGE_BYTES (256ull * 1024ull * 1024ull)
#define DEFAULT_MAX_SINGLE_IMAGE_BYTES (256ull * 1024ull * 1024ull)
#define DEFAULT_MAX_IMAGE_TEMP_BYTES (512ull * 1024ull * 1024ull)

static hop_xfer_budget_limits s_limits = {
    DEFAULT_MAX_JOBS, DEFAULT_MAX_ENTRIES,
    DEFAULT_MAX_PENDING_BYTES, DEFAULT_MAX_IMAGE_BYTES, DEFAULT_MAX_SINGLE_IMAGE_BYTES,
    HOP_XFER_DEFAULT_RX_QUEUE_SLOTS, HOP_XFER_DEFAULT_RX_QUEUE_BYTES,
    DEFAULT_MAX_IMAGE_TEMP_BYTES
};
static hop_xfer_budget_snapshot s_used;
static uint64_t s_next_handle_id;

static int add64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out || b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static void subtract_u32(uint32_t *value, uint32_t amount) {
    if (amount > *value) {
        s_used.accounting_errors++;
        *value = 0;
    } else {
        *value -= amount;
    }
}

static void subtract_u64(uint64_t *value, uint64_t amount) {
    if (amount > *value) {
        s_used.accounting_errors++;
        *value = 0;
    } else {
        *value -= amount;
    }
}

int hop_xfer_budget_reservation_init(hop_xfer_reservation_handle *r, uint64_t task_id) {
    if (!r || !task_id) return 0;
    memset(r, 0, sizeof(*r));
    LOCK();
    r->handle_id = ++s_next_handle_id;
    if (!r->handle_id) r->handle_id = ++s_next_handle_id;
    r->task_id = task_id;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_job(hop_xfer_reservation_handle *r) {
    if (!r || !r->task_id || !r->handle_id || r->active) return 0;
    LOCK();
    if (s_used.jobs >= s_limits.max_jobs || s_used.jobs == UINT32_MAX) {
        s_used.rejected_jobs++;
        UNLOCK();
        return 0;
    }
    s_used.jobs++;
    if (s_used.jobs > s_used.jobs_high_water) s_used.jobs_high_water = s_used.jobs;
    r->jobs = 1;
    r->active = 1;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_entry(hop_xfer_reservation_handle *r, uint64_t bytes) {
    uint64_t next;
    if (!r || !r->task_id || !r->handle_id || !r->active) return 0;
    LOCK();
    if (s_used.entries >= s_limits.max_entries || s_used.entries == UINT32_MAX) {
        s_used.rejected_entries++;
        UNLOCK();
        return 0;
    }
    if (!add64(s_used.pending_bytes, bytes, &next) ||
        next > s_limits.max_pending_bytes ||
        bytes > UINT64_MAX - r->pending_bytes ||
        r->entries == UINT32_MAX) {
        s_used.rejected_bytes++;
        UNLOCK();
        return 0;
    }
    s_used.entries++;
    s_used.pending_bytes = next;
    if (s_used.entries > s_used.entries_high_water) s_used.entries_high_water = s_used.entries;
    if (next > s_used.pending_bytes_high_water) s_used.pending_bytes_high_water = next;
    r->entries++;
    r->pending_bytes += bytes;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_image(hop_xfer_reservation_handle *r, uint64_t bytes) {
    uint64_t pending, image;
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (bytes > s_limits.max_single_image_bytes ||
        s_used.entries >= s_limits.max_entries || s_used.entries == UINT32_MAX) {
        s_used.rejected_images++;
        UNLOCK();
        return 0;
    }
    if (!add64(s_used.pending_bytes, bytes, &pending) ||
        pending > s_limits.max_pending_bytes ||
        !add64(s_used.image_bytes, bytes, &image) ||
        image > s_limits.max_image_bytes ||
        bytes > UINT64_MAX - r->pending_bytes ||
        bytes > UINT64_MAX - r->image_bytes ||
        r->entries == UINT32_MAX) {
        s_used.rejected_images++;
        UNLOCK();
        return 0;
    }
    s_used.entries++;
    s_used.pending_bytes = pending;
    s_used.image_bytes = image;
    if (s_used.entries > s_used.entries_high_water) s_used.entries_high_water = s_used.entries;
    if (pending > s_used.pending_bytes_high_water) s_used.pending_bytes_high_water = pending;
    if (image > s_used.image_bytes_high_water) s_used.image_bytes_high_water = image;
    r->entries++;
    r->pending_bytes += bytes;
    r->image_bytes += bytes;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_temp_image(hop_xfer_reservation_handle *r, uint64_t bytes) {
    uint64_t pending, image, temporary;
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (!add64(s_used.pending_bytes, bytes, &pending) ||
        pending > s_limits.max_pending_bytes ||
        !add64(s_used.image_bytes, bytes, &image) ||
        image > s_limits.max_image_bytes ||
        !add64(s_used.temp_image_bytes, bytes, &temporary) ||
        bytes > UINT64_MAX - r->pending_bytes ||
        bytes > UINT64_MAX - r->image_bytes ||
        bytes > UINT64_MAX - r->temp_image_bytes) {
        s_used.rejected_temp_images++;
        UNLOCK();
        return 0;
    }
    s_used.pending_bytes = pending;
    s_used.image_bytes = image;
    s_used.temp_image_bytes = temporary;
    if (pending > s_used.pending_bytes_high_water) s_used.pending_bytes_high_water = pending;
    if (image > s_used.image_bytes_high_water) s_used.image_bytes_high_water = image;
    if (temporary > s_used.temp_image_bytes_high_water)
        s_used.temp_image_bytes_high_water = temporary;
    r->pending_bytes += bytes;
    r->image_bytes += bytes;
    r->temp_image_bytes += bytes;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_release_temp_image(hop_xfer_reservation_handle *r, uint64_t bytes) {
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (bytes > r->temp_image_bytes || bytes > r->pending_bytes ||
        bytes > r->image_bytes || bytes > s_used.temp_image_bytes ||
        bytes > s_used.pending_bytes || bytes > s_used.image_bytes) {
        s_used.accounting_errors++;
        UNLOCK();
        return 0;
    }
    r->temp_image_bytes -= bytes;
    r->pending_bytes -= bytes;
    r->image_bytes -= bytes;
    s_used.temp_image_bytes -= bytes;
    s_used.pending_bytes -= bytes;
    s_used.image_bytes -= bytes;
    s_used.released_temp_buffers++;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_rx_buffer(hop_xfer_reservation_handle *r, uint64_t bytes) {
    uint64_t next;
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (s_used.rx_queue_slots >= s_limits.max_rx_queue_slots ||
        s_used.rx_queue_slots == UINT32_MAX ||
        !add64(s_used.rx_queue_bytes, bytes, &next) ||
        next > s_limits.max_rx_queue_bytes ||
        r->rx_queue_slots == UINT32_MAX ||
        bytes > UINT64_MAX - r->rx_queue_bytes) {
        s_used.rejected_rx_buffers++;
        UNLOCK();
        return 0;
    }
    s_used.rx_queue_slots++;
    s_used.rx_queue_bytes = next;
    if (s_used.rx_queue_slots > s_used.rx_queue_slots_high_water)
        s_used.rx_queue_slots_high_water = s_used.rx_queue_slots;
    if (next > s_used.rx_queue_bytes_high_water) s_used.rx_queue_bytes_high_water = next;
    r->rx_queue_slots++;
    r->rx_queue_bytes += bytes;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_release_rx_buffer(hop_xfer_reservation_handle *r, uint64_t bytes) {
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (!r->rx_queue_slots || bytes > r->rx_queue_bytes ||
        !s_used.rx_queue_slots || bytes > s_used.rx_queue_bytes) {
        s_used.accounting_errors++;
        UNLOCK();
        return 0;
    }
    r->rx_queue_slots--;
    r->rx_queue_bytes -= bytes;
    s_used.rx_queue_slots--;
    s_used.rx_queue_bytes -= bytes;
    s_used.released_rx_buffers++;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_reserve_image_temp(hop_xfer_reservation_handle *r, uint64_t bytes) {
    uint64_t next;
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (!add64(s_used.image_temp_bytes, bytes, &next) ||
        next > s_limits.max_image_temp_bytes ||
        bytes > UINT64_MAX - r->image_temp_bytes) {
        s_used.rejected_image_temp_bytes++;
        UNLOCK();
        return 0;
    }
    s_used.image_temp_bytes = next;
    if (next > s_used.image_temp_bytes_high_water)
        s_used.image_temp_bytes_high_water = next;
    r->image_temp_bytes += bytes;
    UNLOCK();
    return 1;
}

int hop_xfer_budget_release_image_temp(hop_xfer_reservation_handle *r, uint64_t bytes) {
    if (!r || !r->task_id || !r->handle_id || !r->active || !bytes) return 0;
    LOCK();
    if (bytes > r->image_temp_bytes || bytes > s_used.image_temp_bytes) {
        s_used.accounting_errors++;
        UNLOCK();
        return 0;
    }
    r->image_temp_bytes -= bytes;
    s_used.image_temp_bytes -= bytes;
    if (bytes > UINT64_MAX - s_used.released_image_temp_bytes) {
        s_used.accounting_errors++;
    } else {
        s_used.released_image_temp_bytes += bytes;
    }
    UNLOCK();
    return 1;
}

void hop_xfer_budget_release(hop_xfer_reservation_handle *r) {
    if (!r || !r->task_id || !r->handle_id || !r->active) return;
    LOCK();
    if (r->rx_queue_slots || r->rx_queue_bytes || r->image_temp_bytes) {
        /* Keep the reservation alive until the writer releases every buffer. */
        s_used.accounting_errors++;
        UNLOCK();
        return;
    }
    int had_resources = r->jobs || r->entries || r->pending_bytes || r->image_bytes || r->temp_image_bytes;
    subtract_u32(&s_used.jobs, r->jobs);
    subtract_u32(&s_used.entries, r->entries);
    subtract_u64(&s_used.pending_bytes, r->pending_bytes);
    subtract_u64(&s_used.image_bytes, r->image_bytes);
    subtract_u64(&s_used.temp_image_bytes, r->temp_image_bytes);
    if (had_resources) s_used.released_reservations++;
    r->jobs = 0;
    r->entries = 0;
    r->pending_bytes = 0;
    r->image_bytes = 0;
    r->temp_image_bytes = 0;
    r->active = 0;
    UNLOCK();
}

void hop_xfer_budget_get_snapshot(hop_xfer_budget_snapshot *out) {
    if (!out) return;
    LOCK();
    *out = s_used;
    UNLOCK();
}

int hop_xfer_budget_set_limits_for_test(const hop_xfer_budget_limits *l) {
    if (!l || !l->max_jobs || !l->max_entries ||
        !l->max_pending_bytes || !l->max_image_bytes ||
        !l->max_single_image_bytes || l->max_single_image_bytes > l->max_image_bytes ||
        !l->max_rx_queue_slots || !l->max_rx_queue_bytes ||
        !l->max_image_temp_bytes) return 0;
    LOCK();
    if (s_used.jobs || s_used.entries || s_used.pending_bytes || s_used.image_bytes ||
        s_used.rx_queue_slots || s_used.rx_queue_bytes || s_used.image_temp_bytes) {
        UNLOCK();
        return 0;
    }
    s_limits = *l;
    memset(&s_used, 0, sizeof(s_used));
    UNLOCK();
    return 1;
}
