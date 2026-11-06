#ifndef HOP_XFER_RATE_H
#define HOP_XFER_RATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single-sender-thread token bucket and conservative AIMD feedback controller.
 * Time values are monotonic nanoseconds; RTT/send samples are microseconds. */
typedef struct {
    uint64_t current_rate_bytes_per_sec;
    uint64_t min_rate_bytes_per_sec;
    uint64_t max_rate_bytes_per_sec;
    uint64_t burst_bytes;
    uint64_t tokens_bytes;
    uint64_t last_refill_ns;
    uint64_t ack_rtt_ewma_us;
    uint64_t send_time_ewma_us;
    uint64_t rate_increases;
    uint64_t rate_decreases;
    uint32_t send_samples;
    uint8_t send_congested;
} hop_xfer_rate_limiter;

void hop_xfer_rate_init(hop_xfer_rate_limiter *rate,
                        uint64_t initial_rate_bytes_per_sec,
                        uint64_t min_rate_bytes_per_sec,
                        uint64_t max_rate_bytes_per_sec,
                        uint64_t burst_bytes,
                        uint64_t now_ns);

/* Returns the delay before 'bytes' may be sent. Call again after sleeping.
 * A chunk larger than burst_bytes is invalid and returns UINT64_MAX. */
uint64_t hop_xfer_rate_wait_ns(hop_xfer_rate_limiter *rate,
                               uint64_t bytes,
                               uint64_t now_ns);

/* Socket send duration is compared with the time allowed by the current rate. */
void hop_xfer_rate_note_send_time(hop_xfer_rate_limiter *rate, uint64_t bytes, uint64_t send_us);

/* ACK latency plus receive-queue pressure adjusts the token rate using AIMD.
 * queue_pressure_permille is clamped to [0,1000]. */
void hop_xfer_rate_feedback(hop_xfer_rate_limiter *rate,
                            uint64_t ack_rtt_us,
                            uint32_t queue_pressure_permille);

#ifdef __cplusplus
}
#endif
#endif
