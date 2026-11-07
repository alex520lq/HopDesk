/* =====================================================================
 *  HopDesk - 帧编解码（平台无关）
 *
 *  帧 = type(1) + length(4, BE) + payload(length)
 * ===================================================================== */
#include "hop.h"
#include <stdlib.h>
#include <string.h>


/* ---- Lazy file clipboard control payloads ---- */

static int clip_offer_name_valid(const char *name, size_t len) {
    if (!name || len == 0 || len > HOP_NAME_LEN_MAX) return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0 || c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

uint32_t hop_clip_file_offer_encode(uint8_t *out, uint32_t capacity,
                                    uint32_t origin, uint64_t generation,
                                    const char *const *names, int count) {
    if (!out || !names || origin == 0 || generation == 0 ||
        count <= 0 || count > (int)HOP_CLIP_OFFER_MAX_ITEMS ||
        capacity < HOP_CLIP_OFFER_HEADER_BYTES) return 0;
    hop_put_be32(out, origin);
    hop_put_be64(out + 4, generation);
    hop_put_be16(out + 12, (uint16_t)count);
    uint32_t off = HOP_CLIP_OFFER_HEADER_BYTES;
    for (int i = 0; i < count; ++i) {
        if (!names[i]) return 0;
        size_t n = strlen(names[i]);
        if (!clip_offer_name_valid(names[i], n) ||
            capacity - off < 2u + n) return 0;
        hop_put_be16(out + off, (uint16_t)n);
        off += 2;
        memcpy(out + off, names[i], n);
        off += (uint32_t)n;
    }
    return off;
}

int hop_clip_file_offer_decode(const uint8_t *p, uint32_t len,
                               hop_clip_file_offer_msg *out) {
    if (!p || !out || len < HOP_CLIP_OFFER_HEADER_BYTES) return 0;
    memset(out, 0, sizeof(*out));
    out->origin = hop_get_be32(p);
    out->generation = hop_get_be64(p + 4);
    out->count = hop_get_be16(p + 12);
    if (!out->origin || !out->generation || !out->count ||
        out->count > HOP_CLIP_OFFER_MAX_ITEMS) return 0;
    uint32_t off = HOP_CLIP_OFFER_HEADER_BYTES;
    for (uint16_t i = 0; i < out->count; ++i) {
        if (len - off < 2u) return 0;
        uint16_t n = hop_get_be16(p + off);
        off += 2;
        if (n == 0 || n > HOP_NAME_LEN_MAX || len - off < n ||
            !clip_offer_name_valid((const char *)p + off, n)) return 0;
        out->names[i] = p + off;
        out->name_lens[i] = n;
        off += n;
    }
    return off == len;
}

int hop_clip_file_offer_clear_decode(const uint8_t *p, uint32_t len,
                                     uint32_t *origin, uint64_t *generation) {
    if (!p || len != HOP_CLIP_OFFER_CLEAR_BYTES) return 0;
    uint32_t o = hop_get_be32(p);
    uint64_t g = hop_get_be64(p + 4);
    if (!o || !g) return 0;
    if (origin) *origin = o;
    if (generation) *generation = g;
    return 1;
}

uint32_t hop_clip_paste_request_encode(uint8_t out[HOP_CLIP_PASTE_REQUEST_BYTES],
                                       uint32_t request_id, uint32_t origin,
                                       uint64_t generation, uint64_t clipboard_version) {
    if (!out || !(request_id & HOP_XFER_PASTE_BATCH_FLAG) ||
        !origin || !generation) return 0;
    hop_put_be32(out, request_id);
    hop_put_be32(out + 4, origin);
    hop_put_be64(out + 8, generation);
    hop_put_be64(out + 16, clipboard_version);
    return HOP_CLIP_PASTE_REQUEST_BYTES;
}

int hop_clip_paste_request_decode(const uint8_t *p, uint32_t len,
                                  hop_clip_paste_request_msg *out) {
    if (!p || !out || len != HOP_CLIP_PASTE_REQUEST_BYTES) return 0;
    out->request_id = hop_get_be32(p);
    out->origin = hop_get_be32(p + 4);
    out->generation = hop_get_be64(p + 8);
    out->clipboard_version = hop_get_be64(p + 16);
    return (out->request_id & HOP_XFER_PASTE_BATCH_FLAG) != 0 &&
           out->origin != 0 && out->generation != 0;
}

uint32_t hop_clip_paste_failed_encode(uint8_t out[HOP_CLIP_PASTE_FAILED_BYTES],
                                      uint32_t request_id, uint8_t reason) {
    if (!out || !(request_id & HOP_XFER_PASTE_BATCH_FLAG) || reason == 0) return 0;
    hop_put_be32(out, request_id);
    out[4] = reason;
    return HOP_CLIP_PASTE_FAILED_BYTES;
}

int hop_clip_paste_failed_decode(const uint8_t *p, uint32_t len,
                                 uint32_t *request_id, uint8_t *reason) {
    if (!p || len != HOP_CLIP_PASTE_FAILED_BYTES) return 0;
    uint32_t id = hop_get_be32(p);
    if (!(id & HOP_XFER_PASTE_BATCH_FLAG) || p[4] == 0) return 0;
    if (request_id) *request_id = id;
    if (reason) *reason = p[4];
    return 1;
}

/* ---- 字节序 ---- */
void hop_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}
void hop_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}
void hop_put_be64(uint8_t *p, uint64_t v) {
    hop_put_be32(p, (uint32_t)(v >> 32));
    hop_put_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}
