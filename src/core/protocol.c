/* =====================================================================
 *  HopDesk - 帧编解码（平台无关）
 *
 *  帧 = type(1) + length(4, BE) + payload(length)
 * ===================================================================== */
#include "hop.h"
#include <stdlib.h>
#include <string.h>

/* 单帧上限：文本 + 文件头 + 文件体。超过这个长度视为坏帧，丢弃重同步。 */
#define HOP_MAX_PAYLOAD (HOP_FILE_MAX + 1024)

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
uint16_t hop_get_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
uint32_t hop_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
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
};

hop_decoder *hop_decoder_new(void *user, hop_on_message cb) {
    hop_decoder *d = (hop_decoder *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->user = user;
    d->cb = cb;
    return d;
}

void hop_decoder_reset(hop_decoder *d) {
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

void hop_decoder_feed(hop_decoder *d, const uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = buf[i];
        if (d->hdr_have < HOP_HEADER_SIZE) {
            d->hdr[d->hdr_have++] = b;
            if (d->hdr_have == HOP_HEADER_SIZE) {
                d->body_len = hop_get_be32(d->hdr + 1);
                if (d->body_len > HOP_MAX_PAYLOAD) {
                    /* 异常帧：丢弃，重新同步 */
                    hop_decoder_reset(d);
                    continue;
                }
                if (d->body_len == 0) {
                    d->body = NULL;
                    emit(d);
                } else {
                    d->body = (uint8_t *)malloc(d->body_len);
                    if (!d->body) { hop_decoder_reset(d); return; }
                }
            }
        } else {
            d->body[d->body_have++] = b;
            if (d->body_have >= d->body_len) emit(d);
        }
    }
}
