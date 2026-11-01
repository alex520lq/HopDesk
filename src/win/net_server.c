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
static void on_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    connection *c = (connection *)user;
    net_server *s = c->server;
    if (c->rejected) return;
    if (!c->authed && s->cfg.role==HOP_ROLE_CLIENT) {
        int ok;uint32_t w,h;hop_session_id session;
        if(type!=HOP_MSG_HELLO_ACK || !hop_hello_ack_decode(p,len,&ok,&w,&h,&session)||!ok){
            c->rejected=1;shutdown(c->sock,SD_BOTH);return;
        }
        EnterCriticalSection(&s->lock);
        queue_clear(s);s->client=c->sock;c->epoch=++s->epoch;c->authed=1;
        c->session=session;hop_session_publish(&session);
        c->last_rx=s->last_ping=hop_monotonic_ms();
        PostMessageW(s->hwnd,WM_APP_LINK,1,(LPARAM)c->epoch);
        if(!post_message(c,type,p,len))c->rejected=1;
        LeaveCriticalSection(&s->lock);return;
    }
    if (!c->authed) {
        hop_hello_msg hello;
        if (type != HOP_MSG_HELLO || !hop_hello_decode(p, len, &hello) ||
            !hop_token_matches(s->cfg.token, hello.token, hello.token_len)) {
            c->rejected = 1; shutdown(c->sock, SD_BOTH); return;
        }
        if (!hop_session_generate(&c->session)) { c->rejected = 1; shutdown(c->sock, SD_BOTH); return; }
        EnterCriticalSection(&s->lock);
        if (WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0) {
            c->rejected = 1;
        } else {
            int replaced = s->client != INVALID_SOCKET;
            if (replaced) shutdown(s->client, SD_BOTH);
            queue_clear(s);
            s->client = c->sock; c->epoch = ++s->epoch; c->authed = 1;
            hop_session_publish(&c->session);
            c->last_rx = s->last_ping = hop_monotonic_ms();
            uint8_t ack[2 + HOP_NAME_MAX + 9 + HOP_SESSION_BYTES];
            const char *name = s->cfg.device_name[0] ? s->cfg.device_name : "HopDesk";
            size_t nl = strlen(name);
            ack[0] = 1; ack[1] = (uint8_t)nl;
            memcpy(ack + 2, name, nl);
            hop_put_be32(ack + 2 + nl, (uint32_t)s->cfg.screen_w);
            hop_put_be32(ack + 6 + nl, (uint32_t)s->cfg.screen_h);
            ack[nl + 10] = HOP_CTRL_PROTO_VER;
            memcpy(ack + nl + 11, c->session.bytes, HOP_SESSION_BYTES);
            queue_push(s, HOP_MSG_HELLO_ACK, ack, (uint32_t)nl + 11 + HOP_SESSION_BYTES);
            PostMessageW(s->hwnd, WM_APP_LINK, replaced ? 2 : 1, (LPARAM)c->epoch);
            if (!post_message(c, type, p, len)) c->rejected = 1;
            hop_log(HOP_LOG_INFO, "net: authenticated control session %lu%s",
                    c->epoch, replaced ? " (replaced old peer)" : "");
        }
        LeaveCriticalSection(&s->lock);
        return;
    }
    EnterCriticalSection(&s->lock);
    if (s->client != c->sock || s->epoch != c->epoch) c->rejected = 1;
    else {
        c->last_rx = hop_monotonic_ms();
        if (type == HOP_MSG_HELLO || type == HOP_MSG_HELLO_ACK) c->rejected = 1;
        else if (type == HOP_MSG_PING) queue_push(s, HOP_MSG_PONG, NULL, 0);
        else if (type != HOP_MSG_PONG && !post_message(c, type, p, len)) c->rejected = 1;
    }
    LeaveCriticalSection(&s->lock);
}
void net_set_keepalive(SOCKET sock) {
    struct tcp_keepalive ka = {1, 60000, 10000};
    DWORD bytes;
    WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &bytes, NULL, NULL);
}
static DWORD WINAPI reader_thread(LPVOID arg) {
    connection *c = (connection *)arg;
    net_server *s = c->server;
    char buf[8192];
    c->decoder = hop_decoder_new(c, on_message);
    while (c->decoder && !c->rejected && WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
        uint64_t now = hop_monotonic_ms();
        if ((!c->authed && hop_handshake_expired(now, c->started)) ||
            (c->authed && hop_peer_silent(now, c->last_rx))) break;
        int n = recv(c->sock, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAEINTR) continue;
            break;
        }
        int guard = hop_watchdog_begin("control receive frame", HOP_IO_DEADLINE_MS);
        int decoded = hop_decoder_feed(c->decoder, (uint8_t *)buf, (uint32_t)n);
        hop_watchdog_end(guard);
        if (!decoded) break;
    }
    EnterCriticalSection(&s->lock);
    if (c->authed && s->client == c->sock && s->epoch == c->epoch) {
        hop_session_clear(&c->session);
        s->client = INVALID_SOCKET; queue_clear(s);
        PostMessageW(s->hwnd, WM_APP_LINK, 0, (LPARAM)c->epoch);
    }
    shutdown(c->sock, SD_BOTH);
    LeaveCriticalSection(&s->lock);
    EnterCriticalSection(&s->send_lock);
    EnterCriticalSection(&s->lock);
    closesocket(c->sock); c->sock = INVALID_SOCKET;
    LeaveCriticalSection(&s->lock);
    LeaveCriticalSection(&s->send_lock);
    hop_decoder_free(c->decoder); c->decoder = NULL;
    return 0;
}
static int send_all(SOCKET sock, const void *data, uint32_t len) {
    const char *p = (const char *)data;
    while (len) {
        int n = send(sock, p, (int)len, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEINTR) continue;
        if (n <= 0) return 0;
        p += n; len -= (uint32_t)n;
    }
    return 1;
}
static DWORD WINAPI sender_thread(LPVOID arg) {
    net_server *s = (net_server *)arg;
    HANDLE events[2] = {s->stop, s->q_evt};
    while (WaitForMultipleObjects(2, events, FALSE, 200) != WAIT_OBJECT_0) {
        EnterCriticalSection(&s->send_lock);
        EnterCriticalSection(&s->lock);
        uint64_t now = hop_monotonic_ms();
        if (s->client != INVALID_SOCKET && hop_heartbeat_due(now, s->last_ping)) {
            queue_push(s, HOP_MSG_PING, NULL, 0); s->last_ping = now;
        }
        send_item *it = s->q_head;
        SOCKET sock = s->client;
        unsigned long epoch = s->epoch;
        if (it) {
            s->q_head = it->next; if (!s->q_head) s->q_tail = NULL;
            s->q_count--; s->q_bytes -= it->len;
            if (s->q_head) SetEvent(s->q_evt);
        }
        LeaveCriticalSection(&s->lock);
        if (it) {
            int guard = hop_watchdog_begin("control frame send", 10000);
            uint8_t hdr[HOP_HEADER_SIZE];
            hop_encode_header(hdr, it->type, it->len);
            int ok = sock != INVALID_SOCKET && send_all(sock, hdr, sizeof(hdr)) &&
                     send_all(sock, it->data, it->len);
            hop_watchdog_end(guard);
            free(it);
            if (!ok) {
                EnterCriticalSection(&s->lock);
                if (s->client == sock && s->epoch == epoch) {
                    shutdown(sock, SD_BOTH); queue_clear(s);
                }
                LeaveCriticalSection(&s->lock);
            }
        }
        LeaveCriticalSection(&s->send_lock);
    }
    return 0;
}
static SOCKET open_listener(net_server *s) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return sock;
    BOOL yes = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_port = htons(s->cfg.port); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 8)) {
        hop_log(HOP_LOG_WARN, "net: port %u unavailable (%d), retrying",
                s->cfg.port, WSAGetLastError());
        closesocket(sock); return INVALID_SOCKET;
    }
    return sock;
}
static DWORD WINAPI server_thread(LPVOID arg) {
    net_server *s = (net_server *)arg;
    while (WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
        if (s->listen_s == INVALID_SOCKET) {
            s->listen_s = open_listener(s);
            InterlockedExchange((LONG *)&s->listening, s->listen_s != INVALID_SOCKET);
            if (s->listen_s == INVALID_SOCKET) { WaitForSingleObject(s->stop, 2000); continue; }
        }
        fd_set rd; FD_ZERO(&rd); FD_SET(s->listen_s, &rd);
        struct timeval tv = {0, 250000};
        int ready = select(0, &rd, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) {
            closesocket(s->listen_s); s->listen_s = INVALID_SOCKET;
            InterlockedExchange((LONG *)&s->listening, 0); continue;
        }
        if (!ready) continue;
        SOCKET sock = accept(s->listen_s, NULL, NULL);
        if (sock == INVALID_SOCKET) continue;
        connection *slot = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; ++i) {
            connection *c = &s->peers[i];
            if (c->thread && WaitForSingleObject(c->thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(c->thread); c->thread = NULL;
            }
            if (!c->thread && !slot) slot = c;
        }
        if (!slot) { closesocket(sock); continue; }
        DWORD rcv = 500, snd = HOP_CTRL_SEND_TIMEOUT_MS;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv, sizeof(rcv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd, sizeof(snd));
        net_set_keepalive(sock);
        EnterCriticalSection(&s->lock);
        memset(slot, 0, sizeof(*slot)); slot->server = s; slot->sock = sock;
        slot->started = hop_monotonic_ms();
        slot->thread = CreateThread(NULL, 0, reader_thread, slot, 0, NULL);
        if (!slot->thread) { closesocket(sock); slot->sock = INVALID_SOCKET; }
        LeaveCriticalSection(&s->lock);
    }
    if (s->listen_s != INVALID_SOCKET) closesocket(s->listen_s);
    InterlockedExchange((LONG *)&s->listening, 0);
    EnterCriticalSection(&s->lock);
    for (int i = 0; i < MAX_CONNECTIONS; ++i)
        if (s->peers[i].sock != INVALID_SOCKET) shutdown(s->peers[i].sock, SD_BOTH);
    LeaveCriticalSection(&s->lock);
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (s->peers[i].thread) {
            WaitForSingleObject(s->peers[i].thread, INFINITE);
            CloseHandle(s->peers[i].thread);
        }
    }
    return 0;
}

