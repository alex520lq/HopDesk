#include "xfer_scanner.h"

#include "../core/hop.h"
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void scan_fail(elist *list, hop_xfer_scan_error error) {
    if (!list) return;
    if (!list->failed) list->error = error;
    list->failed = 1;
}

static int scan_watchdog_begin(const hop_xfer_scan_services *services, const char *operation) {
    return services->watchdog_begin(services->context, operation, HOP_IO_DEADLINE_MS);
}

static void scan_watchdog_end(const hop_xfer_scan_services *services, int ticket) {
    services->watchdog_end(services->context, ticket);
}

static int elist_push(elist *list, xf_entry *entry, const hop_xfer_scan_services *services) {
    if (!entry || !entry->name || !entry->rel) {
        scan_fail(list, HOP_XFER_SCAN_OUT_OF_MEMORY);
        return 0;
    }
    if (strlen(entry->name) > HOP_NAME_LEN_MAX ||
        strlen(entry->rel) > HOP_PATH_LEN_MAX) {
        scan_fail(list, HOP_XFER_SCAN_PATH_TOO_LONG);
        return 0;
    }
    if (list->n >= HOP_XFER_SCAN_MAX_ENTRIES) {
        scan_fail(list, HOP_XFER_SCAN_ENTRY_LIMIT);
        return 0;
    }
    if (!services->reserve_entry(services->context, entry->size)) {
        list->budget_rejected = 1;
        scan_fail(list, HOP_XFER_SCAN_BUDGET_REJECTED);
        return 0;
    }
    if (list->n == list->cap) {
        int next_cap = list->cap ? list->cap * 2 : 32;
        xf_entry *next = (xf_entry *)realloc(list->v, (size_t)next_cap * sizeof(*next));
        if (!next) { scan_fail(list, HOP_XFER_SCAN_OUT_OF_MEMORY); return 0; }
        list->v = next;
        list->cap = next_cap;
    }
    list->v[list->n++] = *entry;
    return 1;
}

void elist_free(elist *list) {
    if (!list) return;
    for (int i = 0; i < list->n; ++i) {
        free(list->v[i].full);
        free(list->v[i].name);
        free(list->v[i].rel);
    }
    free(list->v);
    list->v = NULL;
    list->n = list->cap = 0;
    list->failed = 0;
    list->budget_rejected = 0;
    list->error = HOP_XFER_SCAN_OK;
}

static struct dirent *guarded_readdir(DIR *directory,
                                      const hop_xfer_scan_services *services,
                                      int *read_error) {
    errno = 0;
    int guard = scan_watchdog_begin(services, "directory next");
    struct dirent *entry = readdir(directory);
    int error = errno;
    scan_watchdog_end(services, guard);
    if (!entry && error && read_error) *read_error = error;
    return entry;
}

static void walk_into(elist *list, const char *full, const char *relative, const hop_xfer_scan_services *services) {
    if (!list || list->failed) return;
    if (services->is_cancelled(services->context)) { scan_fail(list, HOP_XFER_SCAN_CANCELLED); return; }

    struct stat st;
    int stat_guard = scan_watchdog_begin(services, "file attributes");
    int stat_ok = lstat(full, &st);
    scan_watchdog_end(services, stat_guard);
    if (stat_ok != 0) {
        scan_fail(list, HOP_XFER_SCAN_SOURCE_UNAVAILABLE);
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        scan_fail(list, HOP_XFER_SCAN_LINK_REJECTED);
        return;
    }

    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;
    xf_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.full = strdup(full);
    entry.name = strdup(base);
    entry.rel = relative ? strdup(relative) : NULL;
    entry.mtime = (uint64_t)st.st_mtime;
    if (!entry.full || !entry.name || !entry.rel) {
        free(entry.full); free(entry.name); free(entry.rel);
        scan_fail(list, HOP_XFER_SCAN_OUT_OF_MEMORY);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {
            free(entry.full); free(entry.name); free(entry.rel);
            scan_fail(list, HOP_XFER_SCAN_UNSUPPORTED_ENTRY);
            return;
        }
        entry.size = (uint64_t)st.st_size;
        entry.flags = 0;
        if (!elist_push(list, &entry, services)) {
            free(entry.full); free(entry.name); free(entry.rel);
        }
        return;
    }

    entry.size = 0;
    entry.flags = HOP_FILE_FLAG_DIR;
    if (!elist_push(list, &entry, services)) {
        free(entry.full); free(entry.name); free(entry.rel);
        return;
    }

    int open_guard = scan_watchdog_begin(services, "directory open");
    DIR *directory = opendir(full);
    scan_watchdog_end(services, open_guard);
    if (!directory) {
        scan_fail(list, HOP_XFER_SCAN_ENUMERATION_FAILED);
        return;
    }

    struct dirent *de;
    int read_error = 0;
    while ((de = guarded_readdir(directory, services, &read_error)) != NULL) {
        if (list->failed) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        size_t full_len = strlen(full), name_len = strlen(de->d_name), rel_len = strlen(relative);
        char *child = (char *)malloc(full_len + name_len + 2);
        char *child_rel = (char *)malloc(rel_len + name_len + 2);
        if (!child || !child_rel) {
            free(child); free(child_rel);
            scan_fail(list, HOP_XFER_SCAN_OUT_OF_MEMORY);
            break;
        }
        snprintf(child, full_len + name_len + 2, "%s/%s", full, de->d_name);
        snprintf(child_rel, rel_len + name_len + 2, "%s/%s", relative, de->d_name);
        walk_into(list, child, child_rel, services);
        free(child);
        free(child_rel);
        if (list->failed) break;
    }
    closedir(directory);
    if (!list->failed && read_error)
        scan_fail(list, HOP_XFER_SCAN_ENUMERATION_FAILED);
}

void hop_xfer_scan_paths(elist *list, const hop_xfer_scan_path *paths, int count,
                         const hop_xfer_scan_services *services) {
    if (!list || !paths || count <= 0 || !services || !services->reserve_entry ||
        !services->is_cancelled || !services->watchdog_begin || !services->watchdog_end) {
        if (list) scan_fail(list, HOP_XFER_SCAN_INVALID_INPUT);
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (!paths[i] || !paths[i][0]) { scan_fail(list, HOP_XFER_SCAN_INVALID_INPUT); break; }
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        walk_into(list, paths[i], base, services);
        if (list->failed) break;
    }
}
