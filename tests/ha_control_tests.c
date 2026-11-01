/* Native Windows loopback integration against the production control server.
 * No injected desktop events, clipboard access, or external network required. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../src/win/net_server.h"
#include "../src/win/xfer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Receiving clipboard payloads would be a test failure; no desktop writes. */
void clip_apply_remote_paths(const wchar_t *const *paths, int n) {(void)paths;(void)n;assert(0);}
void clip_apply_remote_image(const uint8_t *p,int w,int h,int stride) {(void)p;(void)w;(void)h;(void)stride;assert(0);}

static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_RECV) { free((hop_recv *)lp); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}
static SOCKET connect_peer(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); assert(s != INVALID_SOCKET);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
    assert(!connect(s, (struct sockaddr *)&addr, sizeof(addr)));
    DWORD timeout = 2500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    return s;
}
static void write_frame(SOCKET s, uint8_t type, const uint8_t *p, uint32_t len) {
    uint8_t out[256];
    assert(len + HOP_HEADER_SIZE <= sizeof(out));
    uint32_t size = hop_encode_frame(type, p, len, out);
    for (uint32_t sent = 0; sent < size;) {
        int n = send(s, (char *)out + sent, (int)(size - sent), 0); assert(n > 0);
        sent += (uint32_t)n;
    }
}
static int read_exact(SOCKET s, uint8_t *p, uint32_t len) {
    while (len) {
        int n = recv(s, (char *)p, (int)len, 0);
        if (n <= 0) return 0;
        p += n; len -= (uint32_t)n;
    }
    return 1;
}
static int read_frame(SOCKET s) {
    uint8_t header[HOP_HEADER_SIZE], body[256];
    if (!read_exact(s, header, sizeof(header))) return -1;
    uint32_t len = hop_get_be32(header + 1); assert(len <= sizeof(body));
    assert(read_exact(s, body, len));
    return header[0];
}
static void hello(SOCKET s, const char *token) {
    uint8_t p[128]; size_t tl = strlen(token);
    p[0] = (uint8_t)tl; memcpy(p + 1, token, tl);
    p[tl + 1] = 1; p[tl + 2] = 'M';
    hop_put_be32(p + tl + 3, 1920); hop_put_be32(p + tl + 7, 1080);
    write_frame(s, HOP_MSG_HELLO, p, (uint32_t)tl + 11);
}
static int wait_client(net_server *s, int expected, unsigned ms) {
    uint64_t deadline = hop_monotonic_ms() + ms;
    do { if (net_server_has_client(s) == expected) return 1; Sleep(20); }
    while (hop_monotonic_ms() < deadline);
    return 0;
}
int main(void) {
    WSADATA wsa; assert(!WSAStartup(MAKEWORD(2,2), &wsa));
    WNDCLASSW cls; memset(&cls, 0, sizeof(cls)); cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(NULL); cls.lpszClassName = L"HopDeskResilienceTest";
    assert(RegisterClassW(&cls));
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0,0,0,0, HWND_MESSAGE, NULL, cls.hInstance, NULL);
    assert(hwnd);
    SOCKET blocker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); assert(blocker != INVALID_SOCKET);
    BOOL yes = TRUE;
    setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(!bind(blocker, (struct sockaddr *)&addr, sizeof(addr)));
    int alen = sizeof(addr); assert(!getsockname(blocker, (struct sockaddr *)&addr, &alen));
    assert(!listen(blocker, 1));
    SOCKET data_blocker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); assert(data_blocker != INVALID_SOCKET);
    setsockopt(data_blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes));
    struct sockaddr_in data_addr; memset(&data_addr, 0, sizeof(data_addr)); data_addr.sin_family = AF_INET;
    assert(!bind(data_blocker, (struct sockaddr *)&data_addr, sizeof(data_addr)));
    alen = sizeof(data_addr); assert(!getsockname(data_blocker, (struct sockaddr *)&data_addr, &alen));
    assert(!listen(data_blocker, 1));
    hop_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = ntohs(addr.sin_port); strcpy(cfg.token, "test-secret");
    cfg.data_port = ntohs(data_addr.sin_port);
    cfg.screen_w = 1920; cfg.screen_h = 1080;
    net_server *server = net_server_start(hwnd, &cfg); assert(server);
    SetEnvironmentVariableW(L"LOCALAPPDATA", NULL); /* no history files */
    assert(xfer_init(hwnd, &cfg));
    Sleep(200); assert(!net_server_listening(server)); assert(!xfer_listening());
    closesocket(blocker); closesocket(data_blocker);
    uint64_t deadline = hop_monotonic_ms() + 4000;
    while (!net_server_listening(server) && hop_monotonic_ms() < deadline) Sleep(20);
    assert(net_server_listening(server));
    deadline = hop_monotonic_ms() + 4000;
    while (!xfer_listening() && hop_monotonic_ms() < deadline) Sleep(20);
    assert(xfer_listening());
    SOCKET data_silent = connect_peer(cfg.data_port);
    SOCKET data_first = connect_peer(cfg.data_port);
    uint8_t data_hello[] = {HOP_XFER_PROTO_VER, 11, 't','e','s','t','-','s','e','c','r','e','t'};
    write_frame(data_first, HOP_MSG_XFER_HELLO, data_hello, sizeof(data_hello));
    assert(read_frame(data_first) == HOP_MSG_XFER_HELLO);
    deadline = hop_monotonic_ms() + 1500;
    while (!xfer_connected() && hop_monotonic_ms() < deadline) Sleep(20);
    assert(xfer_connected());
    SOCKET data_second = connect_peer(cfg.data_port);
    write_frame(data_second, HOP_MSG_XFER_HELLO, data_hello, sizeof(data_hello));
    assert(read_frame(data_second) == HOP_MSG_XFER_HELLO);
    assert(read_frame(data_first) == -1); closesocket(data_first);
    SOCKET data_bad = connect_peer(cfg.data_port); data_hello[2] = 'X';
    write_frame(data_bad, HOP_MSG_XFER_HELLO, data_hello, sizeof(data_hello));
    assert(read_frame(data_bad) == -1); closesocket(data_bad);
    assert(xfer_connected());
    uint8_t oversized[HOP_HEADER_SIZE]; hop_encode_header(oversized, 99, HOP_MAX_PAYLOAD + 1u);
    assert(send(data_second, (char *)oversized, sizeof(oversized), 0) == sizeof(oversized));
    deadline = hop_monotonic_ms() + 1500;
    while (xfer_connected() && hop_monotonic_ms() < deadline) Sleep(20);
    assert(!xfer_connected()); closesocket(data_second);
    assert(read_frame(data_silent) == -1); closesocket(data_silent);
    SOCKET data_timeout = connect_peer(cfg.data_port);

    SOCKET silent = connect_peer(cfg.port);
    SOCKET rejected = connect_peer(cfg.port);
    write_frame(rejected, HOP_MSG_KEY_DOWN, NULL, 0);
    assert(read_frame(rejected) == -1); closesocket(rejected);
    assert(!net_server_has_client(server));
    SOCKET first = connect_peer(cfg.port); hello(first, "test-secret");
    assert(read_frame(first) == HOP_MSG_HELLO_ACK); assert(wait_client(server, 1, 1000));
    unsigned long old_epoch = net_server_epoch(server);
    SOCKET second = connect_peer(cfg.port); Sleep(100);
    assert(net_server_epoch(server) == old_epoch); /* TCP != authenticated */
    hello(second, "test-secret"); assert(read_frame(second) == HOP_MSG_HELLO_ACK);
    assert(net_server_epoch(server) != old_epoch);
    closesocket(first);
    rejected = connect_peer(cfg.port); hello(rejected, "wrong-token");
    assert(read_frame(rejected) == -1); closesocket(rejected);
    assert(net_server_has_client(server)); /* bad peer cannot evict a good peer */
    uint8_t bad[HOP_HEADER_SIZE]; hop_encode_header(bad, 99, HOP_MAX_PAYLOAD + 1u);
    assert(send(second, (char *)bad, sizeof(bad), 0) == sizeof(bad));
    assert(wait_client(server, 0, 1500)); closesocket(second);
    SOCKET live = connect_peer(cfg.port); hello(live, "test-secret");
    assert(read_frame(live) == HOP_MSG_HELLO_ACK);
    deadline = hop_monotonic_ms() + 7000;
    while (hop_monotonic_ms() < deadline) {
        int type = read_frame(live); assert(type == HOP_MSG_PING);
        write_frame(live, HOP_MSG_PONG, NULL, 0);
    }
    assert(net_server_has_client(server));
    assert(wait_client(server, 0, 7500)); /* no PONG: bounded blackhole recovery */
    closesocket(live);
    assert(read_frame(silent) == -1); closesocket(silent); /* handshake timeout */
    assert(read_frame(data_timeout) == -1); closesocket(data_timeout);
    xfer_shutdown();
    net_server_stop(server);
    MSG message;
    while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    DestroyWindow(hwnd); UnregisterClassW(cls.lpszClassName, cls.hInstance); WSACleanup();
    puts("PASS: production control/data bind retry, auth gating, replacement, decoder failure and heartbeat timeout");
    return 0;
}
