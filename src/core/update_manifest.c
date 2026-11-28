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
#define SEEN_CHANNEL      (1u << 8)
#define SEEN_CTRL_PROTO   (1u << 9)
#define SEEN_XFER_PROTO   (1u << 10)
#define SEEN_SUMMARY      (1u << 11)

#define SEEN_V1_REQUIRED  0xFFu
#define SEEN_V2_REQUIRED  (SEEN_V1_REQUIRED | SEEN_CHANNEL | SEEN_CTRL_PROTO | SEEN_XFER_PROTO)

typedef struct {
    uint32_t parts[3];
    int      is_dev;
    uint32_t dev_num;
} parsed_version;

static int parse_u32_strict(const char *s, const char **end_out, uint32_t *out, int allow_zero) {
    uint64_t value = 0;
    const char *p = s;
    if (!p || *p < '0' || *p > '9') return 0;
    if (*p == '0') {
        if (!allow_zero) return 0;
        if (p[1] >= '0' && p[1] <= '9') return 0;
    }
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (unsigned)(*p - '0');
        if (value > UINT32_MAX) return 0;
        ++p;
    }
    if (p == s) return 0;
    *out = (uint32_t)value;
    if (end_out) *end_out = p;
    return 1;
}

static int parse_version_full(const char *s, parsed_version *out) {
    const char *p = s;
    if (!s || !*s || !out) return 0;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 3; ++i) {
        if (!parse_u32_strict(p, &p, &out->parts[i], 1)) return 0;
        if (i < 2) {
            if (*p != '.') return 0;
            ++p;
        }
    }
    if (*p == '\0') {
        out->is_dev = 0;
        out->dev_num = 0;
        return 1;
    }
    if (strncmp(p, "-dev.", 5) == 0) {
        p += 5;
        if (!parse_u32_strict(p, &p, &out->dev_num, 0) || *p != '\0') return 0;
        out->is_dev = 1;
        return 1;
    }
    return 0;
}

const char *hop_update_manifest_url(int channel) {
    return channel == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_DEV_MANIFEST_URL
        : HOP_UPDATE_STABLE_MANIFEST_URL;
}

const char *hop_update_channel_name(int channel) {
    return channel == HOP_UPDATE_CHANNEL_DEV ? "dev" : "stable";
}

int hop_update_channel_from_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "stable") == 0) return HOP_UPDATE_CHANNEL_STABLE;
    if (strcmp(name, "dev") == 0) return HOP_UPDATE_CHANNEL_DEV;
    return -1;
}

int hop_update_channel_from_version(const char *version) {
    parsed_version pv;
    if (!parse_version_full(version, &pv)) return -1;
    return pv.is_dev ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
}

