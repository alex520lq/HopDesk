#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET audio_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
static SRWLOCK state_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE state_changed = CONDITION_VARIABLE_INIT;
static void lock_state(void) { AcquireSRWLockExclusive(&state_lock); }
static void unlock_state(void) { ReleaseSRWLockExclusive(&state_lock); }
static void wait_state(void) { SleepConditionVariableSRW(&state_changed, &state_lock, INFINITE, 0); }
static void signal_state(void) { WakeAllConditionVariable(&state_changed); }
static void pause_ms(unsigned ms) { Sleep(ms); }
static int retry_socket(void) {
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS;
}
static int nonblocking(audio_socket s) {
    u_long on = 1;
    return SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0) &&
           ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int audio_socket;
#define BAD_SOCKET (-1)
#define close_socket close
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static void lock_state(void) { pthread_mutex_lock(&state_lock); }
static void unlock_state(void) { pthread_mutex_unlock(&state_lock); }
static void wait_state(void) { pthread_cond_wait(&state_changed, &state_lock); }
static void signal_state(void) { pthread_cond_broadcast(&state_changed); }
static void pause_ms(unsigned ms) { usleep(ms * 1000); }
static int retry_socket(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS;
}
static int nonblocking(audio_socket s) {
    int flags = fcntl(s, F_GETFL);
    return flags >= 0 && fcntl(s, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

#include "audio_link.h"
#include <string.h>

enum {
    POLL_MS = 5,
    RETRY_MS = 500,
    HELLO_MS = 1500,
    READY_MS = 6000,
    SEND_LIMIT_MS = 120,
    KEEPALIVE_MS = 1000,
    PEER_TIMEOUT_MS = 3000
};

static hop_config s_cfg;
static hop_audio_status s_status;
static uint64_t s_revision;
static int s_worker;

typedef struct {
    audio_socket socket;
    hop_session_id session;
    uint64_t revision;
    unsigned requested;
} audio_connection;

static int current(const audio_connection *conn) {
    lock_state();
    int keep = s_revision == conn->revision;
    unlock_state();
    return keep && hop_session_matches(&conn->session);
}

static void mark_unavailable(const audio_connection *conn, unsigned modes) {
    lock_state();
    if (s_revision == conn->revision) {
        s_status.unavailable |= modes;
        s_status.active &= ~modes;
    }
    unlock_state();
}

static int wait_socket(audio_socket s, int writing, unsigned ms) {
#if defined(_WIN32)
    fd_set wanted, failed;
    FD_ZERO(&wanted);
    FD_ZERO(&failed);
    FD_SET(s, &wanted);
    if (writing) FD_SET(s, &failed);
    struct timeval timeout = {ms / 1000, (ms % 1000) * 1000};
    int rc = select(0, writing ? NULL : &wanted, writing ? &wanted : NULL, &failed, &timeout);
    if (rc < 0 && retry_socket()) return 0;
    return rc;
#else
    struct pollfd p = {s, writing ? POLLOUT : POLLIN, 0};
    int rc = poll(&p, 1, (int)ms);
    return rc < 0 && errno == EINTR ? 0 : rc;
#endif
}

static int transfer(audio_socket s, void *data, unsigned len, int writing) {
    int n = writing ? (int)send(s, (const char *)data, (int)len, 0)
                    : (int)recv(s, (char *)data, (int)len, 0);
    if (n > 0) return n;
    return n < 0 && retry_socket() ? 0 : -1;
}

static int exchange_bytes(const audio_connection *c, uint8_t *buf, unsigned size,
                          int writing, uint64_t deadline) {
    unsigned pos = 0;
    while (pos < size && current(c) && hop_monotonic_ms() < deadline) {
        int n = transfer(c->socket, buf + pos, size - pos, writing);
        if (n < 0) return 0;
        pos += (unsigned)n;
        if (!n && wait_socket(c->socket, writing, 20) < 0) return 0;
    }
    return pos == size;
}

static unsigned handshake(const audio_connection *c, const hop_config *cfg) {
    uint8_t local[HOP_AUDIO_HELLO_SIZE], remote[HOP_AUDIO_HELLO_SIZE];
    hop_audio_hello(local, c->requested, cfg->token, &c->session);
    uint64_t deadline = hop_monotonic_ms() + HELLO_MS;
    if (cfg->role == HOP_ROLE_CLIENT && !exchange_bytes(c, local, sizeof(local), 1, deadline)) return 0;
    if (!exchange_bytes(c, remote, sizeof(remote), 0, deadline)) return 0;
    unsigned peer = hop_audio_peer_mode(remote, cfg->token, &c->session);
    if (!peer) return 0;
    if (cfg->role == HOP_ROLE_SERVER && !exchange_bytes(c, local, sizeof(local), 1, deadline)) return 0;
    return peer;
}

static int configure_connection(audio_socket s) {
    int on = 1;
    int limit = 8192;
    if (!nonblocking(s) ||
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&limit, sizeof(limit)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&limit, sizeof(limit)) != 0) return 0;
#if defined(SO_NOSIGPIPE)
    if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) return 0;
#endif
    return 1;
}

