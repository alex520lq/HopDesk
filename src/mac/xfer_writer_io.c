#include "xfer_writer_io.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

void hop_writer_io_init(hop_writer_io *io) {
    if (io) io->file = -1;
}

void hop_writer_io_close(hop_writer_io *io) {
    if (!io) return;
    if (io->file >= 0) close(io->file);
    io->file = -1;
}

int hop_writer_io_open(hop_writer_io *io, const char *path, int *error) {
    if (!io || !path) return 0;
    hop_writer_io_close(io);
    io->file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (io->file >= 0) return 1;
    if (error) *error = errno;
    return 0;
}

int hop_writer_io_write_at(hop_writer_io *io, uint64_t offset,
                           const uint8_t *data, uint32_t len, int *error) {
    if (!io || io->file < 0 || (!data && len)) return 0;
    if (offset > (uint64_t)INT64_MAX || lseek(io->file, (off_t)offset, SEEK_SET) < 0) {
        if (error) *error = errno ? errno : EOVERFLOW;
        return 0;
    }
    size_t done = 0;
    while (done < len) {
        ssize_t written = write(io->file, data + done, len - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (error) *error = errno ? errno : EIO;
            return 0;
        }
        done += (size_t)written;
    }
    return 1;
}
