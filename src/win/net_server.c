/* =====================================================================
 *  HopDesk - Windows 网络服务端实现
 *
 *  两条通道，各有各的线程：
 *    控制通道（port）：鼠标/键盘/显示/剪贴板文本 —— 发送走队列 + 独立线程，
 *                      调用方（钩子回调、窗口消息）只入队，绝不阻塞。
 *    数据通道（port+1）：文件字节，见 xfer.c。
 *
 *  为什么控制通道必须异步：
 *    WH_MOUSE_LL / WH_KEYBOARD_LL 的回调跑在装钩子的线程上，也就是主线程；
 *    WM_CLIPBOARDUPDATE 也在主线程。原来的实现是持锁阻塞 send，
 *    一次大载荷就能让本机鼠标键盘整体僵住，再久一点 Windows 会直接摘钩，
 *    输入捕获永久失效。
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

/* 队列深度上限。正常情况远到不了；到得了说明对端收不动，
 * 与其无限吃内存，不如丢弃（鼠标移动见 queue_push_locked 的合并逻辑）。 */
#define Q_MAX 2048

typedef struct send_item {
    struct send_item *next;
    uint8_t           type;
    uint32_t          len;
    uint8_t           data[1];      /* 柔性数组 */
} send_item;

struct net_server {
    HWND            hwnd;
    hop_config      cfg;
    SOCKET          listen_s;
    SOCKET          client;
    CRITICAL_SECTION lock;          /* 保护 client 与发送队列 */
    HANDLE          stop;
    HANDLE          thread;
    HANDLE          q_evt;          /* 队列非空 */
    HANDLE          sender;
    send_item      *q_head, *q_tail;
    int             q_count;
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

/* ---- 发送队列 ---- */

static void queue_clear(net_server *s) {
    send_item *it = s->q_head;
    while (it) {
        send_item *nx = it->next;
        free(it);
        it = nx;
    }
    s->q_head = s->q_tail = NULL;
    s->q_count = 0;
}

/* 入队。调用方已持锁。 */
static void queue_push_locked(net_server *s, uint8_t type,
                              const void *p, uint32_t len) {
    /* 鼠标移动合并：队列尾也是 MOVE 就把位移累加进去，不新增节点。
     * 必须累加而非覆盖 —— 相对位移一旦丢掉就是永久落后，光标会越用越偏。 */
    if (type == HOP_MSG_MOUSE_MOVE && len == 4 &&
        s->q_tail && s->q_tail->type == HOP_MSG_MOUSE_MOVE) {
        int dx = (int)(int16_t)hop_get_be16((const uint8_t *)p) +
                 (int)(int16_t)hop_get_be16(s->q_tail->data);
        int dy = (int)(int16_t)hop_get_be16((const uint8_t *)p + 2) +
                 (int)(int16_t)hop_get_be16(s->q_tail->data + 2);
        /* 溢出就另起一帧，交给下面正常入队 —— 钳位会丢位移 */
        if (dx > -32768 && dx < 32767 && dy > -32768 && dy < 32767) {
            hop_put_be16(s->q_tail->data, (uint16_t)(int16_t)dx);
            hop_put_be16(s->q_tail->data + 2, (uint16_t)(int16_t)dy);
            return;
        }
    }

    if (s->q_count >= Q_MAX) return;   /* 对端收不动，丢弃而不是无限吃内存 */

    send_item *it = (send_item *)malloc(sizeof(send_item) + len);
    if (!it) return;
    it->next = NULL;
    it->type = type;
    it->len = len;
    if (len && p) memcpy(it->data, p, len);

    if (s->q_tail) s->q_tail->next = it;
    else           s->q_head = it;
    s->q_tail = it;
    s->q_count++;
}

/* 逐个取出队首，锁外发送。 */
static int sender_flush(net_server *s) {
    for (;;) {
        EnterCriticalSection(&s->lock);
        send_item *it = s->q_head;
        SOCKET c = s->client;
        if (!it || c == INVALID_SOCKET) {
            LeaveCriticalSection(&s->lock);
            return 1;
        }
        s->q_head = it->next;
        if (!s->q_head) s->q_tail = NULL;
        s->q_count--;
        LeaveCriticalSection(&s->lock);

        uint8_t hdr[HOP_HEADER_SIZE];
        hop_encode_header(hdr, it->type, it->len);
        int ok = 1;
        if (send(c, (char *)hdr, HOP_HEADER_SIZE, 0) != HOP_HEADER_SIZE) ok = 0;
        if (ok && it->len &&
            send(c, (const char *)it->data, (int)it->len, 0) != (int)it->len) ok = 0;
        free(it);

        if (!ok) {
            EnterCriticalSection(&s->lock);
            queue_clear(s);
            LeaveCriticalSection(&s->lock);
            return 0;
        }
    }
}

static DWORD WINAPI sender_thread(LPVOID arg) {
    net_server *s = (net_server *)arg;
    HANDLE ev[2] = { s->stop, s->q_evt };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;              /* stop */
        sender_flush(s);
    }
    return 0;
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

