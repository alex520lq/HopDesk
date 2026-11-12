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
#include <wchar.h>

/* Observe the Receiver's clipboard handoff without writing the desktop. */
void clip_apply_remote_image(const uint8_t *p,int w,int h,int stride) {(void)p;(void)w;(void)h;(void)stride;assert(0);}
void clip_complete_remote_paste(uint32_t batch,const wchar_t *const *paths,int n,int success) {
    (void)batch;(void)paths;(void)n;(void)success;
}

static hop_session_id s_last_ack;
static uint8_t s_last_body[256];static uint32_t s_last_len;

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
    memcpy(s_last_body,body,len);s_last_len=len;
    if (header[0] == HOP_MSG_HELLO_ACK) {
        int ok; assert(hop_hello_ack_decode(body,len,&ok,NULL,NULL,&s_last_ack));
    }
    return header[0];
}

typedef SOCKET peer_socket;
static volatile LONG s_receive_publications;
static int receive_publications(void) {
    return InterlockedCompareExchange(&s_receive_publications, 0, 0);
}
static void test_pause(void) { Sleep(10); }
static void close_peer(peer_socket peer) { closesocket(peer); }
static peer_socket connect_data_peer(uint16_t port, const char *token, const hop_session_id *session) {
    peer_socket peer = connect_peer(port);
    uint8_t body[2 + HOP_TOKEN_MAX + HOP_SESSION_BYTES];
    uint32_t length = hop_xfer_hello_encode(body, token, session);
    assert(length);
    write_frame(peer, HOP_MSG_XFER_HELLO, body, length);
    assert(read_frame(peer) == HOP_MSG_XFER_HELLO);
    assert(hop_xfer_hello_bound(s_last_body, s_last_len, token, session));
    return peer;
}
static int receive_staging_exists(const hop_session_id *session, uint32_t batch) {
    wchar_t temp[MAX_PATH], path[MAX_PATH], wide_session[33];
    char session_hex[33];
    hop_session_hex(session, session_hex);
    for (size_t i = 0; i < sizeof(session_hex); ++i) wide_session[i] = session_hex[i];
    DWORD length = GetTempPathW(MAX_PATH, temp);
    assert(length > 0 && length < MAX_PATH);
    assert(swprintf(path, MAX_PATH, L"%lsHopDesk\\%ls\\%u", temp, wide_session, batch) > 0);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD error = GetLastError();
    assert(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    return 0;
}
#include "receive_transfer_scenario.h"

void clip_apply_remote_paths(const wchar_t *const *paths, int n) {
    assert(n == 2);
    receive_expect_file(_wfopen(paths[0], L"rb"), receive_first, sizeof(receive_first));
    receive_expect_file(_wfopen(paths[1], L"rb"), receive_second, sizeof(receive_second));
    assert(DeleteFileW(paths[0]) && DeleteFileW(paths[1]));
    wchar_t directory[MAX_PATH];
    assert(wcslen(paths[0]) < MAX_PATH);
    wcscpy(directory, paths[0]);
    wchar_t *separator = wcsrchr(directory, L'\\');
    assert(separator);
    *separator = L'\0';
    assert(RemoveDirectoryW(directory));
    InterlockedIncrement(&s_receive_publications);
}
static void hello(SOCKET s, const char *token) {
    uint8_t p[128]; size_t tl = strlen(token);
    p[0] = (uint8_t)tl; memcpy(p + 1, token, tl);
    p[tl + 1] = 1; p[tl + 2] = 'M';
    hop_put_be32(p + tl + 3, 1920); hop_put_be32(p + tl + 7, 1080);
    p[tl+11]=HOP_CTRL_PROTO_VER;
    write_frame(s, HOP_MSG_HELLO, p, (uint32_t)tl + 12);
}
static int wait_client(net_server *s, int expected, unsigned ms) {
    uint64_t deadline = hop_monotonic_ms() + ms;
    do { if (net_server_has_client(s) == expected) return 1; Sleep(20); }
    while (hop_monotonic_ms() < deadline);
    return 0;
}
/* The peer below is a protocol double, not a second production instance:
 * the process-wide session registry must remain owned by the app under test. */
static SOCKET loopback_listener(uint16_t *port){
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(s!=INVALID_SOCKET);
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(s,(struct sockaddr*)&a,sizeof(a)));
    int n=sizeof(a);assert(!getsockname(s,(struct sockaddr*)&a,&n));*port=ntohs(a.sin_port);
    assert(!listen(s,4));return s;
}
static SOCKET accept_bounded(SOCKET listener){
    fd_set rd;FD_ZERO(&rd);FD_SET(listener,&rd);struct timeval tv={5,0};
    assert(select(0,&rd,NULL,NULL,&tv)>0);SOCKET s=accept(listener,NULL,NULL);assert(s!=INVALID_SOCKET);
    DWORD ms=2500;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&ms,sizeof(ms));return s;
}
static void test_client_role(HWND hwnd){
    hop_config cfg;memset(&cfg,0,sizeof(cfg));cfg.role=HOP_ROLE_CLIENT;
    strcpy(cfg.peer_host,"127.0.0.1");strcpy(cfg.device_name,"Windows-client");strcpy(cfg.token,"client-secret");
    cfg.screen_w=1920;cfg.screen_h=1080;
    SOCKET control=loopback_listener(&cfg.port),data=loopback_listener(&cfg.data_port);
    net_server *client=net_server_start(hwnd,&cfg);assert(client);
    SOCKET peer=accept_bounded(control);assert(read_frame(peer)==HOP_MSG_HELLO);
    hop_hello_msg hello_msg;assert(hop_hello_decode(s_last_body,s_last_len,&hello_msg));
    assert(hop_token_matches(cfg.token,hello_msg.token,hello_msg.token_len));assert(!net_server_has_client(client));
    uint8_t ack[12+HOP_SESSION_BYTES];hop_session_id session;assert(hop_session_generate(&session));
    ack[0]=1;ack[1]=1;ack[2]='M';hop_put_be32(ack+3,1440);hop_put_be32(ack+7,900);
    ack[11]=HOP_CTRL_PROTO_VER;memcpy(ack+12,session.bytes,HOP_SESSION_BYTES);
    write_frame(peer,HOP_MSG_HELLO_ACK,ack,sizeof(ack));assert(wait_client(client,1,1500));
    assert(xfer_init(hwnd,&cfg));SOCKET data_peer=accept_bounded(data);
    assert(read_frame(data_peer)==HOP_MSG_XFER_HELLO);
    assert(hop_xfer_hello_bound(s_last_body,s_last_len,cfg.token,&session));
    uint8_t hello[2+HOP_TOKEN_MAX+HOP_SESSION_BYTES];uint32_t len=hop_xfer_hello_encode(hello,cfg.token,&session);
    write_frame(data_peer,HOP_MSG_XFER_HELLO,hello,len);
    uint64_t deadline=hop_monotonic_ms()+1500;while(!xfer_connected()&&hop_monotonic_ms()<deadline)Sleep(20);
    assert(xfer_connected());assert(!xfer_listening());
    closesocket(data_peer);closesocket(peer);assert(wait_client(client,0,1500));
    peer=accept_bounded(control);assert(read_frame(peer)==HOP_MSG_HELLO);
    assert(hop_session_generate(&session));memcpy(ack+12,session.bytes,HOP_SESSION_BYTES);
    write_frame(peer,HOP_MSG_HELLO_ACK,ack,sizeof(ack));assert(wait_client(client,1,1500));
    assert(xfer_shutdown());net_server_stop(client);closesocket(peer);closesocket(control);closesocket(data);
    puts("PASS: production Windows client control/data handshake and reconnect (protocol peer double)");
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
    SOCKET silent = connect_peer(cfg.port);
    SOCKET rejected = connect_peer(cfg.port);
    write_frame(rejected, HOP_MSG_KEY_DOWN, NULL, 0);
    assert(read_frame(rejected) == -1); closesocket(rejected);
    assert(!net_server_has_client(server));
    SOCKET first = connect_peer(cfg.port); hello(first, "test-secret");
    assert(read_frame(first) == HOP_MSG_HELLO_ACK); assert(wait_client(server, 1, 1000));
    SOCKET data_silent = connect_peer(cfg.data_port);
    SOCKET data_first = connect_peer(cfg.data_port);
    uint8_t data_hello[2+HOP_TOKEN_MAX+HOP_SESSION_BYTES];
    uint32_t data_hello_len=hop_xfer_hello_encode(data_hello,"test-secret",&s_last_ack);
    hop_session_id first_session=s_last_ack;
    write_frame(data_first, HOP_MSG_XFER_HELLO, data_hello, data_hello_len);
    assert(read_frame(data_first) == HOP_MSG_XFER_HELLO);
    deadline = hop_monotonic_ms() + 1500;
    while (!xfer_connected() && hop_monotonic_ms() < deadline) Sleep(20);
    assert(xfer_connected());
    SOCKET data_second = connect_peer(cfg.data_port);
    write_frame(data_second, HOP_MSG_XFER_HELLO, data_hello, data_hello_len);
    assert(read_frame(data_second) == HOP_MSG_XFER_HELLO);
    assert(read_frame(data_first) == -1); closesocket(data_first);
    SOCKET data_bad = connect_peer(cfg.data_port); data_hello[2] = 'X';
    write_frame(data_bad, HOP_MSG_XFER_HELLO, data_hello, data_hello_len);
    assert(read_frame(data_bad) == -1); closesocket(data_bad);
    assert(xfer_connected());
    uint8_t oversized[HOP_HEADER_SIZE]; hop_encode_header(oversized, 99, HOP_MAX_PAYLOAD + 1u);
    assert(send(data_second, (char *)oversized, sizeof(oversized), 0) == sizeof(oversized));
    deadline = hop_monotonic_ms() + 1500;
    while (xfer_connected() && hop_monotonic_ms() < deadline) Sleep(20);
    assert(!xfer_connected()); closesocket(data_second);
    assert(read_frame(data_silent) == -1); closesocket(data_silent);

    run_receive_transfer(&first_session, cfg.token, cfg.data_port);

    unsigned long old_epoch = net_server_epoch(server);
    SOCKET second = connect_peer(cfg.port); Sleep(100);
    assert(net_server_epoch(server) == old_epoch); /* TCP != authenticated */
    hello(second, "test-secret"); assert(read_frame(second) == HOP_MSG_HELLO_ACK);
    assert(net_server_epoch(server) != old_epoch);
    /* A data connection from the previous control session must now be rejected. */
    SOCKET stale_data=connect_peer(cfg.data_port);
    data_hello_len=hop_xfer_hello_encode(data_hello,"test-secret",&first_session);
    write_frame(stale_data,HOP_MSG_XFER_HELLO,data_hello,data_hello_len);
    assert(read_frame(stale_data)==-1);closesocket(stale_data);
    SOCKET fresh_data=connect_peer(cfg.data_port);
    data_hello_len=hop_xfer_hello_encode(data_hello,"test-secret",&s_last_ack);
    write_frame(fresh_data,HOP_MSG_XFER_HELLO,data_hello,data_hello_len);
    assert(read_frame(fresh_data)==HOP_MSG_XFER_HELLO);
    /* An unbound v5 data frame is never processed as v6 content. */
    write_frame(fresh_data,HOP_MSG_FILE_END,(const uint8_t *)"123456789",9);
    assert(read_frame(fresh_data)==-1);closesocket(fresh_data);
    SOCKET data_timeout=connect_peer(cfg.data_port);

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
    assert(xfer_shutdown());
    net_server_stop(server);
    test_client_role(hwnd);
    MSG message;
    while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    DestroyWindow(hwnd); UnregisterClassW(cls.lpszClassName, cls.hInstance); WSACleanup();
    puts("PASS: production control/data bind retry, auth gating, replacement, decoder failure and heartbeat timeout");
    return 0;
}
