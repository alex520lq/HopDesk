#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "xfer_writer_worker.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread/qos.h>
#include <errno.h>
#include <time.h>

static void *hop_writer_worker_main(void *opaque) {
    hop_writer_worker *worker = (hop_writer_worker *)opaque;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    while (worker->callbacks.take(worker->context, worker->task_storage)) {
        worker->callbacks.run(worker->context, worker->task_storage);
        worker->callbacks.complete(worker->context, worker->task_storage);
    }
    if (worker->callbacks.on_exit) worker->callbacks.on_exit(worker->context);
    pthread_mutex_lock(&worker->exit_lock);
    worker->exited = 1;
    pthread_cond_broadcast(&worker->exit_cv);
    pthread_mutex_unlock(&worker->exit_lock);
    return NULL;
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
    if (pthread_mutex_init(&worker->exit_lock, NULL) != 0) {
        free(worker->task_storage);
        memset(worker, 0, sizeof(*worker));
        return 0;
    }
    if (pthread_cond_init(&worker->exit_cv, NULL) != 0) {
        pthread_mutex_destroy(&worker->exit_lock);
        free(worker->task_storage);
        memset(worker, 0, sizeof(*worker));
        return 0;
    }
    worker->exit_sync_initialized = 1;
    if (pthread_create(&worker->thread, NULL, hop_writer_worker_main, worker) != 0) {
        pthread_cond_destroy(&worker->exit_cv);
        pthread_mutex_destroy(&worker->exit_lock);
        free(worker->task_storage);
        memset(worker, 0, sizeof(*worker));
        return 0;
    }
    worker->state = 1;
    return 1;
}

int hop_writer_worker_join(hop_writer_worker *worker) {
    if (!worker || worker->state != 1) return worker && worker->state == 2;
#if defined(__APPLE__)
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 0;
    uint64_t deadline_ns = (uint64_t)start.tv_sec * 1000000000ull +
                           (uint64_t)start.tv_nsec + 3000000000ull;
    pthread_mutex_lock(&worker->exit_lock);
    while (!worker->exited) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            pthread_mutex_unlock(&worker->exit_lock);
            return 0;
        }
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        if (now_ns >= deadline_ns) {
            pthread_mutex_unlock(&worker->exit_lock);
            return 0;
        }
        uint64_t rem_ns = deadline_ns - now_ns;
        struct timespec rel = {
            (time_t)(rem_ns / 1000000000ull),
            (long)(rem_ns % 1000000000ull)
        };
        int wait_result = pthread_cond_timedwait_relative_np(
            &worker->exit_cv, &worker->exit_lock, &rel);
        if (wait_result == ETIMEDOUT) {
            if (!worker->exited) {
                pthread_mutex_unlock(&worker->exit_lock);
                return 0;
            }
            break;
        }
        if (wait_result != 0) {
            pthread_mutex_unlock(&worker->exit_lock);
            return 0;
        }
    }
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return 0;
    deadline.tv_sec += 3;
    pthread_mutex_lock(&worker->exit_lock);
    while (!worker->exited) {
        int wait_result = pthread_cond_timedwait(&worker->exit_cv, &worker->exit_lock, &deadline);
        if (wait_result == ETIMEDOUT) {
            if (!worker->exited) {
                pthread_mutex_unlock(&worker->exit_lock);
                return 0;
            }
            break;
        }
        if (wait_result != 0) {
            pthread_mutex_unlock(&worker->exit_lock);
            return 0;
        }
    }
#endif
    pthread_mutex_unlock(&worker->exit_lock);
    if (pthread_join(worker->thread, NULL) != 0) return 0;
    pthread_cond_destroy(&worker->exit_cv);
    pthread_mutex_destroy(&worker->exit_lock);
    worker->exit_sync_initialized = 0;
    free(worker->task_storage);
    worker->task_storage = NULL;
    worker->state = 2;
    return 1;
}
