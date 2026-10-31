/* =====================================================================
 * HopDesk - portable validation for untrusted data-channel frames.
 * Keep Windows and macOS acceptance rules identical and testable.
 * ===================================================================== */
#include "hop.h"
#include <string.h>

int hop_xfer_decode_file_begin(const uint8_t *p, uint32_t len,
                               hop_xfer_file_begin_msg *out) {
    if (!p || !out || len < 29u) return 0;
    uint16_t nlen = hop_get_be16(p + 25);
    if (nlen == 0 || nlen > HOP_NAME_LEN_MAX) return 0;
    if (27u + (uint32_t)nlen + 2u > len) return 0;
    uint16_t plen = hop_get_be16(p + 27u + nlen);
    if (plen == 0 || plen > HOP_PATH_LEN_MAX) return 0;
    if (29u + (uint32_t)nlen + (uint32_t)plen != len) return 0;

    out->batch = hop_get_be32(p);
    out->id = hop_get_be32(p + 4);
    out->size = hop_get_be64(p + 8);
    out->mtime = hop_get_be64(p + 16);
    out->flags = p[24];
    out->name_len = nlen;
    out->name = p + 27;
    out->relpath_len = plen;
    out->relpath = p + 29u + nlen;
    return 1;
}

int hop_xfer_decode_image_begin(const uint8_t *p, uint32_t len,
                                hop_xfer_image_begin_msg *out) {
    if (!p || !out || len != 20u) return 0;
    out->batch = hop_get_be32(p);
    out->id = hop_get_be32(p + 4);
    out->width = hop_get_be32(p + 8);
    out->height = hop_get_be32(p + 12);
    out->stride = hop_get_be32(p + 16);
    return 1;
}

int hop_xfer_hello_valid(const uint8_t *p, uint32_t len, const char *token) {
    if (!p || !token || len < 2) return 0;
    size_t want = strlen(token);
    if (want > HOP_TOKEN_MAX || want > 255u) return 0;
    return p[0] == HOP_XFER_PROTO_VER &&
           p[1] == (uint8_t)want &&
           len == 2u + (uint32_t)want &&
           (want == 0 || memcmp(p + 2, token, want) == 0);
}

int hop_xfer_relpath_bytes_safe(const uint8_t *rel, size_t len) {
    if (!rel || len == 0 || len > HOP_PATH_LEN_MAX) return 0;
    if (rel[0] == '/' || rel[0] == '\\') return 0;

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && rel[i] != '/') {
            if (rel[i] == '\\' || rel[i] == ':' || rel[i] == 0 ||
                rel[i] < 0x20u) return 0;
            ++i;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && rel[start] == '.') ||
            (n == 2 && rel[start] == '.' && rel[start + 1] == '.')) return 0;
        if (i < len) {
            ++i;
            if (i == len) return 0;
        }
    }
    return 1;
}

int hop_xfer_relpath_safe(const char *rel) {
    return rel ? hop_xfer_relpath_bytes_safe((const uint8_t *)rel, strlen(rel)) : 0;
}

int hop_clip_should_apply_remote(uint32_t origin, uint32_t local_device) {
    return origin != local_device;
}

uint32_t hop_xfer_file_crc_step(uint32_t seed, uint32_t chunk_crc) {
    uint8_t encoded[4];
    hop_put_be32(encoded, chunk_crc);
    return hop_crc32(seed, encoded, sizeof(encoded));
}

int hop_xfer_image_valid(int32_t width, int32_t height, int32_t stride) {
    if (width <= 0 || height <= 0 ||
        width > HOP_IMG_MAX_SIDE || height > HOP_IMG_MAX_SIDE) return 0;
    uint64_t min_stride = (uint64_t)(uint32_t)width * 4u;
    if (stride <= 0 || (uint64_t)(uint32_t)stride < min_stride) return 0;
    return (uint64_t)(uint32_t)stride * (uint64_t)(uint32_t)height <= HOP_IMG_MAX_BYTES;
}

int hop_xfer_chunk_valid(const uint8_t *p, uint32_t len,
                         uint64_t declared_size, int is_image) {
    if (!p || len < 20u) return 0;
    uint64_t off = hop_get_be64(p + 4);
    uint32_t n = hop_get_be32(p + 12);
    if (n == 0 || (uint64_t)n != (uint64_t)len - 20u) return 0;
    if (off > UINT64_MAX - n) return 0;
    uint64_t end = off + n;
    if (is_image) return end <= HOP_IMG_MAX_BYTES;
    return end <= declared_size;
}
