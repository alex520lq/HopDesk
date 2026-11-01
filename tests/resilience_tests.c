/* Portable production helpers: handshake framing, queue policy, deadlines,
 * monotonic clock, and bounded/thread-safe logging. Not native desktop E2E. */
#include "hop.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static DWORD WINAPI log_worker(LPVOID arg) {
#else
#include <pthread.h>
#include <time.h>
static void *log_worker(void *arg) {
#endif
    (void)arg;
    for (int i = 0; i < 100; ++i) hop_log(HOP_LOG_INFO, "concurrent log %d", i);
    return 0;
}
static void pause_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms/1000), (long)(ms%1000)*1000000}; nanosleep(&t,NULL);
#endif
}
static void test_sessions(void) {
    hop_session_id a={{1}}, b={{2}}, out;
    assert(!hop_session_current(&out));
    hop_session_publish(&a); assert(hop_session_matches(&a));
    uint64_t generation=hop_session_generation();
    hop_session_publish(&b);hop_session_clear(&a);
    assert(hop_session_current(&out) && hop_session_id_equal(&out,&b));
    assert(hop_session_generation()>generation); /* stale close cannot kill replacement */
    uint8_t packet[HOP_DATA_PREFIX+4];hop_data_prefix(packet,&b,42);hop_put_be32(packet+HOP_DATA_PREFIX,42);
    const uint8_t *body;uint32_t batch,n;
    for(uint32_t i=0;i<HOP_DATA_PREFIX;i++)assert(!hop_data_unwrap(HOP_MSG_FILE_END,packet,i,&b,&batch,&body,&n));
    assert(!hop_data_unwrap(HOP_MSG_FILE_END,packet,sizeof(packet),&a,&batch,&body,&n));
    assert(hop_data_unwrap(HOP_MSG_FILE_BEGIN,packet,sizeof(packet),&b,&batch,&body,&n) && batch==42);
    hop_put_be32(packet+HOP_DATA_PREFIX,43);
    assert(!hop_data_unwrap(HOP_MSG_FILE_BEGIN,packet,sizeof(packet),&b,&batch,&body,&n));
    assert(!hop_data_unwrap(99,packet,sizeof(packet),&b,&batch,&body,&n));
    hop_session_clear(&b);assert(!hop_session_current(NULL));
    assert(hop_session_generate(&a) && hop_session_generate(&b));
    assert(!hop_session_id_equal(&a,&b));
}
static void test_handshake(void) {
    uint8_t p[] = {3, 'a', 'b', 'c', 1, 'M', 0, 0, 7, 128, 0, 0, 4, 56, HOP_CTRL_PROTO_VER};
    hop_hello_msg h;
    for (uint32_t i = 0; i < sizeof(p); ++i) assert(!hop_hello_decode(p, i, &h));
    assert(hop_hello_decode(p, sizeof(p), &h));
    assert(h.width == 1920 && h.height == 1080 && h.name_len == 1);
    assert(hop_token_matches("abc", h.token, h.token_len));
    assert(!hop_token_matches("abd", h.token, h.token_len));
    assert(!hop_token_matches("abcd", h.token, h.token_len));
    assert(!hop_token_matches("ab", h.token, h.token_len));
    assert(hop_token_matches("", NULL, 0));
    assert(!hop_token_matches(NULL, h.token, h.token_len));
    uint8_t extra[sizeof(p) + 1]; memcpy(extra, p, sizeof(p)); extra[sizeof(p)] = 0;
    assert(!hop_hello_decode(extra, sizeof(extra), &h));
    uint8_t ack[12 + HOP_SESSION_BYTES] = {1, 1, 'W', 0, 0, 7, 128, 0, 0, 4, 56, HOP_CTRL_PROTO_VER, 1};
    int ok = 0; uint32_t w = 0, height = 0;
    for (uint32_t i = 0; i < sizeof(ack); ++i) assert(!hop_hello_ack_decode(ack, i, &ok, &w, &height, NULL));
    assert(hop_hello_ack_decode(ack, sizeof(ack), &ok, &w, &height, NULL));
    assert(ok && w == 1920 && height == 1080);
    ack[0] = 0;
    assert(!hop_hello_ack_decode(ack, sizeof(ack), &ok, NULL, NULL, NULL));
}
static void test_queue(void) {
    assert(hop_ctrl_queue_admit(HOP_MSG_MOUSE_MOVE, HOP_CTRL_QUEUE_SOFT, 0, 4) == HOP_Q_DROP);
    assert(hop_ctrl_queue_admit(HOP_MSG_KEY_UP, HOP_CTRL_QUEUE_SOFT, 0, 2) == HOP_Q_ADMIT);
    assert(hop_ctrl_queue_admit(HOP_MSG_KEY_DOWN, HOP_CTRL_QUEUE_HARD, 0, 2) == HOP_Q_OVERFLOW);
    assert(hop_ctrl_queue_admit(HOP_MSG_MOUSE_BUTTON, HOP_CTRL_QUEUE_HARD, 0, 2) == HOP_Q_OVERFLOW);
    assert(hop_ctrl_queue_admit(HOP_MSG_KEY_UP, 0, HOP_CTRL_QUEUE_BYTES, 2) == HOP_Q_OVERFLOW);
    assert(hop_ctrl_queue_admit(HOP_MSG_MOUSE_MOVE, 0, HOP_CTRL_QUEUE_BYTES, 4) == HOP_Q_DROP);
    assert(hop_ctrl_queue_admit(HOP_MSG_KEY_UP, 0, (size_t)-1, 2) == HOP_Q_OVERFLOW);
    assert(!hop_msg_droppable(HOP_MSG_EDGE_LEAVE));
    assert(!hop_msg_droppable(HOP_MSG_MOUSE_ENTER));
}
static void test_deadlines(void) {
    uint64_t start = 9000;
    assert(!hop_heartbeat_due(start + 1999, start));
    assert(hop_heartbeat_due(start + 2000, start));
    assert(!hop_peer_silent(start + 6000, start));
    assert(hop_peer_silent(start + 6001, start));
    assert(!hop_handshake_expired(start + 5000, start));
    assert(hop_handshake_expired(start + 5001, start));
    assert(hop_heartbeat_due(1000, UINT64_MAX - 1000));
    uint64_t t = hop_monotonic_ms();
    assert(t > 0 && hop_monotonic_ms() >= t);
}
static void test_log(const char *path) {
    char rotated[1024];
    assert(strlen(path) + 3 < sizeof(rotated));
    snprintf(rotated, sizeof(rotated), "%s.1", path);
    remove(path); remove(rotated);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(!fseek(f, HOP_LOG_ROTATE_BYTES - 1, SEEK_SET)); fputc('\n', f); fclose(f);
    hop_log_open(path);
    hop_log(HOP_LOG_INFO, "rotation trigger");
    f = fopen(rotated, "rb"); assert(f); fclose(f);
#if defined(_WIN32)
    HANDLE workers[4];
    for (int i = 0; i < 4; ++i) { workers[i] = CreateThread(NULL, 0, log_worker, NULL, 0, NULL); assert(workers[i]); }
    for (int i = 0; i < 4; ++i) { WaitForSingleObject(workers[i], INFINITE); CloseHandle(workers[i]); }
#else
    pthread_t workers[4];
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, log_worker, NULL));
    for (int i = 0; i < 4; ++i) pthread_join(workers[i], NULL);
#endif
    hop_log_open(NULL);
    f = fopen(path, "rb"); assert(f);
    char line[256]; int lines = 0;
    while (fgets(line, sizeof(line), f)) { assert(strstr(line, "INFO concurrent log ")); lines++; }
    fclose(f); assert(lines == 400);
    remove(path); remove(rotated);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if(!strcmp(argv[1],"--watchdog-hang")) {
        hop_watchdog_begin("injected stalled OS call",200);pause_ms(3000);return 99;
    }
    if(!strcmp(argv[1],"--watchdog-progress")) {
        int ticket=hop_watchdog_begin("progressing long operation",500);
        for(int i=0;i<10;i++){pause_ms(100);hop_watchdog_ping(ticket);}
        hop_watchdog_end(ticket);pause_ms(600);return 0;
    }
    test_sessions();
    test_handshake(); test_queue(); test_deadlines(); test_log(argv[1]);
    puts("PASS: handshake, bounded queue, heartbeat deadlines, clock and concurrent log rotation");
    return 0;
}
