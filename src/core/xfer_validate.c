/* =====================================================================
 * HopDesk - portable validation for untrusted data-channel frames.
 * Keep Windows and macOS acceptance rules identical and testable.
 * ===================================================================== */
#include "hop.h"
#include <string.h>

int hop_xfer_hello_valid(const uint8_t *p, uint32_t len, const char *token) {
    if (!p || !token || len < 2) return 0;
    size_t want = strlen(token);
    if (want > HOP_TOKEN_MAX || want > 255u) return 0;
    return p[0] == HOP_XFER_PROTO_VER &&
           p[1] == (uint8_t)want &&
           len == 2u + (uint32_t)want &&
           (want == 0 || memcmp(p + 2, token, want) == 0);
}

int hop_xfer_relpath_safe(const char *rel) {
    if (!rel || !*rel) return 0;
    /* Protocol paths are UTF-8 and always use '/'; reject platform-specific separators. */
    if (rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':') || strchr(rel, '\\')) return 0;

    const char *p = rel;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') {
            if ((unsigned char)*p < 0x20u) return 0;
            ++p;
        }
        size_t n = (size_t)(p - seg);
        if (n == 0 || (n == 1 && seg[0] == '.') ||
            (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
        if (*p) {
            ++p;
            if (!*p) return 0;
        }
    }
    return 1;
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
