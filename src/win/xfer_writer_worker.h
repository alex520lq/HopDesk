#ifndef HOP_WIN_XFER_WRITER_WORKER_H
#define HOP_WIN_XFER_WRITER_WORKER_H

#include <stddef.h>
#include <windows.h>

typedef struct {
    int (*take)(void *context, void *task_out);
    void (*run)(void *context, const void *task);
    void (*complete)(void *context, const void *task);
    void (*on_exit)(void *context);
} hop_writer_worker_callbacks;

typedef struct {
    void *context;
    hop_writer_worker_callbacks callbacks;
    size_t task_size;
    int state;
    HANDLE thread;
} hop_writer_worker;

/* Callback context and its referenced state must outlive a successful join. */
int hop_writer_worker_start(hop_writer_worker *worker, void *context,
                            size_t task_size,
                            const hop_writer_worker_callbacks *callbacks);
/* A timeout/failure preserves the thread handle and worker state for safe retry. */
int hop_writer_worker_join(hop_writer_worker *worker);

#endif
