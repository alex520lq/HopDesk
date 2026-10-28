/* =====================================================================
 *  HopDesk - Windows 网络服务端实现
 * ===================================================================== */
/* winsock2 必须在 windows.h 之前 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net_server.h"

#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

struct net_server {
    HWND            hwnd;
    hop_config      cfg;
    SOCKET          listen_s;
    SOCKET          client;
    CRITICAL_SECTION lock;      /* 保护 client 与 send */
    HANDLE          stop;
    HANDLE          thread;
    hop_decoder    *decoder;
};

/* ---- decoder 回调（在网络线程）：转主线程 ---- */
static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    net_server *s = (net_server *)user;
    if (type == HOP_MSG_PING) {
        net_server_send(s, HOP_MSG_PONG, NULL, 0);
        return;
    }
    hop_recv *r = (hop_recv *)malloc(sizeof(hop_recv) + len);
    if (!r) return;
    r->type = type;
    r->len = len;
    if (len) memcpy(r->data, p, len);
    if (!PostMessageW(s->hwnd, WM_APP_RECV, (WPARAM)type, (LPARAM)r)) {
        free(r);
    }
}

static void post_link(net_server *s, int up) {
    PostMessageW(s->hwnd, WM_APP_LINK, (WPARAM)up, 0);
}

static void handle_client(net_server *s, SOCKET c) {
    EnterCriticalSection(&s->lock);
    if (s->client != INVALID_SOCKET) {
        LeaveCriticalSection(&s->lock);
        closesocket(c);
        return;
    }
    s->client = c;
    LeaveCriticalSection(&s->lock);

    post_link(s, 1);

    /* 握手回 ACK：{ok=1, name, w, h} */
    uint8_t ack[2 + HOP_NAME_MAX + 8];
    uint32_t n = 0;
    ack[n++] = 1;
    const char *nm = s->cfg.device_name[0] ? s->cfg.device_name : "Alex";
    uint8_t nlen = (uint8_t)strlen(nm);
    ack[n++] = nlen;
    memcpy(ack + n, nm, nlen); n += nlen;
    hop_put_be32(ack + n, (uint32_t)s->cfg.screen_w); n += 4;
    hop_put_be32(ack + n, (uint32_t)s->cfg.screen_h); n += 4;
    net_server_send(s, HOP_MSG_HELLO_ACK, ack, n);

    hop_decoder_reset(s->decoder);

    char buf[2048];
    for (;;) {
        int r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
        hop_decoder_feed(s->decoder, (const uint8_t *)buf, (uint32_t)r);
    }

    EnterCriticalSection(&s->lock);
    s->client = INVALID_SOCKET;
    LeaveCriticalSection(&s->lock);
    post_link(s, 0);
}

static DWORD WINAPI server_thread(LPVOID arg) {
    net_server *s = (net_server *)arg;
    while (WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
        struct sockaddr_in addr;
        int alen = sizeof(addr);
        SOCKET c = accept(s->listen_s, (struct sockaddr *)&addr, &alen);
        if (c == INVALID_SOCKET) {
            if (WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0) break;
            Sleep(100);
            continue;
        }
        handle_client(s, c);
    }
    return 0;
}

net_server *net_server_start(HWND hwnd, const hop_config *cfg) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;

    net_server *s = (net_server *)calloc(1, sizeof(*s));
    if (!s) { WSACleanup(); return NULL; }
    s->hwnd = hwnd;
    s->cfg = *cfg;
    s->client = INVALID_SOCKET;
    s->listen_s = INVALID_SOCKET;
    InitializeCriticalSection(&s->lock);
    s->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->decoder = hop_decoder_new(s, on_message);

    s->listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listen_s == INVALID_SOCKET) goto fail;

    BOOL yes = TRUE;
    setsockopt(s->listen_s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->port);
    if (bind(s->listen_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
    if (listen(s->listen_s, 2) != 0) goto fail;

    s->thread = CreateThread(NULL, 0, server_thread, s, 0, NULL);
    if (!s->thread) goto fail;

    hop_log(HOP_LOG_INFO, "listening on port %u", (unsigned)cfg->port);
    return s;

fail:
    if (s->listen_s != INVALID_SOCKET) closesocket(s->listen_s);
    if (s->decoder) hop_decoder_free(s->decoder);
    if (s->stop) CloseHandle(s->stop);
    DeleteCriticalSection(&s->lock);
    free(s);
    WSACleanup();
    return NULL;
}

void net_server_stop(net_server *s) {
    if (!s) return;
    SetEvent(s->stop);
    if (s->listen_s != INVALID_SOCKET) closesocket(s->listen_s);
    EnterCriticalSection(&s->lock);
    if (s->client != INVALID_SOCKET) {
        shutdown(s->client, SD_BOTH);
        closesocket(s->client);
        s->client = INVALID_SOCKET;
    }
    LeaveCriticalSection(&s->lock);
    if (s->thread) { WaitForSingleObject(s->thread, 2000); CloseHandle(s->thread); }
    hop_decoder_free(s->decoder);
    CloseHandle(s->stop);
    DeleteCriticalSection(&s->lock);
    free(s);
    WSACleanup();
}

int net_server_has_client(net_server *s) {
    int has = 0;
    EnterCriticalSection(&s->lock);
    has = (s->client != INVALID_SOCKET);
    LeaveCriticalSection(&s->lock);
    return has;
}

int net_server_send(net_server *s, uint8_t type, const void *payload, uint32_t len) {
    if (!s) return 0;
    uint8_t hdr[HOP_HEADER_SIZE];
    hop_encode_header(hdr, type, len);

    EnterCriticalSection(&s->lock);
    SOCKET c = s->client;
    if (c == INVALID_SOCKET) { LeaveCriticalSection(&s->lock); return 0; }

    int ok = 1;
    if (send(c, (char *)hdr, HOP_HEADER_SIZE, 0) != HOP_HEADER_SIZE) ok = 0;
    if (ok && len && send(c, (const char *)payload, (int)len, 0) != (int)len) ok = 0;
    LeaveCriticalSection(&s->lock);
    return ok;
}
