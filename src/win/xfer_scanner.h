#ifndef HOP_XFER_SCANNER_H
#define HOP_XFER_SCANNER_H

#include <stdint.h>

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

typedef enum {
    HOP_XFER_SCAN_OK = 0,
    HOP_XFER_SCAN_INVALID_INPUT,
    HOP_XFER_SCAN_CANCELLED,
    HOP_XFER_SCAN_SOURCE_UNAVAILABLE,
    HOP_XFER_SCAN_LINK_REJECTED,
    HOP_XFER_SCAN_UNSUPPORTED_ENTRY,
    HOP_XFER_SCAN_PATH_TOO_LONG,
    HOP_XFER_SCAN_ENTRY_LIMIT,
    HOP_XFER_SCAN_BUDGET_REJECTED,
    HOP_XFER_SCAN_ENUMERATION_FAILED,
    HOP_XFER_SCAN_OUT_OF_MEMORY
} hop_xfer_scan_error;

/* The result is an owned, complete snapshot. It contains no Task lifecycle. */
typedef struct {
    hop_xfer_scan_entry_array v;
    int n, cap;
    int failed;
    int budget_rejected;
    hop_xfer_scan_error error;
} elist;

/* Scanner dependencies are explicit; Task ownership remains with the caller. */
typedef struct {
    void *context;
    int (*reserve_entry)(void *context, uint64_t bytes);
    int (*is_cancelled)(void *context);
    int (*watchdog_begin)(void *context, const char *operation, unsigned timeout_ms);
    void (*watchdog_end)(void *context, int ticket);
} hop_xfer_scan_services;

/* Scans all roots atomically: any error invalidates the whole snapshot. */
void hop_xfer_scan_paths(elist *list, const hop_xfer_scan_path *paths, int count,
                         const hop_xfer_scan_services *services);

/* Releases only snapshot-owned memory; never disposes the caller's Task. */
void elist_free(elist *list);

#endif