static uint16_t cfg_audio_port(const hop_config *cfg) {
    return (cfg && cfg->audio_port) ? cfg->audio_port : (uint16_t)HOP_AUDIO_PORT;
}
static unsigned cfg_audio_keepalive_ms(const hop_config *cfg) {
    return (cfg && cfg->audio_keepalive_ms >= 200) ? (unsigned)cfg->audio_keepalive_ms : (unsigned)KEEPALIVE_MS;
}
static unsigned cfg_audio_peer_timeout_ms(const hop_config *cfg) {
    return (cfg && cfg->audio_peer_timeout_ms >= 500) ? (unsigned)cfg->audio_peer_timeout_ms : (unsigned)PEER_TIMEOUT_MS;
}
static unsigned cfg_audio_send_limit_ms(const hop_config *cfg) {
    return (cfg && cfg->audio_send_limit_ms >= 20) ? (unsigned)cfg->audio_send_limit_ms : (unsigned)SEND_LIMIT_MS;
}

static audio_socket open_listener(uint16_t port) {
    audio_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCKET) return s;
    int on = 1;
#if defined(_WIN32)
    int option = SO_EXCLUSIVEADDRUSE;
#else
    int option = SO_REUSEADDR;
#endif
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (nonblocking(s) &&
        setsockopt(s, SOL_SOCKET, option, (const char *)&on, sizeof(on)) == 0 &&
        bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(s, 2) == 0) return s;
    close_socket(s);
    return BAD_SOCKET;
}

static audio_socket open_client(const audio_connection *c, const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_INET; /* Matches the control listener. */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &list) != 0) return BAD_SOCKET;
    audio_socket s = BAD_SOCKET;
    for (const struct addrinfo *it = list; it && current(c); it = it->ai_next) {
        struct sockaddr_in addr = *(const struct sockaddr_in *)it->ai_addr;
        addr.sin_port = htons(port);
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == BAD_SOCKET) continue;
        if (configure_connection(s)) {
            if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
            if (retry_socket() && wait_socket(s, 1, 250) > 0) {
                int err = 0;
                int len = sizeof(err);
#if defined(_WIN32)
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0) break;
#else
                socklen_t size = (socklen_t)len;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &size) == 0 && err == 0) break;
#endif
            }
        }
        close_socket(s);
        s = BAD_SOCKET;
    }
    freeaddrinfo(list);
    return s;
}

typedef struct {
    int16_t wire[HOP_AUDIO_HEADER_SIZE / 2 + HOP_AUDIO_FRAMES * 2];
    unsigned offset;
    unsigned size;
    uint32_t rate;
    unsigned frames;
    uint64_t queued;
} audio_packet;

/* One partial frame per direction. Socket buffers and the native playback
 * queue have fixed bounds; a stalled send abandons the connection. */
