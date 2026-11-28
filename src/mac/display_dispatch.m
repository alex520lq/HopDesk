/* The main queue owns display config. Keep control reception nonblocking and
 * reject callbacks queued by an earlier control session. */
#include "mac_app.h"
#include "display_switch.h"
#include <dispatch/dispatch.h>
#include <string.h>
void mac_display_enqueue(const uint8_t *p, uint32_t len) {
    struct { uint8_t bytes[HOP_DISPLAY_WIRE_SIZE]; } frame;
    if (len != sizeof(frame.bytes)) return;
    memcpy(frame.bytes, p, sizeof(frame.bytes));
    hop_session_id session;
    if (!hop_session_current(&session)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (hop_session_matches(&session)) disp_switch_message(frame.bytes, sizeof(frame.bytes));
    });
}
