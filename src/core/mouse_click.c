#include "mouse_click.h"

#include <string.h>

static uint64_t distance(int32_t a, int32_t b) {
    int64_t delta = (int64_t)a - (int64_t)b;
    return (uint64_t)(delta < 0 ? -delta : delta);
}

void hop_mouse_click_reset(hop_mouse_click_tracker *tracker) {
    if (tracker) memset(tracker, 0, sizeof(*tracker));
}

uint8_t hop_mouse_click_next(hop_mouse_click_tracker *tracker,
                             uint64_t now_ms, int32_t x, int32_t y,
                             uint32_t double_click_ms,
                             uint32_t double_click_width,
                             uint32_t double_click_height) {
    uint8_t count = 1;
    if (!tracker) return count;

    /* Windows exposes the full double-click rectangle dimensions. The
     * rectangle is centered on the previous click, so compare half-widths. */
    uint32_t half_w = double_click_width / 2u;
    uint32_t half_h = double_click_height / 2u;
    if (half_w == 0) half_w = 1;
    if (half_h == 0) half_h = 1;

    if (double_click_ms > 0 && tracker->completed && !tracker->down &&
        now_ms >= tracker->last_up_ms &&
        now_ms - tracker->last_up_ms <= double_click_ms &&
        distance(x, tracker->last_x) <= half_w &&
        distance(y, tracker->last_y) <= half_h) {
        count = tracker->click_count < 3u
              ? (uint8_t)(tracker->click_count + 1u) : 1u;
    }

    tracker->last_x = x;
    tracker->last_y = y;
    tracker->click_count = count;
    tracker->down = 1;
    tracker->completed = 0;
    return count;
}

void hop_mouse_click_complete(hop_mouse_click_tracker *tracker,
                              uint64_t now_ms) {
    if (!tracker || !tracker->down) return;
    tracker->down = 0;
    tracker->completed = 1;
    tracker->last_up_ms = now_ms;
}
