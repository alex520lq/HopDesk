/* Real child processes + production command lifecycle; no display mutations. */
#include "../src/mac/display.c"
#include <assert.h>
mac_app g_mac;
int net_send(uint8_t type, const void *p, uint32_t len) {
    (void)type; (void)p; (void)len; return 0;
}
int main(void) {
    hop_session_id session;
    assert(hop_session_generate(&session));
    hop_session_publish(&session);
    assert(run_switch_command("/usr/bin/true", 17, &session));
    assert(!run_switch_command("/bin/sh -c 'exit 7' #", 17, &session));
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
    puts("PASS: macOS display command success, exit failure, session cancellation and shutdown kill/reap (real child processes; no display test)");
}
