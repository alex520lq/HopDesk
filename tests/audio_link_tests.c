#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
typedef SOCKET test_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
typedef int test_socket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

#include "../src/platform/audio_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned opens, closes, plays;
static int fail_capture, fail_playback;
static const int16_t expected[] = {123, -456, 789, -1234};

struct hop_native_audio { int sent; };
static hop_native_audio fixture;

hop_native_audio *hop_native_audio_open(unsigned *directions) {
    CHECK(*directions == HOP_AUDIO_MODES || *directions == HOP_AUDIO_SEND);
    ++opens;
    fixture.sent = 0;
    return &fixture;
}

int hop_native_audio_capture(hop_native_audio *a, int16_t *pcm, uint32_t *rate) {
    if (fail_capture) return -1;
    if (a->sent) return 0;
    a->sent = 1;
    memcpy(pcm, expected, sizeof(expected));
    *rate = 44100;
    return 2;
}

int hop_native_audio_play(hop_native_audio *a, const int16_t *pcm, unsigned count, uint32_t rate) {
    CHECK(a == &fixture && count == 2 && rate == 48000);
    CHECK(!memcmp(pcm, expected, sizeof(expected)));
    ++plays;
    return !fail_playback;
}

void hop_native_audio_close(hop_native_audio *a) {
    CHECK(a == &fixture);
    ++closes;
}

static void pause_short(void) {
#if defined(_WIN32)
    Sleep(10);
#else
    struct timeval timeout = {0, 10000};
    select(0, NULL, NULL, NULL, &timeout);
#endif
}

static test_socket connect_peer(void) {
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(HOP_AUDIO_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (unsigned i = 0; i < 150; ++i) {
        test_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(s != BAD_SOCKET);
        if (connect(s, (struct sockaddr *)&address, sizeof(address)) == 0) {
#if defined(_WIN32)
            DWORD timeout = 3000;
#else
            struct timeval timeout = {3, 0};
#endif
            CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
            return s;
        }
        close_socket(s);
        pause_short();
    }
    CHECK(0 && "audio listener did not start");
    return BAD_SOCKET;
}

static void send_bytes(test_socket s, const uint8_t *p, unsigned size) {
    for (unsigned i = 0; i < size; ++i) CHECK(send(s, (const char *)(p + i), 1, 0) == 1);
}

static void read_bytes(test_socket s, uint8_t *p, unsigned size) {
    while (size) {
        int n = (int)recv(s, (char *)p, (int)size, 0);
        CHECK(n > 0);
        p += n;
        size -= (unsigned)n;
    }
}

static void expect_close(test_socket s) {
    char byte;
    CHECK(recv(s, &byte, 1, 0) == 0);
    close_socket(s);
}

static void check_protocol(void) {
    uint8_t wire[HOP_AUDIO_HEADER_SIZE];
    uint32_t rate;
    unsigned frames;
    hop_audio_header(wire, 192000, HOP_AUDIO_FRAMES);
    CHECK(hop_audio_read_header(wire, &rate, &frames));
    CHECK(rate == 192000 && frames == HOP_AUDIO_FRAMES);
    hop_audio_header(wire, 48000, HOP_AUDIO_FRAMES + 1);
    CHECK(!hop_audio_read_header(wire, &rate, &frames));
    hop_audio_header(wire, 7999, 1);
    CHECK(!hop_audio_read_header(wire, &rate, &frames));
    hop_audio_header(wire, 0, 0);
    CHECK(hop_audio_read_header(wire, &rate, &frames) && !rate && !frames);
    wire[7] = 1;
    CHECK(!hop_audio_read_header(wire, &rate, &frames));
    CHECK(hop_audio_directions(HOP_AUDIO_SEND, HOP_AUDIO_SEND) == 0);
    CHECK(hop_audio_directions(HOP_AUDIO_PLAY, HOP_AUDIO_SEND) == HOP_AUDIO_PLAY);
    CHECK(hop_audio_directions(HOP_AUDIO_SEND, HOP_AUDIO_PLAY) == HOP_AUDIO_SEND);
    CHECK(hop_audio_directions(HOP_AUDIO_MODES, HOP_AUDIO_MODES) == HOP_AUDIO_MODES);
}

typedef struct {
    test_socket listener;
    hop_session_id session;
    char token[HOP_TOKEN_MAX];
} client_peer_context;

static test_socket listen_peer(void) {
    test_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(s != BAD_SOCKET);
    int on = 1;
    CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on)) == 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(HOP_AUDIO_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(s, 1) == 0);
    return s;
}

