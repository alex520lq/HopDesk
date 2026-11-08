#include "xfer_writer_worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <pthread/qos.h>

typedef struct {
    pthread_t thread;
} mac_thread_storage;

_Static_assert(sizeof(mac_thread_storage) <= sizeof(((hop_writer_worker *)0)->thread_storage),
               "worker thread storage is too small");

static void *hop_writer_worker_main(void *opaque) {
    hop_writer_worker *worker = (hop_writer_worker *)opaque;
    void *task = malloc(worker->task_size);
    if (task) {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        while (worker->callbacks.take(worker->context, task)) {
            worker->callbacks.run(worker->context, task);
            worker->callbacks.complete(worker->context, task);
        }
        free(task);
    }
    if (worker->callbacks.on_exit) worker->callbacks.on_exit(worker->context);
    return NULL;
}

int hop_writer_worker_start(hop_writer_worker *worker, void *context,
                            size_t task_size,
                            const hop_writer_worker_callbacks *callbacks) {
    if (!worker || !context || !task_size || !callbacks ||
        !callbacks->take || !callbacks->run || !callbacks->complete ||
        worker->state != 0) return 0;
    memset(worker, 0, sizeof(*worker));
    worker->context = context;
    worker->callbacks = *callbacks;
    worker->task_size = task_size;
    mac_thread_storage *storage = (mac_thread_storage *)worker->thread_storage;
    if (pthread_create(&storage->thread, NULL, hop_writer_worker_main, worker) != 0) {
        memset(worker, 0, sizeof(*worker));
        return 0;
    }
    worker->state = 1;
    return 1;
}

int hop_writer_worker_join(hop_writer_worker *worker) {
    if (!worker || worker->state != 1) return worker && worker->state == 2;
    mac_thread_storage *storage = (mac_thread_storage *)worker->thread_storage;
    if (pthread_join(storage->thread, NULL) != 0) return 0;
    worker->state = 2;
    return 1;
}
