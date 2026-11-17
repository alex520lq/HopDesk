#ifndef HOP_MOUSE_CLICK_H
#define HOP_MOUSE_CLICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tracks one physical mouse button's click sequence. All coordinates are in
 * the source desktop's coordinate space; timestamps are monotonic milliseconds. */
typedef struct {
    uint32_t last_down_ms;
    int32_t last_x, last_y;
    uint8_t click_count;
    uint8_t down;
    uint8_t completed;
} hop_mouse_click_tracker;

void hop_mouse_click_reset(hop_mouse_click_tracker *tracker);
uint8_t hop_mouse_click_next(hop_mouse_click_tracker *tracker,
                             uint32_t now_ms, int32_t x, int32_t y,
                             uint32_t double_click_ms,
                             uint32_t double_click_width,
                             uint32_t double_click_height);
void hop_mouse_click_complete(hop_mouse_click_tracker *tracker,
                              void);

#ifdef __cplusplus
}
#endif
#endif
