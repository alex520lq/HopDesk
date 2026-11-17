#include "mouse_click.h"

#include <assert.h>
#include <stdio.h>

static uint8_t down(hop_mouse_click_tracker *t, uint64_t now,
                    int x, int y, uint32_t interval) {
    return hop_mouse_click_next(t, now, x, y, interval, 4, 4);
}
static void up(hop_mouse_click_tracker *t, uint64_t now) {
    hop_mouse_click_complete(t, now);
}

int main(void) {
    hop_mouse_click_tracker left = {0}, right = {0};

    assert(down(&left, 1000, 100, 100, 500) == 1);
    up(&left, 1020);
    assert(down(&left, 1200, 100, 100, 500) == 2);
    up(&left, 1220);
    assert(down(&left, 1350, 100, 100, 500) == 3);
    up(&left, 1370);
    assert(down(&left, 1450, 100, 100, 500) == 1);
    up(&left, 1470);

    /* Interval is measured from previous button-up to the next button-down. */
    assert(down(&left, 1980, 100, 100, 500) == 1);
    up(&left, 2000);

    assert(down(&left, 2100, 100, 100, 500) == 2);
    up(&left, 2120);
    assert(down(&left, 2200, 103, 100, 500) == 1); /* outside X rectangle */
    up(&left, 2220);
    assert(down(&left, 2300, 103, 100, 500) == 2);
    up(&left, 2320);
    assert(down(&left, 2400, 103, 103, 500) == 1); /* outside Y rectangle */
    up(&left, 2420);

    /* Separate buttons never share click history. */
    assert(down(&left, 2500, 103, 103, 500) == 2);
    assert(down(&right, 2501, 103, 103, 500) == 1);
    up(&left, 2520);
    up(&right, 2521);

    /* A zero system double-click interval disables multi-click recognition. */
    assert(down(&left, 2600, 103, 103, 0) == 1);
    up(&left, 2620);
    assert(down(&left, 2630, 103, 103, 0) == 1);
    up(&left, 2650);

    hop_mouse_click_reset(&left);
    assert(down(&left, 2700, 103, 103, 500) == 1);
    up(&left, 2720);
    assert(down(&left, 2800, 103, 103, 500) == 2);

    puts("PASS: mouse click sequence detects single/double/triple clicks and resets");
    return 0;
}
