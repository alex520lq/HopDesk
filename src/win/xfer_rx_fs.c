#include "xfer_rx_fs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *alloc_extended_path(const wchar_t *path) {
    if (!path) return NULL;
    size_t len = wcslen(path);
    if (len < MAX_PATH - 12 || (len >= 4 && wcsncmp(path, L"\\\\?\\", 4) == 0))
        return NULL;
    if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\') {
        wchar_t *out = (wchar_t *)malloc((len + 5u) * sizeof(wchar_t));
        if (!out) return NULL;
        memcpy(out, L"\\\\?\\", 4u * sizeof(wchar_t));
        memcpy(out + 4u, path, (len + 1u) * sizeof(wchar_t));
        return out;
    }
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.') {
        wchar_t *out = (wchar_t *)malloc((len + 7u) * sizeof(wchar_t));
        if (!out) return NULL;
        memcpy(out, L"\\\\?\\UNC\\", 8u * sizeof(wchar_t));
        memcpy(out + 8u, path + 2u, (len - 1u) * sizeof(wchar_t));
        return out;
    }
    return NULL;
}

wchar_t *hop_xfer_rx_fs_path_for(const wchar_t *root, const char *relative_utf8) {
    if (!root || !relative_utf8) return NULL;
    int length = (int)strlen(relative_utf8);
    if (length <= 0) return NULL;
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, relative_utf8, length, NULL, 0);
    if (wide_length <= 0) return NULL;
    wchar_t *relative = (wchar_t *)malloc(((size_t)wide_length + 1) * sizeof(wchar_t));
    if (!relative) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, relative_utf8, length, relative, wide_length)) {
        free(relative);
        return NULL;
    }
    relative[wide_length] = L'\0';
    for (wchar_t *p = relative; *p; ++p) if (*p == L'/') *p = L'\\';

    size_t root_length = wcslen(root);
    size_t total = root_length + (root_length && root[root_length - 1] != L'\\' ? 1u : 0u) +
                   (size_t)wide_length;
    if (total >= 32767u) { free(relative); return NULL; }
    wchar_t *path = (wchar_t *)malloc((total + 1u) * sizeof(wchar_t));
    if (!path) { free(relative); return NULL; }
    memcpy(path, root, (root_length + 1u) * sizeof(wchar_t));
    size_t offset = root_length;
    if (root_length && path[root_length - 1] != L'\\') path[offset++] = L'\\';
    memcpy(path + offset, relative, ((size_t)wide_length + 1u) * sizeof(wchar_t));
    free(relative);
    return path;
}

int hop_xfer_rx_fs_create_directory_if_missing(const wchar_t *path) {
    if (!path) return 0;
    wchar_t *extended = alloc_extended_path(path);
    const wchar_t *target = extended ? extended : path;
    int ok = 0;
    if (CreateDirectoryW(target, NULL)) {
        ok = 1;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(target);
        ok = attributes != INVALID_FILE_ATTRIBUTES &&
             (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    free(extended);
    return ok;
}

int hop_xfer_rx_fs_ensure_dirs(wchar_t *path) {
    if (!path || !*path) return 0;
    wchar_t *p = path;
    if (((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z')) && p[1] == L':') {
        p += 2;
        if (*p == L'\\' || *p == L'/') ++p;
    } else if (p[0] == L'\\' && p[1] == L'\\') {
        p += 2;
        while (*p && *p != L'\\' && *p != L'/') ++p;
        if (*p) ++p;
        while (*p && *p != L'\\' && *p != L'/') ++p;
        if (*p) ++p;
    }
    for (; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t save = *p;
            *p = L'\0';
            int ok = hop_xfer_rx_fs_create_directory_if_missing(path);
            *p = save;
            if (!ok) return 0;
        }
    }
    return 1;
}

int hop_xfer_rx_fs_remove_file(const wchar_t *path, unsigned long *error_code) {
    if (!path) return 0;
    wchar_t *extended = alloc_extended_path(path);
    const wchar_t *target = extended ? extended : path;
    BOOL ok = DeleteFileW(target);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    free(extended);
    if (error_code) *error_code = err;
    return ok ? 1 : 0;
}

static int remove_tree_recursive(const wchar_t *path) {
    size_t len = wcslen(path);
    int need_sep = len && path[len - 1] != L'\\' && path[len - 1] != L'/';
    wchar_t *pattern = (wchar_t *)malloc((len + (size_t)need_sep + 2u) * sizeof(wchar_t));
    if (!pattern) return -1;
    memcpy(pattern, path, len * sizeof(wchar_t));
    size_t pos = len;
    if (need_sep) pattern[pos++] = L'\\';
    pattern[pos++] = L'*';
    pattern[pos] = L'\0';

    wchar_t *ext_pattern = alloc_extended_path(pattern);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(ext_pattern ? ext_pattern : pattern, &data);
    DWORD find_err = find == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(ext_pattern);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        if (find_err != ERROR_FILE_NOT_FOUND && find_err != ERROR_PATH_NOT_FOUND)
            return -1;
    } else {
        int rc = 0;
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
                continue;
            size_t name_len = wcslen(data.cFileName);
            wchar_t *child = (wchar_t *)malloc((len + (size_t)need_sep + name_len + 1u) * sizeof(wchar_t));
            if (!child) { rc = -1; break; }
            memcpy(child, path, len * sizeof(wchar_t));
            size_t cpos = len;
            if (need_sep) child[cpos++] = L'\\';
            memcpy(child + cpos, data.cFileName, (name_len + 1u) * sizeof(wchar_t));

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    wchar_t *ext = alloc_extended_path(child);
                    if (!RemoveDirectoryW(ext ? ext : child)) rc = -1;
                    free(ext);
                } else if (remove_tree_recursive(child) != 0) {
                    rc = -1;
                }
            } else {
                wchar_t *ext = alloc_extended_path(child);
                const wchar_t *target = ext ? ext : child;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(target, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(target)) rc = -1;
                free(ext);
            }
            free(child);
            if (rc != 0) break;
        } while (FindNextFileW(find, &data));
        FindClose(find);
        if (rc != 0) return -1;
    }

    wchar_t *ext_dir = alloc_extended_path(path);
    const wchar_t *target_dir = ext_dir ? ext_dir : path;
    BOOL removed = RemoveDirectoryW(target_dir);
    DWORD err = removed ? ERROR_SUCCESS : GetLastError();
    if (!removed && err == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(target_dir, FILE_ATTRIBUTE_NORMAL);
        removed = RemoveDirectoryW(target_dir);
        err = removed ? ERROR_SUCCESS : GetLastError();
    }
    free(ext_dir);
    if (removed || err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return 0;
    return -1;
}

int hop_xfer_rx_fs_remove_tree(const wchar_t *path, const wchar_t *required_path_fragment) {
    if (!path || !*path || !required_path_fragment ||
        !wcsstr(path, required_path_fragment)) return -2;

    wchar_t *ext = alloc_extended_path(path);
    const wchar_t *target = ext ? ext : path;
    /* An already absent staging directory is a successful cleanup. */
    DWORD attributes = GetFileAttributesW(target);
    DWORD attr_err = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        free(ext);
        return (attr_err == ERROR_FILE_NOT_FOUND || attr_err == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(ext);
        return -1;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        BOOL removed = RemoveDirectoryW(target);
        free(ext);
        return removed ? 0 : -1;
    }
    free(ext);
    return remove_tree_recursive(path);
}
