#ifndef HOP_UPDATE_MANIFEST_H
#define HOP_UPDATE_MANIFEST_H

#include <stddef.h>

#define HOP_UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/alex520lq/HopDesk-Releases/main/latest.txt"
#define HOP_UPDATE_MANIFEST_MAX 8192u
#define HOP_UPDATE_VERSION_MAX 32u
#define HOP_UPDATE_TAG_MAX 40u
#define HOP_UPDATE_URL_MAX 512u

typedef struct {
    char version[HOP_UPDATE_VERSION_MAX];
    char tag[HOP_UPDATE_TAG_MAX];
    char windows_url[HOP_UPDATE_URL_MAX];
    char windows_sha256[65];
    char macos_url[HOP_UPDATE_URL_MAX];
    char macos_sha256[65];
    char notes_url[HOP_UPDATE_URL_MAX];
} hop_update_manifest;

/* Parses the deliberately small, strict key=value manifest used by releases.
 * Unknown keys, duplicate keys, malformed versions, arbitrary URLs and invalid
 * hashes are rejected. No allocation is performed. */
int hop_update_manifest_parse(const char *text, size_t len, hop_update_manifest *out);

/* Returns -1, 0 or 1. valid is set to zero for malformed version strings. */
int hop_version_compare(const char *a, const char *b, int *valid);

#endif