uint16_t hop_get_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
uint32_t hop_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
uint64_t hop_get_be64(const uint8_t *p) {
    return ((uint64_t)hop_get_be32(p) << 32) | (uint64_t)hop_get_be32(p + 4);
}

/* ---- 编码 ---- */
uint32_t hop_encode_frame(uint8_t type, const void *payload, uint32_t plen,
                          uint8_t *out) {
    out[0] = type;
    hop_put_be32(out + 1, plen);
    if (plen && payload) memcpy(out + HOP_HEADER_SIZE, payload, plen);
    return HOP_HEADER_SIZE + plen;
}

void hop_encode_header(uint8_t out[HOP_HEADER_SIZE], uint8_t type,
                       uint32_t plen) {
    out[0] = type;
    hop_put_be32(out + 1, plen);
}

/* ---- 解码状态机 ---- */
struct hop_decoder {
    void         *user;
    hop_on_message cb;
    uint8_t       hdr[HOP_HEADER_SIZE];
    uint32_t      hdr_have;
    uint8_t      *body;
    uint32_t      body_len;    /* 期望长度 */
    uint32_t      body_have;
    int           failed;      /* 粘滞失败：流已损坏，只能 reset / 断开 */
};

hop_decoder *hop_decoder_new(void *user, hop_on_message cb) {
    hop_decoder *d = (hop_decoder *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->user = user;
    d->cb = cb;
    return d;
}

void hop_decoder_reset(hop_decoder *d) {
    d->failed = 0;
    d->hdr_have = 0;
    d->body_have = 0;
    d->body_len = 0;
    if (d->body) { free(d->body); d->body = NULL; }
}

void hop_decoder_free(hop_decoder *d) {
    if (!d) return;
    if (d->body) free(d->body);
    free(d);
}

static void emit(hop_decoder *d) {
    d->cb(d->user, d->hdr[0], d->body, d->body_len);
    d->hdr_have = 0;
    d->body_have = 0;
    d->body_len = 0;
    if (d->body) { free(d->body); d->body = NULL; }
}

int hop_decoder_failed(const hop_decoder *d) {
    return !d || d->failed;
}

static int fail(hop_decoder *d) {
    hop_decoder_reset(d);
    d->failed = 1;
    return 0;
}

int hop_decoder_feed(hop_decoder *d, const uint8_t *buf, uint32_t n) {
    uint32_t i = 0;

    if (!d) return 0;
    if (d->failed) return 0;

    /* 按段拷贝，不是逐字节。
     * 原来的实现对每个字节跑一遍状态机 —— 1 MB 分块就是 100 万次循环，
     * GB 级传输光解码就能吃掉一个核。这里每段一次 memcpy，
     * 段边界仍然落在"头部未满 / 体未满"两处，跨段续传语义不变。 */
    while (i < n) {
        if (d->hdr_have < HOP_HEADER_SIZE) {
            uint32_t take = HOP_HEADER_SIZE - d->hdr_have;
            if (take > n - i) take = n - i;
            memcpy(d->hdr + d->hdr_have, buf + i, take);
            d->hdr_have += take;
            i += take;

            if (d->hdr_have < HOP_HEADER_SIZE) break;

            d->body_len = hop_get_be32(d->hdr + 1);
            d->body_have = 0;
            /* 长度不可信 = 帧边界丢失。不能"跳过这一帧"：下一帧从哪开始
             * 已经无从知道，继续解只会把载荷字节当成帧头。 */
            if (d->body_len > HOP_MAX_PAYLOAD) return fail(d);
            if (d->body_len == 0) {
                d->body = NULL;
                emit(d);
                if (d->failed) return 0;
                continue;
            }
            d->body = (uint8_t *)malloc(d->body_len);
            if (!d->body) return fail(d);
        } else {
            uint32_t take = d->body_len - d->body_have;
            if (take > n - i) take = n - i;
            memcpy(d->body + d->body_have, buf + i, take);
            d->body_have += take;
            i += take;

            if (d->body_have >= d->body_len) {
                emit(d);
                if (d->failed) return 0;
            }
        }
    }
    return 1;
}

/* ---- 控制通道会话辅助 ---- */

int hop_msg_droppable(uint8_t type) {
    switch (type) {
        case HOP_MSG_MOUSE_MOVE:   /* 相对位移晚到不如不到：队列已积压时光标本来就跟不上 */
        case HOP_MSG_MOUSE_WHEEL:
        case HOP_MSG_PING:         /* 队列里有别的帧，对端照样能判活 */
        case HOP_MSG_PONG:
            return 1;
        default:
            return 0;
    }
}

hop_queue_verdict hop_ctrl_queue_admit(uint8_t type, int count, size_t queued_bytes,
                                       uint32_t len) {
    int droppable = hop_msg_droppable(type);
    if (queued_bytes > HOP_CTRL_QUEUE_BYTES ||
        (size_t)len > HOP_CTRL_QUEUE_BYTES - queued_bytes)
        return droppable ? HOP_Q_DROP : HOP_Q_OVERFLOW;
    if (droppable) return count >= HOP_CTRL_QUEUE_SOFT ? HOP_Q_DROP : HOP_Q_ADMIT;
    if (count >= HOP_CTRL_QUEUE_HARD) return HOP_Q_OVERFLOW;
    return HOP_Q_ADMIT;
}

int hop_hello_decode(const uint8_t *p, uint32_t len, hop_hello_msg *out) {
    uint32_t off = 0;
    if (!p || !out || len < 1) return 0;
    out->token_len = p[off++];
    if ((uint32_t)out->token_len + 1u > len - off) return 0;   /* token + nlen */
    out->token = p + off;
    off += out->token_len;
    out->name_len = p[off++];
    if ((uint32_t)out->name_len + 9u != len - off || p[len - 1] != HOP_CTRL_PROTO_VER) return 0;
    out->name = p + off;
    off += out->name_len;
    out->width = hop_get_be32(p + off);
    out->height = hop_get_be32(p + off + 4);
    return 1;
}

int hop_token_matches(const char *expected, const uint8_t *got, size_t got_len) {
    size_t want, i;
    unsigned diff;
    if (!expected || !got) return 0;
    want = strlen(expected);
    if (want == 0 || got_len == 0) return 0;
    diff = (unsigned)(want ^ got_len);
    for (i = 0; i < want; ++i) {
        uint8_t g = i < got_len ? got[i] : 0;
        diff |= (unsigned)((uint8_t)expected[i] ^ g);
    }
    return diff == 0;
}

int hop_hello_ack_decode(const uint8_t *p, uint32_t len, int *ok,
                         uint32_t *width, uint32_t *height, hop_session_id *session) {
    uint32_t off;
    if (!p || len < 2) return 0;
    off = 2u + p[1];
    if (off + 9u + HOP_SESSION_BYTES != len || p[off + 8] != HOP_CTRL_PROTO_VER || p[0] != 1) return 0;
    hop_session_id id;
    memcpy(id.bytes, p + off + 9, HOP_SESSION_BYTES);
    if (!hop_session_id_valid(&id)) return 0;
    if (session) *session = id;
    if (ok) *ok = p[0] != 0;
    if (width) *width = hop_get_be32(p + off);
    if (height) *height = hop_get_be32(p + off + 4);
    return 1;
}

int hop_session_id_valid(const hop_session_id *id) {
    unsigned bits = 0;
    if (!id) return 0;
    for (size_t i = 0; i < HOP_SESSION_BYTES; ++i) bits |= id->bytes[i];
    return bits != 0;
}
int hop_session_id_equal(const hop_session_id *a, const hop_session_id *b) {
    return hop_session_id_valid(a) && hop_session_id_valid(b) &&
           memcmp(a->bytes, b->bytes, HOP_SESSION_BYTES) == 0;
}
void hop_data_prefix(uint8_t out[HOP_DATA_PREFIX], const hop_session_id *id, uint32_t batch) {
    memcpy(out, id->bytes, HOP_SESSION_BYTES);
    hop_put_be32(out + HOP_SESSION_BYTES, batch);
}
int hop_data_unwrap(uint8_t type, const uint8_t *p, uint32_t len,
                    const hop_session_id *expected, uint32_t *batch,
                    const uint8_t **body, uint32_t *body_len) {
    if (!p || len < HOP_DATA_PREFIX || !batch || !body || !body_len) return 0;
    hop_session_id id; memcpy(id.bytes, p, HOP_SESSION_BYTES);
    if (!hop_session_id_equal(&id, expected)) return 0;
    uint32_t b = hop_get_be32(p + HOP_SESSION_BYTES);
    if (!b) return 0;
    p += HOP_DATA_PREFIX; len -= HOP_DATA_PREFIX;
    switch (type) {
        case HOP_MSG_FILE_BEGIN: case HOP_MSG_IMAGE_BEGIN:
        case HOP_MSG_FILE_ACK: case HOP_MSG_XFER_DONE: case HOP_MSG_XFER_DONE_ACK: case HOP_MSG_XFER_ABORT:
            if (len < 4 || hop_get_be32(p) != b) return 0;
            break;
        case HOP_MSG_FILE_CHUNK: case HOP_MSG_FILE_END: break;
        default: return 0;
    }
    *batch = b; *body = p; *body_len = len;
    return 1;
}
uint32_t hop_xfer_hello_encode(uint8_t *out, const char *token, const hop_session_id *id) {
    if (!out || !token) return 0;
    size_t n = strlen(token);
    if (n == 0 || n >= HOP_TOKEN_MAX || !hop_session_id_valid(id)) return 0;
    out[0] = HOP_XFER_PROTO_VER; out[1] = (uint8_t)n;
    memcpy(out + 2, token, n); memcpy(out + 2 + n, id->bytes, HOP_SESSION_BYTES);
    return (uint32_t)n + 2 + HOP_SESSION_BYTES;
}
int hop_xfer_hello_bound(const uint8_t *p, uint32_t len, const char *token,
                         const hop_session_id *expected) {
    if (!p || !token || len < 2 + HOP_SESSION_BYTES) return 0;
    size_t n = strlen(token);
    if (p[0] != HOP_XFER_PROTO_VER || len != 2 + n + HOP_SESSION_BYTES ||
        p[1] != n || !hop_token_matches(token, p + 2, n)) return 0;
    hop_session_id id; memcpy(id.bytes, p + 2 + n, HOP_SESSION_BYTES);
    return hop_session_id_equal(&id, expected);
}

void hop_session_hex(const hop_session_id *id, char out[33]) {
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<HOP_SESSION_BYTES;i++) {
        out[i*2]=digits[id->bytes[i]>>4]; out[i*2+1]=digits[id->bytes[i]&15];
    }
    out[32]=0;
}