    int rcv = 256 * 1024;
    setsockopt(c, SOL_SOCKET, SO_RCVBUF, (char *)&rcv, sizeof(rcv));

    char buf[8192];
    for (;;) {
        int r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
        hop_decoder_feed(s->decoder, (const uint8_t *)buf, (uint32_t)r);
    }

    EnterCriticalSection(&s->lock);
    s->client = INVALID_SOCKET;
    queue_clear(s);
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
    s->q_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->decoder = hop_decoder_new(s, on_message);

    s->listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listen_s == INVALID_SOCKET) goto fail;

    /* SO_EXCLUSIVEADDRUSE，不是 SO_REUSEADDR。
     *
     * 这两个在 Windows 上语义相反：SO_REUSEADDR 允许**别人**绑同一个端口
     * 并把连接抢走（Unix 上它只是允许绕过 TIME_WAIT，没有这个问题）；
     * SO_EXCLUSIVEADDRUSE 则独占端口，别人再绑直接失败。
     * 抢连接的症状是"连上了但键鼠时灵时不灵"，从日志里根本看不出来，
     * 所以宁可让后来者 bind 失败、报一条明确的错误。 */
    BOOL yes = TRUE;
    if (setsockopt(s->listen_s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (char *)&yes, sizeof(yes)) != 0) {
        hop_log(HOP_LOG_WARN, "net: SO_EXCLUSIVEADDRUSE failed (%d)",
                WSAGetLastError());
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->port);
    if (bind(s->listen_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
    if (listen(s->listen_s, 2) != 0) goto fail;

    s->sender = CreateThread(NULL, 0, sender_thread, s, 0, NULL);
    if (!s->sender) goto fail;
    s->thread = CreateThread(NULL, 0, server_thread, s, 0, NULL);
    if (!s->thread) goto fail;

    hop_log(HOP_LOG_INFO, "listening on port %u", (unsigned)cfg->port);
    return s;

fail:
    if (s->listen_s != INVALID_SOCKET) closesocket(s->listen_s);
    if (s->decoder) hop_decoder_free(s->decoder);
    if (s->stop) CloseHandle(s->stop);
    if (s->q_evt) CloseHandle(s->q_evt);
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
    queue_clear(s);
    LeaveCriticalSection(&s->lock);

    /* 线程没退出就不能删临界区 —— 那是释放后使用 */
    int joined = 1;
    if (s->sender) {
        if (WaitForSingleObject(s->sender, 3000) != WAIT_OBJECT_0) joined = 0;
        CloseHandle(s->sender);
    }
    if (s->thread) {
        if (WaitForSingleObject(s->thread, 3000) != WAIT_OBJECT_0) joined = 0;
        CloseHandle(s->thread);
    }

    hop_decoder_free(s->decoder);
    CloseHandle(s->stop);
    CloseHandle(s->q_evt);
    if (joined) DeleteCriticalSection(&s->lock);
    free(s);
    WSACleanup();
}

int net_server_has_client(net_server *s) {
    int has = 0;
    if (!s) return 0;
    EnterCriticalSection(&s->lock);
    has = (s->client != INVALID_SOCKET);
    LeaveCriticalSection(&s->lock);
    return has;
}

void net_server_drop(net_server *s) {
    if (!s) return;
    EnterCriticalSection(&s->lock);
    if (s->client != INVALID_SOCKET) {
        shutdown(s->client, SD_BOTH);   /* 让接收线程的 recv 立刻返回 0 */
    }
    LeaveCriticalSection(&s->lock);
}

/* 异步发送：只入队，立即返回。返回值只表示"有没有连接可以收"，
 * 不代表已经写进内核缓冲。 */
int net_server_send(net_server *s, uint8_t type, const void *payload, uint32_t len) {
    if (!s) return 0;
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->client != INVALID_SOCKET) {
        queue_push_locked(s, type, payload, len);
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    if (ok) SetEvent(s->q_evt);
    return ok;
}
