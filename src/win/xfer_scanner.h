#ifndef HOP_XFER_SCANNER_H
#define HOP_XFER_SCANNER_H

#include <stdint.h>
#include "../core/xfer_task.h"

#define HOP_XFER_SCAN_MAX_ENTRIES 8192

#ifdef _WIN32
#include <wchar.h>
typedef struct {
    wchar_t *full;
    char *name;
    char *rel;
    uint64_t size;
    uint64_t mtime;
    uint32_t flags;
} xfer_entry;
typedef xfer_entry hop_xfer_scan_entry;
typedef xfer_entry *hop_xfer_scan_entry_array;
typedef const wchar_t *hop_xfer_scan_path;
#else
typedef struct {
    char *full;
    char *name;
    char *rel;
    uint64_t size;
    uint64_t mtime;
    uint32_t flags;
} xf_entry;
typedef xf_entry hop_xfer_scan_entry;
typedef xf_entry *hop_xfer_scan_entry_array;
typedef const char *hop_xfer_scan_path;
#endif

typedef struct {
    hop_xfer_scan_entry_array v;
    int n, cap;
    int failed;
    int budget_rejected;
    hop_xfer_task task;
} elist;

/* Scans all requested roots into a complete snapshot. Any unsafe, missing,
 * cancelled or budget-rejected entry marks the entire snapshot failed. */
void hop_xfer_scan_paths(elist *list, const hop_xfer_scan_path *paths, int count);

/* Releases snapshot entries and its embedded Task handle. */
void elist_free(elist *list);

#endif
