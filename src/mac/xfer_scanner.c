#include "xfer_scanner.h"

#include "../core/hop.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int elist_push(elist *list, xf_entry *entry) {
    if (!entry || !entry->name || !entry->rel ||
        strlen(entry->name) > HOP_NAME_LEN_MAX ||
        strlen(entry->rel) > HOP_PATH_LEN_MAX ||
        list->n >= HOP_XFER_SCAN_MAX_ENTRIES) {
        list->failed = 1;
        return 0;
    }
    if (!hop_xfer_task_reserve_entry(&list->task, entry->size)) {
        list->budget_rejected = 1;
        list->failed = 1;
        return 0;
    }
    if (list->n == list->cap) {
        int next_cap = list->cap ? list->cap * 2 : 32;
        xf_entry *next = (xf_entry *)realloc(list->v, (size_t)next_cap * sizeof(*next));
        if (!next) { list->failed = 1; return 0; }
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
    hop_xfer_task_dispose(&list->task);
}

static struct dirent *guarded_readdir(DIR *directory) {
    int guard = hop_watchdog_begin("directory next", HOP_IO_DEADLINE_MS);
    struct dirent *entry = readdir(directory);
    hop_watchdog_end(guard);
    return entry;
}

static void walk_into(elist *list, const char *full, const char *relative) {
    if (!list || list->failed) return;
    if (hop_xfer_task_cancel_requested(list->task.id)) { list->failed = 1; return; }

    struct stat st;
    int stat_guard = hop_watchdog_begin("file attributes", HOP_IO_DEADLINE_MS);
    int stat_ok = lstat(full, &st);
    hop_watchdog_end(stat_guard);
    if (stat_ok != 0) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: source path disappeared or is inaccessible");
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: symbolic link rejected from file selection");
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
        list->failed = 1;
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {
            free(entry.full); free(entry.name); free(entry.rel);
            list->failed = 1;
            hop_log(HOP_LOG_WARN, "xfer: non-regular source entry rejected");
            return;
        }
        entry.size = (uint64_t)st.st_size;
        entry.flags = 0;
        if (!elist_push(list, &entry)) {
            free(entry.full); free(entry.name); free(entry.rel);
        }
        return;
    }

    entry.size = 0;
    entry.flags = HOP_FILE_FLAG_DIR;
    if (!elist_push(list, &entry)) {
        free(entry.full); free(entry.name); free(entry.rel);
        return;
    }

    int open_guard = hop_watchdog_begin("directory open", HOP_IO_DEADLINE_MS);
    DIR *directory = opendir(full);
    hop_watchdog_end(open_guard);
    if (!directory) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: cannot enumerate selected directory");
        return;
    }

    struct dirent *de;
    while ((de = guarded_readdir(directory)) != NULL) {
        if (list->failed) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        size_t full_len = strlen(full), name_len = strlen(de->d_name), rel_len = strlen(relative);
        char *child = (char *)malloc(full_len + name_len + 2);
        char *child_rel = (char *)malloc(rel_len + name_len + 2);
        if (!child || !child_rel) {
            free(child); free(child_rel);
            list->failed = 1;
            break;
        }
        snprintf(child, full_len + name_len + 2, "%s/%s", full, de->d_name);
        snprintf(child_rel, rel_len + name_len + 2, "%s/%s", relative, de->d_name);
        walk_into(list, child, child_rel);
        free(child);
        free(child_rel);
        if (list->failed) break;
    }
    closedir(directory);
}

void hop_xfer_scan_paths(elist *list, const hop_xfer_scan_path *paths, int count) {
    if (!list || !paths || count <= 0) {
        if (list) list->failed = 1;
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (!paths[i] || !paths[i][0]) { list->failed = 1; break; }
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        walk_into(list, paths[i], base);
        if (list->failed) break;
    }
}
