#include "xfer_writer_worker.h"

#include <stdlib.h>
#include <string.h>

static DWORD WINAPI hop_writer_worker_main(LPVOID opaque) {
    hop_writer_worker *worker = (hop_writer_worker *)opaque;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (worker->callbacks.take(worker->context, worker->task_storage)) {
        worker->callbacks.run(worker->context, worker->task_storage);
        worker->callbacks.complete(worker->context, worker->task_storage);
    }
    if (worker->callbacks.on_exit) worker->callbacks.on_exit(worker->context);
    return 0;
}

int hop_writer_worker_start(hop_writer_worker *worker, void *context,
                            size_t task_size,
                            const hop_writer_worker_callbacks *callbacks) {
    if (!worker || !context || !task_size || !callbacks ||
        !callbacks->take || !callbacks->run || !callbacks->complete ||
        worker->state == 1) return 0;
    memset(worker, 0, sizeof(*worker));
    worker->context = context;
    worker->callbacks = *callbacks;
    worker->task_size = task_size;
    worker->task_storage = malloc(task_size);
    if (!worker->task_storage) { memset(worker, 0, sizeof(*worker)); return 0; }
    worker->thread = CreateThread(NULL, 0, hop_writer_worker_main, worker, 0, NULL);
    if (!worker->thread) {
        free(worker->task_storage);
        memset(worker, 0, sizeof(*worker));
        return 0;
    }
    worker->state = 1;
    return 1;
}

int hop_writer_worker_join(hop_writer_worker *worker) {
    if (!worker || worker->state != 1) return worker && worker->state == 2;
    if (WaitForSingleObject(worker->thread, 3000) != WAIT_OBJECT_0) return 0;
    CloseHandle(worker->thread);
    worker->thread = NULL;
    free(worker->task_storage);
    worker->task_storage = NULL;
    worker->state = 2;
    return 1;
}
