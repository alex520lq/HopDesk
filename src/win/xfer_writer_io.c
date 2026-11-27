#include "xfer_writer_io.h"
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

static wchar_t *alloc_long_path(const wchar_t *path) {
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

void hop_writer_io_init(hop_writer_io *io) {
    if (io) io->file = INVALID_HANDLE_VALUE;
}

void hop_writer_io_close(hop_writer_io *io) {
    if (!io) return;
    if (io->file != INVALID_HANDLE_VALUE) CloseHandle(io->file);
    io->file = INVALID_HANDLE_VALUE;
}

int hop_writer_io_open(hop_writer_io *io, const wchar_t *path, unsigned long *error) {
    if (!io || !path) return 0;
    hop_writer_io_close(io);
    wchar_t *extended = alloc_long_path(path);
    io->file = CreateFileW(extended ? extended : path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = io->file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(extended);
    if (io->file != INVALID_HANDLE_VALUE) return 1;
    if (error) *error = err;
    return 0;
}

int hop_writer_io_write_at(hop_writer_io *io, uint64_t offset,
                           const uint8_t *data, uint32_t len,
                           unsigned long *error) {
    if (!io || io->file == INVALID_HANDLE_VALUE || (!data && len)) return 0;
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    DWORD wrote = 0;
    if (!SetFilePointerEx(io->file, position, NULL, FILE_BEGIN) ||
        !WriteFile(io->file, data, len, &wrote, NULL) || wrote != len) {
        if (error) *error = GetLastError();
        return 0;
    }
    return 1;
}
