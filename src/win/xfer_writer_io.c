#include "xfer_writer_io.h"
#include <stdint.h>

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
    io->file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (io->file != INVALID_HANDLE_VALUE) return 1;
    if (error) *error = GetLastError();
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
