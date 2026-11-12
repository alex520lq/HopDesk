#include "xfer_rate.h"
#include "xfer_budget.h"
#include <string.h>

#define RATE_NS_PER_SEC 1000000000ull
#define RATE_ACK_CONGESTED_US 500000ull
#define RATE_ACK_HEALTHY_US 100000ull
#define RATE_QUEUE_CONGESTED_PERMILLE 750u
#define RATE_QUEUE_HEALTHY_PERMILLE 250u


void hop_xfer_rate_init_from_config(hop_xfer_rate_limiter *rate,
                                     int configured_max_mib,
                                     uint64_t burst_bytes,
                                     uint64_t now_ns) {
    uint64_t max_mib = configured_max_mib < 16 ? 16u :
                       configured_max_mib > 4096 ? 4096u :
                       (uint64_t)configured_max_mib;
    uint64_t max_bps = max_mib * HOP_XFER_RATE_MIB_BYTES;
    uint64_t min_bps = 4ull * HOP_XFER_RATE_MIB_BYTES;
    uint64_t initial_bps = 64ull * HOP_XFER_RATE_MIB_BYTES;
    if (min_bps > max_bps) min_bps = max_bps;
    if (initial_bps > max_bps) initial_bps = max_bps;
    hop_xfer_rate_init(rate, initial_bps, min_bps, max_bps, burst_bytes, now_ns);
}

uint32_t hop_xfer_rate_queue_pressure_permille(uint64_t rx_queue_bytes,
                                                uint32_t rx_queue_slots) {
    uint64_t bytes = rx_queue_bytes >= HOP_XFER_DEFAULT_RX_QUEUE_BYTES
        ? 1000ull : rx_queue_bytes * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_BYTES;
    uint64_t slots = rx_queue_slots >= HOP_XFER_DEFAULT_RX_QUEUE_SLOTS
        ? 1000ull : (uint64_t)rx_queue_slots * 1000ull / HOP_XFER_DEFAULT_RX_QUEUE_SLOTS;
    uint64_t pressure = bytes > slots ? bytes : slots;
    return (uint32_t)(pressure > 1000ull ? 1000ull : pressure);
}

uint32_t hop_xfer_rate_feedback_from_budget(hop_xfer_rate_limiter *rate,
                                             uint64_t ack_rtt_us) {
    hop_xfer_budget_snapshot snapshot;
    hop_xfer_budget_get_snapshot(&snapshot);
    uint32_t pressure = hop_xfer_rate_queue_pressure_permille(
        snapshot.rx_queue_bytes, snapshot.rx_queue_slots);
    hop_xfer_rate_feedback(rate, ack_rtt_us, pressure);
    return pressure;
}

static uint64_t ewma(uint64_t old_value, uint64_t sample) {
    if (!old_value) return sample;
    /* Split the weighted average to avoid overflowing old*7 + sample. */
    return old_value - old_value / 8u + sample / 8u;
}

static void decrease_rate(hop_xfer_rate_limiter *rate, uint32_t numerator, uint32_t denominator) {
    uint64_t next = (rate->current_rate_bytes_per_sec * numerator) / denominator;
    if (next < rate->min_rate_bytes_per_sec) next = rate->min_rate_bytes_per_sec;
    if (next < rate->current_rate_bytes_per_sec) {
        rate->current_rate_bytes_per_sec = next;
        rate->rate_decreases++;
    }
}

void hop_xfer_rate_init(hop_xfer_rate_limiter *rate,
                        uint64_t initial_rate_bytes_per_sec,
                        uint64_t min_rate_bytes_per_sec,
                        uint64_t max_rate_bytes_per_sec,
                        uint64_t burst_bytes,
                        uint64_t now_ns) {
    if (!rate) return;
    memset(rate, 0, sizeof(*rate));
    if (!min_rate_bytes_per_sec) min_rate_bytes_per_sec = 1;
    if (max_rate_bytes_per_sec < min_rate_bytes_per_sec)
        max_rate_bytes_per_sec = min_rate_bytes_per_sec;
    if (initial_rate_bytes_per_sec < min_rate_bytes_per_sec)
        initial_rate_bytes_per_sec = min_rate_bytes_per_sec;
    if (initial_rate_bytes_per_sec > max_rate_bytes_per_sec)
        initial_rate_bytes_per_sec = max_rate_bytes_per_sec;
    if (!burst_bytes) burst_bytes = 1;
    rate->current_rate_bytes_per_sec = initial_rate_bytes_per_sec;
    rate->min_rate_bytes_per_sec = min_rate_bytes_per_sec;
    rate->max_rate_bytes_per_sec = max_rate_bytes_per_sec;
    rate->burst_bytes = burst_bytes;
    rate->tokens_bytes = burst_bytes;
    rate->last_refill_ns = now_ns;
}