SOCKET net_connect_timeout_win(const char *host,uint16_t port,HANDLE stop) {
    if(!host || !*host)return INVALID_SOCKET;
    struct addrinfo hints,*res=NULL;memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;
    char service[16];snprintf(service,sizeof(service),"%u",(unsigned)port);
    int guard=hop_watchdog_begin("DNS resolution",5000);
    int rc=getaddrinfo(host,service,&hints,&res);hop_watchdog_end(guard);
    if(rc)return INVALID_SOCKET;
    SOCKET sock=INVALID_SOCKET;
    uint64_t deadline=hop_monotonic_ms()+HOP_CONNECT_TIMEOUT_MS;
    for(struct addrinfo *a=res;a && hop_monotonic_ms()<deadline;a=a->ai_next){
        if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)break;
        sock=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(sock==INVALID_SOCKET)continue;
        u_long mode=1;ioctlsocket(sock,FIONBIO,&mode);
        rc=connect(sock,a->ai_addr,(int)a->ai_addrlen);
        if(rc==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)goto next;
        while(rc==SOCKET_ERROR && hop_monotonic_ms()<deadline && WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){
            fd_set wr,err;FD_ZERO(&wr);FD_ZERO(&err);FD_SET(sock,&wr);FD_SET(sock,&err);
            struct timeval tv={0,200000};int n=select(0,NULL,&wr,&err,&tv);
            if(n<0 || (n>0 && FD_ISSET(sock,&err)))goto next;
            if(n>0){int e=0;int size=sizeof(e);if(getsockopt(sock,SOL_SOCKET,SO_ERROR,(char*)&e,&size)||e)goto next;rc=0;}
        }
        if(rc==0){mode=0;ioctlsocket(sock,FIONBIO,&mode);break;}
next:  closesocket(sock);sock=INVALID_SOCKET;
    }
    freeaddrinfo(res);return sock;
}
static DWORD WINAPI client_thread(LPVOID arg){
    net_server *s=(net_server*)arg;unsigned delay=500;
    while(WaitForSingleObject(s->stop,0)!=WAIT_OBJECT_0){
        SOCKET sock=net_connect_timeout_win(s->cfg.peer_host,s->cfg.port,s->stop);
        if(sock==INVALID_SOCKET){WaitForSingleObject(s->stop,delay);delay=delay<8000?delay*2:8000;continue;}
        connection *c=&s->peers[0];memset(c,0,sizeof(*c));c->server=s;c->sock=sock;c->started=hop_monotonic_ms();
        DWORD rcv=500,snd=HOP_CTRL_SEND_TIMEOUT_MS;
        setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(char*)&rcv,sizeof(rcv));
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(char*)&snd,sizeof(snd));
        net_set_keepalive(sock);
        uint8_t hello[2+HOP_TOKEN_MAX+HOP_NAME_MAX+9],hdr[HOP_HEADER_SIZE];uint32_t n=0;
        size_t tl=strlen(s->cfg.token),nl=strlen(s->cfg.device_name);
        hello[n++]=(uint8_t)tl;memcpy(hello+n,s->cfg.token,tl);n+=(uint32_t)tl;
        hello[n++]=(uint8_t)nl;memcpy(hello+n,s->cfg.device_name,nl);n+=(uint32_t)nl;
        hop_put_be32(hello+n,(uint32_t)s->cfg.screen_w);n+=4;hop_put_be32(hello+n,(uint32_t)s->cfg.screen_h);n+=4;
        hello[n++]=HOP_CTRL_PROTO_VER;hop_encode_header(hdr,HOP_MSG_HELLO,n);
        if(send_all(sock,hdr,sizeof(hdr)) && send_all(sock,hello,n))reader_thread(c);
        else {closesocket(sock);c->sock=INVALID_SOCKET;}
        delay=c->authed?500:(delay<8000?delay*2:8000);
        WaitForSingleObject(s->stop,delay);
    }
    return 0;
}
net_server *net_server_start(HWND hwnd, const hop_config *cfg) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return NULL;
    net_server *s = (net_server *)calloc(1, sizeof(*s));
    if (!s) { WSACleanup(); return NULL; }
    s->hwnd = hwnd; s->cfg = *cfg; s->client = s->listen_s = INVALID_SOCKET;
    for (int i = 0; i < MAX_CONNECTIONS; ++i) s->peers[i].sock = INVALID_SOCKET;
    InitializeCriticalSection(&s->lock); InitializeCriticalSection(&s->send_lock);
    s->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->q_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s->stop && s->q_evt) s->sender = CreateThread(NULL, 0, sender_thread, s, 0, NULL);
    if (s->sender) s->thread = CreateThread(NULL, 0, cfg->role==HOP_ROLE_SERVER?server_thread:client_thread, s, 0, NULL);
    if (!s->thread) { net_server_stop(s); return NULL; }
    return s;
}
void net_server_stop(net_server *s) {
    if (!s) return;
    if (s->stop) SetEvent(s->stop);
    if (s->thread) { WaitForSingleObject(s->thread, INFINITE); CloseHandle(s->thread); }
    if (s->sender) { WaitForSingleObject(s->sender, INFINITE); CloseHandle(s->sender); }
    queue_clear(s);
    if (s->stop) CloseHandle(s->stop);
    if (s->q_evt) CloseHandle(s->q_evt);
    DeleteCriticalSection(&s->send_lock); DeleteCriticalSection(&s->lock);
    free(s); WSACleanup();
}
int net_server_has_client(net_server *s) {
    if (!s) return 0;
    EnterCriticalSection(&s->lock); int on = s->client != INVALID_SOCKET;
    LeaveCriticalSection(&s->lock); return on;
}
unsigned long net_server_epoch(net_server *s) {
    if (!s) return 0;
    EnterCriticalSection(&s->lock); unsigned long epoch = s->epoch;
    LeaveCriticalSection(&s->lock); return epoch;
}
int net_server_listening(net_server *s) {
    return s && InterlockedCompareExchange((LONG *)&s->listening, 0, 0);
}
void net_server_drop(net_server *s) {
    if (!s) return;
    EnterCriticalSection(&s->lock);
    if (s->client != INVALID_SOCKET) shutdown(s->client, SD_BOTH);
    LeaveCriticalSection(&s->lock);
}
int net_server_send(net_server *s, uint8_t type, const void *payload, uint32_t len) {
    if (!s || len > HOP_MAX_PAYLOAD || (len && !payload)) return 0;
    EnterCriticalSection(&s->lock);
    int ok = s->client != INVALID_SOCKET && queue_push(s, type, payload, len);
    LeaveCriticalSection(&s->lock); return ok;
}