static int transmit(const audio_connection *c, hop_native_audio *audio, unsigned directions,
                    audio_packet *p, uint64_t *sent, uint64_t now,
                    unsigned keepalive_ms, unsigned send_limit_ms) {
    uint8_t *wire = (uint8_t *)p->wire;
    int16_t *pcm = p->wire + HOP_AUDIO_HEADER_SIZE / 2;
    for (unsigned i = 0; i < 4; ++i) {
        if (!p->size) {
            uint32_t rate = 0;
            int frames = (directions & HOP_AUDIO_SEND) ? hop_native_audio_capture(audio, pcm, &rate) : 0;
            if (frames < 0 || frames > (int)HOP_AUDIO_FRAMES) {
                mark_unavailable(c, HOP_AUDIO_SEND);
                return 0;
            }
            if (!frames && now - *sent < keepalive_ms) return 1;
            hop_audio_header(wire, frames ? rate : 0, (unsigned)frames);
            p->size = HOP_AUDIO_HEADER_SIZE + (unsigned)frames * HOP_AUDIO_FRAME_BYTES;
            p->offset = 0;
            p->queued = now;
        }
        if (now - p->queued >= send_limit_ms || !current(c)) return 0;
        int n = transfer(c->socket, wire + p->offset, p->size - p->offset, 1);
        if (n < 0) return 0;
        p->offset += (unsigned)n;
        if (p->offset != p->size) return 1;
        p->size = 0;
        *sent = now;
    }
    return 1;
}

static int receive(const audio_connection *c, hop_native_audio *audio, unsigned directions,
                   audio_packet *p, uint64_t *received, uint64_t now) {
    uint8_t *wire = (uint8_t *)p->wire;
    const int16_t *pcm = p->wire + HOP_AUDIO_HEADER_SIZE / 2;
    for (unsigned part = 0; part < 8; ++part) {
        int body = p->offset >= HOP_AUDIO_HEADER_SIZE;
        unsigned end = body ? p->size : HOP_AUDIO_HEADER_SIZE;
        int n = transfer(c->socket, wire + p->offset, end - p->offset, 0);
        if (n < 0) return 0;
        if (!n) return 1;
        p->offset += (unsigned)n;
        if (p->offset != end) continue;
        if (!body) {
            if (!hop_audio_read_header(wire, &p->rate, &p->frames)) return 0;
            if (p->frames && !(directions & HOP_AUDIO_PLAY)) return 0;
            p->size = HOP_AUDIO_HEADER_SIZE + p->frames * HOP_AUDIO_FRAME_BYTES;
        }
        if (p->offset != p->size) continue;
        if (!current(c)) return 0;
        if (p->frames && !hop_native_audio_play(audio, pcm, p->frames, p->rate)) {
            mark_unavailable(c, HOP_AUDIO_PLAY);
            return 0;
        }
        *received = now;
        p->offset = 0;
    }
    return 1;
}

static void stream(const audio_connection *c, const hop_config *cfg) {
    unsigned peer = handshake(c, cfg);
    if (!peer) return;
    unsigned agreed = hop_audio_directions(c->requested, peer);
    unsigned directions = agreed;
    if (!current(c)) return;
    hop_native_audio *audio = directions ? hop_native_audio_open(&directions) : NULL;
    if (!audio) directions = 0;
    lock_state();
    if (s_revision == c->revision) s_status.unavailable = agreed & ~directions;
    unlock_state();
    uint8_t ready = (uint8_t)directions, peer_ready;
    uint64_t deadline = hop_monotonic_ms() + READY_MS;
    if (!exchange_bytes(c, &ready, 1, 1, deadline) ||
        !exchange_bytes(c, &peer_ready, 1, 0, deadline) ||
        (peer_ready & ~hop_audio_directions(peer, c->requested))) {
        if (audio) hop_native_audio_close(audio);
        return;
    }
    directions = hop_audio_directions(directions, peer_ready);
    if (!directions && audio) {
        hop_native_audio_close(audio);
        audio = NULL;
    }
    lock_state();
    if (s_revision == c->revision) s_status.active = directions;
    unlock_state();
    audio_packet outgoing = {0}, incoming = {0};
    uint64_t sent = hop_monotonic_ms(), received = sent;
    unsigned keepalive_ms = cfg_audio_keepalive_ms(cfg);
    unsigned peer_timeout_ms = cfg_audio_peer_timeout_ms(cfg);
    unsigned send_limit_ms = cfg_audio_send_limit_ms(cfg);
    while (current(c)) {
        uint64_t now = hop_monotonic_ms();
        if (now - received > peer_timeout_ms ||
            !transmit(c, audio, directions, &outgoing, &sent, now, keepalive_ms, send_limit_ms) ||
            !receive(c, audio, directions, &incoming, &received, now)) break;
        if (wait_socket(c->socket, 0, POLL_MS) < 0) break;
    }
    lock_state();
    s_status.active = 0;
    unlock_state();
    if (audio) hop_native_audio_close(audio);
}

