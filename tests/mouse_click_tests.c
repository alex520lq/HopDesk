#include "mouse_click.h"

#include <assert.h>
#include <stdio.h>

static uint8_t down(hop_mouse_click_tracker *t, uint32_t now,
                    int x, int y, uint32_t interval) {
    return hop_mouse_click_next(t, now, x, y, interval, 4, 4);
}
static void up(hop_mouse_click_tracker *t) {
    hop_mouse_click_complete(t);
}

int main(void) {
    hop_mouse_click_tracker left = {0}, right = {0};

    assert(down(&left, 1000, 100, 100, 500) == 1);
    up(&left);
    assert(down(&left, 1200, 100, 100, 500) == 2);
    up(&left);
    assert(down(&left, 1350, 100, 100, 500) == 3);
    up(&left);
    assert(down(&left, 1450, 100, 100, 500) == 1);
    up(&left);

    /* The interval is measured between button-down events, not from button-up. */
    assert(down(&left, 1980, 100, 100, 500) == 1);
    up(&left);
    assert(down(&left, 2100, 100, 100, 500) == 2);
    up(&left);
    assert(down(&left, 2200, 103, 100, 500) == 1); /* outside X rectangle */
    up(&left);
    assert(down(&left, 2300, 103, 100, 500) == 2);
    up(&left);
    assert(down(&left, 2400, 103, 103, 500) == 1); /* outside Y rectangle */
    up(&left);

    /* Separate buttons never share click history. */
    assert(down(&left, 2500, 103, 103, 500) == 2);
    assert(down(&right, 2501, 103, 103, 500) == 1);
    up(&left);
    up(&right);

    /* A zero system double-click interval disables multi-click recognition. */
    assert(down(&left, 2600, 103, 103, 0) == 1);
    up(&left);
    assert(down(&left, 2630, 103, 103, 0) == 1);
    up(&left);

    hop_mouse_click_reset(&left);
    assert(down(&left, 2700, 103, 103, 500) == 1);
    up(&left);
    assert(down(&left, 2800, 103, 103, 500) == 2);

    /* The Windows tick counter wraps after about 49.7 days. Unsigned
     * subtraction must keep a fast double click valid across that boundary. */
    hop_mouse_click_reset(&left);
    assert(down(&left, UINT32_MAX - 100u, 100, 100, 500) == 1);
    up(&left);
    assert(down(&left, 100u, 100, 100, 500) == 2);
    up(&left);

    /* A long first press consumes part of the double-click interval. */
    hop_mouse_click_reset(&left);
    assert(down(&left, 1000, 100, 100, 500) == 1);
    up(&left);
    assert(down(&left, 1490, 100, 100, 500) == 2);
    up(&left);
    assert(down(&left, 2000, 100, 100, 500) == 1);
    up(&left);

    puts("PASS: mouse click sequence detects single/double/triple clicks, interval and wraparound");
    return 0;
}