uint64_t hop_xfer_rate_wait_ns(hop_xfer_rate_limiter *rate,
                               uint64_t bytes,
                               uint64_t now_ns) {
    uint64_t elapsed, missing, ns_to_full, add;
    if (!rate || !bytes || !rate->current_rate_bytes_per_sec) return 0;
    if (bytes > rate->burst_bytes) return UINT64_MAX;

    if (now_ns < rate->last_refill_ns) {
        rate->last_refill_ns = now_ns;
    } else {
        elapsed = now_ns - rate->last_refill_ns;
        if (rate->tokens_bytes < rate->burst_bytes && elapsed) {
            missing = rate->burst_bytes - rate->tokens_bytes;
            ns_to_full = (missing * RATE_NS_PER_SEC +
                          rate->current_rate_bytes_per_sec - 1u) /
                         rate->current_rate_bytes_per_sec;
            if (elapsed >= ns_to_full) {
                rate->tokens_bytes = rate->burst_bytes;
            } else {
                /* elapsed is bounded by time-to-fill, so this product cannot
                 * exceed missing * 1e9 and is safe for configured bursts. */
                add = (elapsed * rate->current_rate_bytes_per_sec) / RATE_NS_PER_SEC;
                if (add > missing) add = missing;
                rate->tokens_bytes += add;
            }
        }
        rate->last_refill_ns = now_ns;
    }

    if (rate->tokens_bytes >= bytes) {
        rate->tokens_bytes -= bytes;
        return 0;
    }
    missing = bytes - rate->tokens_bytes;
    return (missing * RATE_NS_PER_SEC + rate->current_rate_bytes_per_sec - 1u) /
           rate->current_rate_bytes_per_sec;
}

void hop_xfer_rate_note_send_time(hop_xfer_rate_limiter *rate, uint64_t bytes, uint64_t send_us) {
    if (!rate || !bytes || !rate->current_rate_bytes_per_sec) return;
    rate->send_time_ewma_us = ewma(rate->send_time_ewma_us, send_us);
    rate->send_samples++;
    /* Compare socket blocking time with the time allowed by our current rate;
     * this works across chunk sizes and does not penalize a normal 1 GbE send. */
    uint64_t expected_us;
    if (bytes > (UINT64_MAX - rate->current_rate_bytes_per_sec + 1u) / 1000000ull) {
        expected_us = UINT64_MAX;
    } else {
        expected_us = (bytes * 1000000ull +
                       rate->current_rate_bytes_per_sec - 1u) /
                      rate->current_rate_bytes_per_sec;
    }
    uint64_t congested_us = expected_us > (UINT64_MAX - 2000u) / 2u ?
                            UINT64_MAX : expected_us * 2u + 2000u;
    rate->send_congested = (uint8_t)(rate->send_time_ewma_us >= congested_us);
}

void hop_xfer_rate_feedback(hop_xfer_rate_limiter *rate,
                            uint64_t ack_rtt_us,
                            uint32_t queue_pressure_permille) {
    int congested, healthy;
    if (!rate) return;
    if (ack_rtt_us) rate->ack_rtt_ewma_us = ewma(rate->ack_rtt_ewma_us, ack_rtt_us);
    if (queue_pressure_permille > 1000u) queue_pressure_permille = 1000u;

    congested = queue_pressure_permille >= RATE_QUEUE_CONGESTED_PERMILLE ||
                rate->send_congested ||
                rate->ack_rtt_ewma_us >= RATE_ACK_CONGESTED_US;
    healthy = queue_pressure_permille <= RATE_QUEUE_HEALTHY_PERMILLE &&
              !rate->send_congested &&
              (!rate->ack_rtt_ewma_us || rate->ack_rtt_ewma_us <= RATE_ACK_HEALTHY_US);

    if (congested) {
        decrease_rate(rate, 4u, 5u);
    } else if (healthy && rate->current_rate_bytes_per_sec < rate->max_rate_bytes_per_sec) {
        /* Additive increase: one percent of the configured ceiling per
         * healthy feedback interval, independent of the current rate. */
        uint64_t increase = rate->max_rate_bytes_per_sec / 100u;
        if (!increase) increase = 1;
        uint64_t room = rate->max_rate_bytes_per_sec - rate->current_rate_bytes_per_sec;
        if (increase > room) increase = room;
        rate->current_rate_bytes_per_sec += increase;
        if (increase) rate->rate_increases++;
    }
}