static void run_worker(void) {
    audio_socket listener = BAD_SOCKET;
    int reported_bind = 0;
    for (;;) {
        lock_state();
        hop_config cfg = s_cfg;
        audio_connection c = {.socket = BAD_SOCKET, .revision = s_revision, .requested = s_status.requested};
        if (!c.requested) {
            if (listener != BAD_SOCKET) close_socket(listener);
            s_worker = 0;
            signal_state();
            unlock_state();
            return;
        }
        unlock_state();
        uint16_t port = cfg_audio_port(&cfg);
        if (!hop_session_current(&c.session)) {
            if (listener != BAD_SOCKET) {
                close_socket(listener);
                listener = BAD_SOCKET;
            }
            pause_ms(100);
            continue;
        }
        if (cfg.role == HOP_ROLE_SERVER) {
            if (listener == BAD_SOCKET) listener = open_listener(port);
            if (listener == BAD_SOCKET) {
                mark_unavailable(&c, c.requested);
                if (!reported_bind) hop_log(HOP_LOG_WARN, "audio: cannot listen on TCP %u", (unsigned)port);
                reported_bind = 1;
                pause_ms(RETRY_MS);
                continue;
            }
            if (reported_bind) {
                lock_state();
                if (s_revision == c.revision) s_status.unavailable &= ~c.requested;
                unlock_state();
            }
            reported_bind = 0;
            if (wait_socket(listener, 0, 100) > 0) c.socket = accept(listener, NULL, NULL);
            if (c.socket == BAD_SOCKET) continue;
            if (configure_connection(c.socket)) stream(&c, &cfg);
        } else {
            c.socket = open_client(&c, cfg.peer_host, port);
            if (c.socket != BAD_SOCKET) stream(&c, &cfg);
        }
        if (c.socket != BAD_SOCKET) close_socket(c.socket);
        if (cfg.role == HOP_ROLE_CLIENT) pause_ms(RETRY_MS);
    }
}

#if defined(_WIN32)
static DWORD WINAPI audio_worker(void *unused) {
#else
static void *audio_worker(void *unused) {
#endif
    (void)unused;
    run_worker();
    return 0;
}

int hop_audio_set_mode(const hop_config *cfg, unsigned mode) {
    if ((mode & ~HOP_AUDIO_MODES) || (mode && !cfg)) return 0;
    uint16_t audio_port = cfg_audio_port(cfg);
    if (mode && (cfg->port == audio_port || cfg->data_port == audio_port)) {
        hop_log(HOP_LOG_WARN, "audio: TCP %u is reserved for audio", (unsigned)audio_port);
        return 0;
    }
    lock_state();
    int tuning_changed = 0;
    if (cfg) {
        if (s_worker &&
            (cfg_audio_keepalive_ms(cfg) != cfg_audio_keepalive_ms(&s_cfg) ||
             cfg_audio_peer_timeout_ms(cfg) != cfg_audio_peer_timeout_ms(&s_cfg) ||
             cfg_audio_send_limit_ms(cfg) != cfg_audio_send_limit_ms(&s_cfg)))
            tuning_changed = 1;
        s_cfg = *cfg;
    }
    if (mode != s_status.requested || (mode && tuning_changed)) {
        s_status.requested = mode;
        s_status.active = 0;
        s_status.unavailable = 0;
        ++s_revision;
    }
    if (mode && !s_worker) {
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, audio_worker, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        s_worker = thread != NULL;
#else
        pthread_t thread;
        s_worker = pthread_create(&thread, NULL, audio_worker, NULL) == 0;
        if (s_worker) pthread_detach(thread);
#endif
        if (!s_worker) s_status.requested = 0;
    }
    int ok = s_status.requested == mode;
    unlock_state();
    return ok;
}

hop_audio_status hop_audio_get_status(void) {
    lock_state();
    hop_audio_status result = s_status;
    unlock_state();
    return result;
}

void hop_audio_shutdown(void) {
    hop_audio_set_mode(NULL, 0);
    lock_state();
    while (s_worker) wait_state();
    unlock_state();
}
