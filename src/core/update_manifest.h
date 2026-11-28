#ifndef HOP_UPDATE_MANIFEST_H
#define HOP_UPDATE_MANIFEST_H

#include <stddef.h>
#include "hop.h"

#define HOP_UPDATE_MANIFEST_URL        "https://raw.githubusercontent.com/alex520lq/HopDesk/updates/latest.txt"
#define HOP_UPDATE_STABLE_MANIFEST_URL "https://raw.githubusercontent.com/alex520lq/HopDesk/updates/stable.txt"
#define HOP_UPDATE_DEV_MANIFEST_URL    "https://raw.githubusercontent.com/alex520lq/HopDesk/updates/dev.txt"
#define HOP_UPDATE_MANIFEST_MAX 8192u
#define HOP_UPDATE_VERSION_MAX  32u
#define HOP_UPDATE_TAG_MAX      40u
#define HOP_UPDATE_URL_MAX      512u
#define HOP_UPDATE_SUMMARY_MAX  160u

typedef struct {
    int  format;
    int  channel;     /* HOP_UPDATE_CHANNEL_STABLE or HOP_UPDATE_CHANNEL_DEV */
    int  ctrl_proto;  /* 0 when legacy format=1 */
    int  xfer_proto;  /* 0 when legacy format=1 */
    char version[HOP_UPDATE_VERSION_MAX];
    char tag[HOP_UPDATE_TAG_MAX];
    char windows_url[HOP_UPDATE_URL_MAX];
    char windows_sha256[65];
    char macos_url[HOP_UPDATE_URL_MAX];
    char macos_sha256[65];
    char notes_url[HOP_UPDATE_URL_MAX];
    char summary[HOP_UPDATE_SUMMARY_MAX];
} hop_update_manifest;

const char *hop_update_manifest_url(int channel);
const char *hop_update_channel_name(int channel);
int hop_update_channel_from_name(const char *name);
int hop_update_channel_from_version(const char *version);

/* Parses the strict key=value manifest (format=1 legacy or format=2 dual-channel).
 * Unknown keys, duplicate keys, malformed versions, arbitrary URLs, channel
 * mismatches and invalid hashes are rejected. No allocation is performed. */
int hop_update_manifest_parse(const char *text, size_t len, hop_update_manifest *out);

/* Compares MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-dev.N strings.
 * Returns -1, 0 or 1. valid is set to zero for malformed version strings. */
int hop_version_compare(const char *a, const char *b, int *valid);

/* Evaluates whether manifest should be offered to the current build.
 * Same-channel updates require strictly newer version (monotonic anti-rollback).
 * Cross-channel switches (stable <-> dev) are allowed in either direction even
 * when the target channel's version is numerically lower. */
int hop_update_evaluate(const char *current_version,
                        int current_channel,
                        int target_channel,
                        const hop_update_manifest *manifest,
                        int *is_channel_switch,
                        int *valid);

#endif
