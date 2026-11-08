#ifndef HOP_MAC_XFER_WRITER_IO_H
#define HOP_MAC_XFER_WRITER_IO_H
#include <stdint.h>
typedef struct { int file; } hop_writer_io;
void hop_writer_io_init(hop_writer_io *io);
int hop_writer_io_open(hop_writer_io *io, const char *path, int *error);
int hop_writer_io_write_at(hop_writer_io *io, uint64_t offset,
                            const uint8_t *data, uint32_t len, int *error);
void hop_writer_io_close(hop_writer_io *io);
#endif
