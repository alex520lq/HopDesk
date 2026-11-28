/* Real child processes + production command lifecycle; no display mutations. */
#include "../src/mac/display.c"
#include <assert.h>
mac_app g_mac;
static int s_net_online, s_sent_results, s_last_result_ok;
int net_send_session(const hop_session_id *expected, uint8_t type, const void *p, uint32_t len) {
    if (!s_net_online) return 0;
    if (expected && !hop_session_matches(expected)) return 0;
    if (type == HOP_MSG_DISP_SWITCH && len == HOP_DISPLAY_WIRE_SIZE) {
        hop_display_msg m;
        assert(hop_display_decode(&m, (const uint8_t *)p, len));
        if (m.op == HOP_DISPLAY_RESULT) {
            s_sent_results++;
            s_last_result_ok = m.ok;
        }
    }
    return 1;
}
int net_send(uint8_t type, const void *p, uint32_t len) {
    return net_send_session(NULL, type, p, len);
}
int main(void) {
    snprintf(g_mac.cfg.mac_input_cmd, sizeof(g_mac.cfg.mac_input_cmd), "%s", "/usr/bin/true");
    g_mac.cfg.disp_win_input = 15;
    g_mac.cfg.disp_mac_input = 17;
    g_mac.cfg.disp_wake_timeout_ms = 5000;
    g_mac.cfg.disp_poll_ms = 100;

    /* Offline fallback: explicit target succeeds locally; AUTO reports unavailable */
    s_net_online = 0;
    char status[192] = {0};
    assert(disp_switch_target(HOP_DISPLAY_WINDOWS) == 1);
    disp_switch_status(status, sizeof(status));
    assert(status[0] != '\0');
    assert(disp_switch_target(HOP_DISPLAY_AUTO) == 0);

    /* Busy worker rejects duplicate peer command immediately with RESULT(ok=0) */
    s_net_online = 1;
    hop_session_id session;
    assert(hop_session_generate(&session));
    hop_session_publish(&session);
    assert(run_switch_command("/usr/bin/true", 17, &session));
    assert(!run_switch_command("/bin/sh -c 'exit 7' #", 17, &session));
    pthread_mutex_lock(&s_switch_lock);
    s_switch_busy = 1;
    pthread_mutex_unlock(&s_switch_lock);
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg req = {HOP_DISPLAY_INPUT, 0, 99, 15};
    assert(hop_display_encode(wire, &req));
    disp_switch_message(wire, sizeof(wire));
    assert(s_sent_results == 1 && s_last_result_ok == 0);
    pthread_mutex_lock(&s_switch_lock);
    s_switch_busy = 0;
    pthread_mutex_unlock(&s_switch_lock);

    /* WAKE_CLEAR is accepted without error */
    hop_display_msg clr = {HOP_DISPLAY_WAKE_CLEAR, 0, 0, 0};
    assert(hop_display_encode(wire, &clr));
    disp_switch_message(wire, sizeof(wire));

    hop_session_clear(&session);
    uint64_t start = hop_monotonic_ms();
    assert(!run_switch_command("exec /bin/sleep 30 #", 17, &session));
    assert(hop_monotonic_ms() - start < 3000);
    hop_session_publish(&session);
    disp_switch_shutdown();
    start = hop_monotonic_ms();
    assert(!run_switch_command("exec /bin/sleep 30 #", 17, &session));
    assert(hop_monotonic_ms() - start < 3000);
    hop_session_clear(&session);
    puts("PASS: macOS display command lifecycle, offline fallback, busy rejection, session cancellation and shutdown kill/reap");
}
