/* Native macOS Receiver and Writer with a TCP protocol peer and no desktop I/O. */
#include "../src/mac/mac_app.h"
#include "../src/mac/xfer.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

mac_app g_mac;

typedef int peer_socket;
static uint8_t s_last_body[256];
static uint32_t s_last_len;
static atomic_int s_receive_publications;

static int receive_publications(void) { return atomic_load(&s_receive_publications); }
static void test_pause(void) { usleep(10000); }
static void close_peer(peer_socket peer) { assert(close(peer) == 0); }

static void write_frame(peer_socket peer, uint8_t type, const uint8_t *body, uint32_t length) {
    uint8_t frame[256];
    assert(length + HOP_HEADER_SIZE <= sizeof(frame));
    uint32_t size = hop_encode_frame(type, body, length, frame);
    for (uint32_t sent = 0; sent < size;) {
        ssize_t count = write(peer, frame + sent, size - sent);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        sent += (uint32_t)count;
    }
}

static void read_exact(peer_socket peer, uint8_t *data, uint32_t length) {
    while (length) {
        ssize_t count = read(peer, data, length);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        data += count;
        length -= (uint32_t)count;
    }
}

static int read_frame(peer_socket peer) {
    uint8_t header[HOP_HEADER_SIZE];
    read_exact(peer, header, sizeof(header));
    s_last_len = hop_get_be32(header + 1);
    assert(s_last_len <= sizeof(s_last_body));
    read_exact(peer, s_last_body, s_last_len);
    return header[0];
}

static struct sockaddr_in loopback_address(uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

static uint16_t available_port(void) {
    peer_socket peer = socket(AF_INET, SOCK_STREAM, 0);
    assert(peer >= 0);
    struct sockaddr_in addr = loopback_address(0);
    socklen_t length = sizeof(addr);
    assert(bind(peer, (struct sockaddr *)&addr, length) == 0);
    assert(getsockname(peer, (struct sockaddr *)&addr, &length) == 0);
    close_peer(peer);
    return ntohs(addr.sin_port);
}

static peer_socket connect_data_peer(uint16_t port, const char *token, const hop_session_id *session) {
    struct sockaddr_in addr = loopback_address(port);
    uint64_t deadline = hop_monotonic_ms() + 4000;
    peer_socket peer;
    for (;;) {
        peer = socket(AF_INET, SOCK_STREAM, 0);
        assert(peer >= 0);
        if (!connect(peer, (struct sockaddr *)&addr, sizeof(addr))) break;
        close_peer(peer);
        assert(hop_monotonic_ms() < deadline);
        test_pause();
    }
    struct timeval timeout = {4, 0};
    int enabled = 1;
    assert(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
    assert(read_frame(peer) == HOP_MSG_XFER_HELLO);
    assert(hop_xfer_hello_bound(s_last_body, s_last_len, token, session));
    uint8_t hello[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    uint32_t length = hop_xfer_hello_encode(hello, token, session);
    assert(length);
    write_frame(peer, HOP_MSG_XFER_HELLO, hello, length);
    return peer;
}

static int receive_staging_exists(const hop_session_id *session, uint32_t batch) {
    char session_hex[33], path[4096];
    const char *temp = getenv("TMPDIR");
    assert(temp && *temp);
    hop_session_hex(session, session_hex);
    int length = snprintf(path, sizeof(path), "%s/HopDesk/%s/%u", temp, session_hex, batch);
    assert(length > 0 && (size_t)length < sizeof(path));
    struct stat st;
    if (!stat(path, &st)) return 1;
    assert(errno == ENOENT);
    return 0;
}
#include "receive_transfer_scenario.h"

void clipboard_apply_paths(const char *const *paths, int n) {
    assert(n == 2);
    receive_expect_file(fopen(paths[0], "rb"), receive_first, sizeof(receive_first));
    receive_expect_file(fopen(paths[1], "rb"), receive_second, sizeof(receive_second));
    atomic_fetch_add(&s_receive_publications, 1);
}

void clipboard_complete_remote_paste(uint32_t batch, const char *const *paths, int n, int success) {
    (void)batch; (void)paths; (void)n; (void)success;
    assert(!"unexpected paste transaction");
}

void clipboard_apply_image(const uint8_t *bgra, int w, int h, int stride) {
    (void)bgra; (void)w; (void)h; (void)stride;
    assert(!"unexpected image transaction");
}

void net_set_keepalive(int socket) {
    int enabled = 1;
    assert(setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == 0);
}

int net_connect_timeout(const char *host, uint16_t port, unsigned timeout_ms) {
    (void)host; (void)port; (void)timeout_ms;
    assert(!"the app under test is the TCP server");
    return -1;
}

int main(void) {
    hop_session_id session;
    assert(hop_session_generate(&session));
    hop_session_publish(&session);
    g_mac.cfg.role = HOP_ROLE_SERVER;
    g_mac.cfg.data_port = available_port();
    strcpy(g_mac.cfg.token, "receiver-loopback");
    assert(xfer_init(&g_mac.cfg));
    run_receive_transfer(&session, g_mac.cfg.token, g_mac.cfg.data_port);
    assert(xfer_shutdown());
    hop_session_clear(&session);
    return 0;
}
