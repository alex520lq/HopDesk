/* Authenticated control sessions. Accept never waits for a peer's recv;
 * a newly authenticated session replaces the old one. Socket ownership stays
 * with its reader until the sender is no longer using it. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "net_server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CONNECTIONS 8
struct connection;
typedef struct send_item {
    struct send_item *next;
    uint8_t type;
    uint32_t len;
    uint8_t data[1];
} send_item;
typedef struct connection {
    struct net_server *server;
    SOCKET sock;
    HANDLE thread;
    hop_decoder *decoder;
    hop_session_id session;
    unsigned long epoch;
    int authed, rejected;
    uint64_t started, last_rx;
} connection;
struct net_server {
    HWND hwnd;
    hop_config cfg;
    SOCKET listen_s, client;
    CRITICAL_SECTION lock, send_lock;
    HANDLE stop, q_evt, thread, sender;
    send_item *q_head, *q_tail;
    int q_count, listening;
    size_t q_bytes;
    unsigned long epoch;
    uint64_t last_ping;
    connection peers[MAX_CONNECTIONS];
};

static void queue_clear(net_server *s) {
    send_item *it = s->q_head;
    while (it) { send_item *next = it->next; free(it); it = next; }
    s->q_head = s->q_tail = NULL;
    s->q_count = 0; s->q_bytes = 0;
}
/* lock held. Critical-event overflow disconnects instead of losing a key-up. */
static int queue_push(net_server *s, uint8_t type, const void *p, uint32_t len) {
    hop_queue_verdict v = hop_ctrl_queue_admit(type, s->q_count, s->q_bytes, len);
    if (v == HOP_Q_DROP) return 1;
    if (v == HOP_Q_OVERFLOW) goto overflow;
    if (type == HOP_MSG_MOUSE_MOVE && len == 4 && s->q_tail &&
        s->q_tail->type == type && s->q_tail->len == 4) {
        int dx = (int16_t)hop_get_be16(p) + (int16_t)hop_get_be16(s->q_tail->data);
        int dy = (int16_t)hop_get_be16((const uint8_t *)p + 2) +
                 (int16_t)hop_get_be16(s->q_tail->data + 2);
        if (dx >= -32768 && dx <= 32767 && dy >= -32768 && dy <= 32767) {
            hop_put_be16(s->q_tail->data, (uint16_t)(int16_t)dx);
            hop_put_be16(s->q_tail->data + 2, (uint16_t)(int16_t)dy);
            return 1;
        }
    }
    send_item *it = (send_item *)malloc(sizeof(*it) + len);
    if (!it) goto overflow;
    it->next = NULL; it->type = type; it->len = len;
    if (len) memcpy(it->data, p, len);
    if (s->q_tail) s->q_tail->next = it; else s->q_head = it;
    s->q_tail = it; s->q_count++; s->q_bytes += len;
    SetEvent(s->q_evt);
    return 1;
overflow:
    hop_log(HOP_LOG_WARN, "net: control queue overflow, resetting session");
    if (s->client != INVALID_SOCKET) shutdown(s->client, SD_BOTH);
    queue_clear(s);
    return 0;
}
static int post_message(connection *c, uint8_t type, const uint8_t *p, uint32_t len) {
    hop_recv *r = (hop_recv *)malloc(sizeof(*r) + len);
    if (!r) return 0;
    r->type = type; r->len = len; r->epoch = c->epoch;
    if (len) memcpy(r->data, p, len);
    if (!PostMessageW(c->server->hwnd, WM_APP_RECV, type, (LPARAM)r)) {
        free(r); return 0;
    }
    return 1;
}
#include "net_server_protocol.inc"
void net_set_keepalive(SOCKET sock) {
    struct tcp_keepalive ka = {1, 60000, 10000};
    DWORD bytes;
    WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &bytes, NULL, NULL);
}
#include "net_server_transport.inc"
