#include "update_manifest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SEEN_FORMAT       (1u << 0)
#define SEEN_VERSION      (1u << 1)
#define SEEN_TAG          (1u << 2)
#define SEEN_WINDOWS_URL  (1u << 3)
#define SEEN_WINDOWS_HASH (1u << 4)
#define SEEN_MACOS_URL    (1u << 5)
#define SEEN_MACOS_HASH   (1u << 6)
#define SEEN_NOTES_URL    (1u << 7)
#define SEEN_ALL          0xFFu

static int parse_version(const char *s, uint32_t parts[3]) {
    const char *p = s;
    if (!s || !*s) return 0;
    for (int i = 0; i < 3; ++i) {
        uint64_t value = 0;
        const char *start = p;
        if (*p < '0' || *p > '9') return 0;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') return 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (unsigned)(*p - '0');
            if (value > UINT32_MAX) return 0;
            ++p;
        }
        if (p == start) return 0;
        parts[i] = (uint32_t)value;
        if (i < 2) {
            if (*p != '.') return 0;
            ++p;
        } else if (*p != '\0') {
            return 0;
        }
    }
    return 1;
}

int hop_version_compare(const char *a, const char *b, int *valid) {
    uint32_t av[3], bv[3];
    if (valid) *valid = 0;
    if (!parse_version(a, av) || !parse_version(b, bv)) return 0;
    if (valid) *valid = 1;
    for (int i = 0; i < 3; ++i) {
        if (av[i] < bv[i]) return -1;
        if (av[i] > bv[i]) return 1;
    }
    return 0;
}

static int copy_value(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x21 || c > 0x7E) return 0;
    }
    memcpy(dst, src, n + 1);
    return 1;
}

static int valid_sha256(const char *s) {
    if (strlen(s) != 64) return 0;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

int hop_update_manifest_parse(const char *text, size_t len, hop_update_manifest *out) {
    char buffer[HOP_UPDATE_MANIFEST_MAX + 1];
    unsigned seen = 0;
    char expected[HOP_UPDATE_URL_MAX];

    if (!text || !out || len == 0 || len > HOP_UPDATE_MANIFEST_MAX ||
        memchr(text, '\0', len) != NULL) return 0;

    memcpy(buffer, text, len);
    buffer[len] = '\0';
    memset(out, 0, sizeof(*out));

    char *line = buffer;
    while (line < buffer + len) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        size_t line_len = strlen(line);
        if (line_len && line[line_len - 1] == '\r') line[--line_len] = '\0';
        if (line_len) {
            char *eq = strchr(line, '=');
            if (!eq || eq == line || !eq[1]) return 0;
            *eq++ = '\0';

            unsigned bit = 0;
            char *dst = NULL;
            size_t cap = 0;
            if (strcmp(line, "format") == 0) {
                bit = SEEN_FORMAT;
                if (strcmp(eq, "1") != 0) return 0;
            } else if (strcmp(line, "version") == 0) {
                bit = SEEN_VERSION; dst = out->version; cap = sizeof(out->version);
            } else if (strcmp(line, "tag") == 0) {
                bit = SEEN_TAG; dst = out->tag; cap = sizeof(out->tag);
            } else if (strcmp(line, "windows_url") == 0) {
                bit = SEEN_WINDOWS_URL; dst = out->windows_url; cap = sizeof(out->windows_url);
            } else if (strcmp(line, "windows_sha256") == 0) {
                bit = SEEN_WINDOWS_HASH; dst = out->windows_sha256; cap = sizeof(out->windows_sha256);
            } else if (strcmp(line, "macos_url") == 0) {
                bit = SEEN_MACOS_URL; dst = out->macos_url; cap = sizeof(out->macos_url);
            } else if (strcmp(line, "macos_sha256") == 0) {
                bit = SEEN_MACOS_HASH; dst = out->macos_sha256; cap = sizeof(out->macos_sha256);
            } else if (strcmp(line, "notes_url") == 0) {
                bit = SEEN_NOTES_URL; dst = out->notes_url; cap = sizeof(out->notes_url);
            } else {
                return 0;
            }
            if (seen & bit) return 0;
            if (dst && !copy_value(dst, cap, eq)) return 0;
            seen |= bit;
        }
        if (!end) break;
        line = end + 1;
    }

    if (seen != SEEN_ALL) return 0;

    uint32_t version_parts[3];
    if (!parse_version(out->version, version_parts)) return 0;
    if (snprintf(expected, sizeof(expected), "v%s", out->version) <= 0 ||
        strcmp(out->tag, expected) != 0) return 0;

    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk-Releases/releases/download/%s/HopDesk-Windows.zip",
                 out->tag) <= 0 || strcmp(out->windows_url, expected) != 0) return 0;
    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk-Releases/releases/download/%s/HopDesk-macOS.zip",
                 out->tag) <= 0 || strcmp(out->macos_url, expected) != 0) return 0;
    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk-Releases/releases/tag/%s",
                 out->tag) <= 0 || strcmp(out->notes_url, expected) != 0) return 0;

    return valid_sha256(out->windows_sha256) && valid_sha256(out->macos_sha256);
}
