#include "xfer_rx_fs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

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
    wcscpy(path, root);
    if (root_length && path[root_length - 1] != L'\\') wcscat(path, L"\\");
    wcscat(path, relative);
    free(relative);
    return path;
}

int hop_xfer_rx_fs_create_directory_if_missing(const wchar_t *path) {
    if (!path) return 0;
    if (CreateDirectoryW(path, NULL)) return 1;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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

int hop_xfer_rx_fs_remove_tree(const wchar_t *path, const wchar_t *required_path_fragment) {
    if (!path || !*path || !required_path_fragment ||
        !wcsstr(path, required_path_fragment)) return -2;
    wchar_t buffer[MAX_PATH + 2];
    size_t length = wcslen(path);
    if (length + 2 > MAX_PATH + 2) return -1;
    wcscpy(buffer, path);
    buffer[length + 1] = L'\0';
    SHFILEOPSTRUCTW operation;
    memset(&operation, 0, sizeof(operation));
    operation.wFunc = FO_DELETE;
    operation.pFrom = buffer;
    operation.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    return SHFileOperationW(&operation) == 0 ? 0 : -1;
}
