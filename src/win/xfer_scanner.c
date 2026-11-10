#include "xfer_scanner.h"

#include "../core/hop.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static char *w2u(const wchar_t *w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return NULL;
    char *out = (char *)malloc((size_t)need);
    if (!out) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL)) {
        free(out);
        return NULL;
    }
    return out;
}

static const wchar_t *base_name(const wchar_t *path) {
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

static uint64_t ft_to_unix(const FILETIME *ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft->dwLowDateTime;
    value.HighPart = ft->dwHighDateTime;
    if (value.QuadPart < 116444736000000000ULL) return 0;
    return (value.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

static int elist_push(elist *list, xfer_entry *entry) {
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
        xfer_entry *next = (xfer_entry *)realloc(list->v, (size_t)next_cap * sizeof(*next));
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

static BOOL guarded_find_next(HANDLE handle, WIN32_FIND_DATAW *data) {
    int guard = hop_watchdog_begin("directory next", HOP_IO_DEADLINE_MS);
    BOOL ok = FindNextFileW(handle, data);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    hop_watchdog_end(guard);
    if (!ok) SetLastError(error);
    return ok;
}

static void walk_into(elist *list, const wchar_t *full, const char *relative) {
    if (!list || list->failed) return;
    if (hop_xfer_task_cancel_requested(list->task.id)) { list->failed = 1; return; }

    WIN32_FILE_ATTRIBUTE_DATA attributes;
    int stat_guard = hop_watchdog_begin("file attributes", HOP_IO_DEADLINE_MS);
    int stat_ok = GetFileAttributesExW(full, GetFileExInfoStandard, &attributes);
    hop_watchdog_end(stat_guard);
    if (!stat_ok) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: source path disappeared or is inaccessible");
        return;
    }
    if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: reparse point rejected from file selection");
        return;
    }

    int is_dir = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const wchar_t *base = base_name(full);
    xfer_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.full = _wcsdup(full);
    entry.name = w2u(base);
    entry.rel = relative ? _strdup(relative) : NULL;
    entry.mtime = ft_to_unix(&attributes.ftLastWriteTime);
    if (!entry.full || !entry.name || !entry.rel) {
        free(entry.full); free(entry.name); free(entry.rel);
        list->failed = 1;
        return;
    }

    if (!is_dir) {
        entry.size = ((uint64_t)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
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

    size_t full_len = wcslen(full);
    wchar_t *pattern = (wchar_t *)malloc((full_len + 3) * sizeof(wchar_t));
    if (!pattern) { list->failed = 1; return; }
    wcscpy(pattern, full);
    if (full_len && pattern[full_len - 1] != L'\\' && pattern[full_len - 1] != L'/')
        wcscat(pattern, L"\\");
    wcscat(pattern, L"*");

    WIN32_FIND_DATAW data;
    int find_guard = hop_watchdog_begin("directory enumerate", HOP_IO_DEADLINE_MS);
    HANDLE handle = FindFirstFileW(pattern, &data);
    hop_watchdog_end(find_guard);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        list->failed = 1;
        hop_log(HOP_LOG_WARN, "xfer: cannot enumerate selected directory");
        return;
    }

    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        size_t parent_len = wcslen(full);
        int need_sep = parent_len && full[parent_len - 1] != L'\\' && full[parent_len - 1] != L'/';
        size_t child_chars = parent_len + (size_t)need_sep + wcslen(data.cFileName) + 1;
        wchar_t *child = (wchar_t *)malloc(child_chars * sizeof(wchar_t));
        if (!child) { list->failed = 1; break; }
        wcscpy(child, full);
        if (need_sep) wcscat(child, L"\\");
        wcscat(child, data.cFileName);

        char *child_name = w2u(data.cFileName);
        char *child_rel = NULL;
        if (child_name) {
            size_t rel_len = strlen(relative), name_len = strlen(child_name);
            child_rel = (char *)malloc(rel_len + name_len + 2);
            if (child_rel) {
                memcpy(child_rel, relative, rel_len);
                child_rel[rel_len] = '/';
                memcpy(child_rel + rel_len + 1, child_name, name_len + 1);
            }
            free(child_name);
        }
        if (!child_rel) list->failed = 1;
        else walk_into(list, child, child_rel);
        free(child_rel);
        free(child);
        if (list->failed) break;
    } while (guarded_find_next(handle, &data));

    if (!list->failed && GetLastError() != ERROR_NO_MORE_FILES) list->failed = 1;
    FindClose(handle);
}

void hop_xfer_scan_paths(elist *list, const hop_xfer_scan_path *paths, int count) {
    if (!list || !paths || count <= 0) {
        if (list) list->failed = 1;
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (!paths[i] || !paths[i][0]) { list->failed = 1; break; }
        const wchar_t *base = base_name(paths[i]);
        char *relative = w2u(base);
        if (!relative) { list->failed = 1; break; }
        walk_into(list, paths[i], relative);
        free(relative);
        if (list->failed) break;
    }
}