int hop_version_compare(const char *a, const char *b, int *valid) {
    parsed_version av, bv;
    if (valid) *valid = 0;
    if (!parse_version_full(a, &av) || !parse_version_full(b, &bv)) return 0;
    if (valid) *valid = 1;
    for (int i = 0; i < 3; ++i) {
        if (av.parts[i] < bv.parts[i]) return -1;
        if (av.parts[i] > bv.parts[i]) return 1;
    }
    /* SemVer precedence: X.Y.Z-dev.N < X.Y.Z */
    if (av.is_dev != bv.is_dev) return av.is_dev ? -1 : 1;
    if (av.is_dev) {
        if (av.dev_num < bv.dev_num) return -1;
        if (av.dev_num > bv.dev_num) return 1;
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

static int copy_summary(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7F) return 0;
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

static int parse_proto_int(const char *s, int *out) {
    uint32_t v = 0;
    const char *end = NULL;
    if (!parse_u32_strict(s, &end, &v, 0) || *end != '\0' || v > 65535u) return 0;
    *out = (int)v;
    return 1;
}

int hop_update_manifest_parse(const char *text, size_t len, hop_update_manifest *out) {
    char buffer[HOP_UPDATE_MANIFEST_MAX + 1];
    unsigned seen = 0;
    char expected[HOP_UPDATE_URL_MAX];
    char channel_buf[16] = {0};
    char ctrl_buf[16] = {0};
    char xfer_buf[16] = {0};
    int fmt = 0;

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
            int is_summary = 0;
            if (strcmp(line, "format") == 0) {
                bit = SEEN_FORMAT;
                if (strcmp(eq, "1") == 0) fmt = 1;
                else if (strcmp(eq, "2") == 0) fmt = 2;
                else return 0;
            } else if (strcmp(line, "channel") == 0) {
                bit = SEEN_CHANNEL; dst = channel_buf; cap = sizeof(channel_buf);
            } else if (strcmp(line, "ctrl_proto") == 0) {
                bit = SEEN_CTRL_PROTO; dst = ctrl_buf; cap = sizeof(ctrl_buf);
            } else if (strcmp(line, "xfer_proto") == 0) {
                bit = SEEN_XFER_PROTO; dst = xfer_buf; cap = sizeof(xfer_buf);
            } else if (strcmp(line, "summary") == 0) {
                bit = SEEN_SUMMARY; dst = out->summary; cap = sizeof(out->summary); is_summary = 1;
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
            if (dst) {
                if (is_summary ? !copy_summary(dst, cap, eq) : !copy_value(dst, cap, eq))
                    return 0;
            }
            seen |= bit;
        }
        if (!end) break;
        line = end + 1;
    }

    if (fmt == 1) {
        if (seen != SEEN_V1_REQUIRED) return 0;
        out->format = 1;
        out->channel = HOP_UPDATE_CHANNEL_STABLE;
        out->ctrl_proto = 0;
        out->xfer_proto = 0;
    } else if (fmt == 2) {
        if ((seen & SEEN_V2_REQUIRED) != SEEN_V2_REQUIRED) return 0;
        out->format = 2;
        out->channel = hop_update_channel_from_name(channel_buf);
        if (out->channel < 0) return 0;
        if (!parse_proto_int(ctrl_buf, &out->ctrl_proto) ||
            !parse_proto_int(xfer_buf, &out->xfer_proto)) return 0;
    } else {
        return 0;
    }

    parsed_version pv;
    if (!parse_version_full(out->version, &pv)) return 0;
    int version_channel = pv.is_dev ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
    if (version_channel != out->channel) return 0;

    if (snprintf(expected, sizeof(expected), "v%s", out->version) <= 0 ||
        strcmp(out->tag, expected) != 0) return 0;

    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk/releases/download/%s/HopDesk-Windows.zip",
                 out->tag) <= 0 || strcmp(out->windows_url, expected) != 0) return 0;
    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk/releases/download/%s/HopDesk-macOS.zip",
                 out->tag) <= 0 || strcmp(out->macos_url, expected) != 0) return 0;
    if (snprintf(expected, sizeof(expected),
                 "https://github.com/alex520lq/HopDesk/releases/tag/%s",
                 out->tag) <= 0 || strcmp(out->notes_url, expected) != 0) return 0;

    return valid_sha256(out->windows_sha256) && valid_sha256(out->macos_sha256);
}

int hop_update_evaluate(const char *current_version,
                        int current_channel,
                        int target_channel,
                        const hop_update_manifest *manifest,
                        int *is_channel_switch,
                        int *valid) {
    if (is_channel_switch) *is_channel_switch = 0;
    if (valid) *valid = 0;
    if (!current_version || !manifest) return 0;
    if ((current_channel != HOP_UPDATE_CHANNEL_STABLE && current_channel != HOP_UPDATE_CHANNEL_DEV) ||
        (target_channel != HOP_UPDATE_CHANNEL_STABLE && target_channel != HOP_UPDATE_CHANNEL_DEV))
        return 0;
    if (manifest->channel != target_channel) return 0;

    int cmp_valid = 0;
    int cmp = hop_version_compare(current_version, manifest->version, &cmp_valid);
    if (!cmp_valid) return 0;
    if (valid) *valid = 1;

    if (current_channel != target_channel) {
        if (is_channel_switch) *is_channel_switch = 1;
        return 1;
    }
    return cmp < 0 ? 1 : 0;
}