#if defined(_WIN32)
static DWORD WINAPI client_peer_thread(void *arg) {
#else
static void *client_peer_thread(void *arg) {
#endif
    client_peer_context *ctx = arg;
    test_socket peer = accept(ctx->listener, NULL, NULL);
    CHECK(peer != BAD_SOCKET);
#if defined(_WIN32)
    DWORD timeout = 3000;
#else
    struct timeval timeout = {3, 0};
#endif
    CHECK(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    uint8_t hello[HOP_AUDIO_HELLO_SIZE];
    read_bytes(peer, hello, sizeof(hello));
    CHECK(hop_audio_peer_mode(hello, ctx->token, &ctx->session) == HOP_AUDIO_SEND);
    hop_audio_hello(hello, HOP_AUDIO_PLAY, ctx->token, &ctx->session);
    send_bytes(peer, hello, sizeof(hello));
    uint8_t ready = 0;
    read_bytes(peer, &ready, 1);
    CHECK(ready == HOP_AUDIO_SEND);
    ready = HOP_AUDIO_PLAY;
    send_bytes(peer, &ready, 1);
    uint8_t packet[HOP_AUDIO_HEADER_SIZE + sizeof(expected)];
    read_bytes(peer, packet, HOP_AUDIO_HEADER_SIZE);
    uint32_t rate = 0;
    unsigned frames = 0;
    CHECK(hop_audio_read_header(packet, &rate, &frames));
    CHECK(rate == 44100 && frames == 2);
    read_bytes(peer, packet + HOP_AUDIO_HEADER_SIZE, frames * HOP_AUDIO_FRAME_BYTES);
    CHECK(!memcmp(packet + HOP_AUDIO_HEADER_SIZE, expected, sizeof(expected)));
    hop_session_clear(&ctx->session); /* client closes first, avoiding a TIME_WAIT listener conflict */
    expect_close(peer);
    close_socket(peer);
    close_socket(ctx->listener);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static test_socket pair_audio(const hop_config *cfg, const hop_session_id *session) {
    test_socket peer = connect_peer();
    uint8_t hello[HOP_AUDIO_HELLO_SIZE];
    hop_audio_hello(hello, HOP_AUDIO_MODES, cfg->token, session);
    send_bytes(peer, hello, sizeof(hello));
    read_bytes(peer, hello, sizeof(hello));
    CHECK(hop_audio_peer_mode(hello, cfg->token, session) == HOP_AUDIO_MODES);
    uint8_t ready = HOP_AUDIO_MODES;
    send_bytes(peer, &ready, 1);
    read_bytes(peer, &ready, 1);
    CHECK(ready == HOP_AUDIO_MODES);
    return peer;
}

int main(void) {
#if defined(_WIN32)
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    check_protocol();
    hop_config cfg = {.role = HOP_ROLE_CLIENT, .port = HOP_DEFAULT_PORT,
                      .data_port = HOP_DATA_PORT, .token = "audio-integration-test"};
    snprintf(cfg.peer_host, sizeof(cfg.peer_host), "%s", "127.0.0.1");
    hop_session_id session = {{1}}, stale = {{2}};
    hop_session_publish(&session);
    client_peer_context context = {.listener = listen_peer(), .session = session};
    snprintf(context.token, sizeof(context.token), "%s", cfg.token);
#if defined(_WIN32)
    HANDLE peer_thread = CreateThread(NULL, 0, client_peer_thread, &context, 0, NULL);
    CHECK(peer_thread != NULL);
#else
    pthread_t peer_thread;
    CHECK(pthread_create(&peer_thread, NULL, client_peer_thread, &context) == 0);
#endif
    CHECK(hop_audio_set_mode(&cfg, HOP_AUDIO_SEND));
#if defined(_WIN32)
    CHECK(WaitForSingleObject(peer_thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(peer_thread);
#else
    CHECK(pthread_join(peer_thread, NULL) == 0);
#endif
    hop_audio_shutdown();
    CHECK(opens == 1 && closes == 1 && plays == 0);

    hop_session_publish(&session);
    cfg.role = HOP_ROLE_SERVER;
    cfg.peer_host[0] = '\0';
    CHECK(hop_audio_set_mode(&cfg, HOP_AUDIO_MODES));

    uint8_t hello[HOP_AUDIO_HELLO_SIZE];
    test_socket peer = connect_peer();
    hop_audio_hello(hello, HOP_AUDIO_MODES, cfg.token, &stale);
    send_bytes(peer, hello, sizeof(hello));
    expect_close(peer);

    peer = connect_peer();
    hop_audio_hello(hello, HOP_AUDIO_MODES, "other-token", &session);
    send_bytes(peer, hello, sizeof(hello));
    expect_close(peer);

    peer = pair_audio(&cfg, &session);
    uint8_t packet[HOP_AUDIO_HEADER_SIZE + sizeof(expected)];
    read_bytes(peer, packet, sizeof(packet));
    uint32_t rate;
    unsigned frames;
    CHECK(hop_audio_read_header(packet, &rate, &frames) && rate == 44100 && frames == 2);
    CHECK(!memcmp(packet + HOP_AUDIO_HEADER_SIZE, expected, sizeof(expected)));
    CHECK(hop_audio_get_status().active == HOP_AUDIO_MODES);

    hop_audio_header(packet, 48000, 2);
    send_bytes(peer, packet, sizeof(packet));
    /* No read callbacks after this invalid frame, which follows the valid one. */
    hop_audio_header(packet, 48000, HOP_AUDIO_FRAMES + 1);
    send_bytes(peer, packet, HOP_AUDIO_HEADER_SIZE);
    expect_close(peer);

    fail_capture = 1;
    peer = pair_audio(&cfg, &session);
    expect_close(peer);
    CHECK(hop_audio_get_status().unavailable & HOP_AUDIO_SEND);
    fail_capture = 0;

    fail_playback = 1;
    peer = pair_audio(&cfg, &session);
    read_bytes(peer, packet, sizeof(packet));
    hop_audio_header(packet, 48000, 2);
    memcpy(packet + HOP_AUDIO_HEADER_SIZE, expected, sizeof(expected));
    send_bytes(peer, packet, sizeof(packet));
    expect_close(peer);
    CHECK(hop_audio_get_status().unavailable & HOP_AUDIO_PLAY);
    fail_playback = 0;

    peer = pair_audio(&cfg, &session);
    read_bytes(peer, packet, sizeof(packet));
    hop_session_clear(&session);
    expect_close(peer);
    hop_audio_shutdown();
    CHECK(!hop_audio_get_status().active && !hop_audio_get_status().requested);
    CHECK(opens == 5 && closes == 5 && plays == 2);
#if defined(_WIN32)
    WSACleanup();
#endif
    puts("audio link tests: client/server roles, direction negotiation, session binding, PCM, invalid frames, reconnect and shutdown passed");
    return 0;
}
