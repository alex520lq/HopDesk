#include "xfer_tracking.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t hop_xfer_job_total(int image, int entries) {
    return image ? 1u : (uint32_t)entries;
}

static void bump_revision(hop_xfer_tracking *t) {
    if (t && ++t->revision == 0) ++t->revision;
}

int hop_xfer_tracking_matches(const hop_xfer_tracking *t, uint32_t batch) {
    return t->status.active && t->status.batch == batch;
}

void hop_xfer_tracking_reset(hop_xfer_tracking *t) {
    if (!t) return;
    hop_xfer_file_record *files = t->files;
    int capacity = t->capacity;
    uint64_t revision = t->revision;
    free(t->path_storage);
    memset(t, 0, sizeof(*t));
    t->files = files;
    t->capacity = capacity;
    t->revision = revision;
}

int hop_xfer_tracking_reserve(hop_xfer_tracking *t, uint32_t capacity) {
    if (!t || capacity > HOP_XFER_FILE_LIMIT) return 0;
    if (capacity < (uint32_t)t->count) capacity = (uint32_t)t->count;
    if (capacity == (uint32_t)t->capacity) return 1;
    if (capacity == 0) {
        free(t->files);
        t->files = NULL;
        t->capacity = 0;
        return 1;
    }
    hop_xfer_file_record *files = (hop_xfer_file_record *)realloc(
        t->files, (size_t)capacity * sizeof(*files));
    if (!files) return 0;
    if (capacity > (uint32_t)t->capacity)
        memset(files + t->capacity, 0,
               (size_t)(capacity - (uint32_t)t->capacity) * sizeof(*files));
    t->files = files;
    t->capacity = (int)capacity;
    return 1;
}

void hop_xfer_tracking_dispose(hop_xfer_tracking *t) {
    if (!t) return;
    free(t->files);
    t->files = NULL;
    t->capacity = 0;
    hop_xfer_tracking_reset(t);
}

void hop_xfer_tracking_add_time(hop_xfer_tracking *t, uint32_t batch,
        int stage, uint64_t elapsed_ns) {
    if (!hop_xfer_tracking_matches(t,batch) || stage < 0 ||
        stage >= HOP_XFER_TIME_COUNT || !elapsed_ns) return;
    uint64_t *value = &t->stage_ns[stage];
    *value = elapsed_ns > UINT64_MAX - *value ? UINT64_MAX : *value + elapsed_ns;
}

void hop_xfer_tracking_begin(hop_xfer_tracking *t, uint32_t batch,
        int direction, uint32_t files, uint64_t bytes, const char *name, int phase) {
    hop_xfer_tracking_reset(t);
    uint32_t required = files > HOP_XFER_FILE_LIMIT ? HOP_XFER_FILE_LIMIT : files;
    (void)hop_xfer_tracking_reserve(t, required);
    t->status.batch = batch; t->status.direction = direction;
    t->status.active = 1; t->status.phase = phase;
    t->status.files_total = files; t->status.bytes_total = bytes;
    if (name) snprintf(t->status.name, sizeof(t->status.name), "%s", name);
    bump_revision(t);
}

static size_t tracking_path_length(const char *path) {
    size_t length = 0;
    const size_t limit = sizeof(((hop_xfer_file_status *)0)->relative_path) - 1;
    if (!path) return 0;
    while (length < limit && path[length]) ++length;
    return length;
}

static int reserve_path_storage(hop_xfer_tracking *t, size_t required) {
    const size_t max_capacity = (size_t)HOP_XFER_FILE_LIMIT *
        sizeof(((hop_xfer_file_status *)0)->relative_path);
    if (required > max_capacity) return 0;
    if (required <= t->path_capacity) return 1;
    size_t capacity = t->path_capacity ? t->path_capacity : 4096;
    while (capacity < required) {
        if (capacity > max_capacity / 2) {
            capacity = max_capacity;
            break;
        }
        capacity *= 2;
    }
    char *storage = (char *)realloc(t->path_storage, capacity);
    if (!storage) return 0;
    t->path_storage = storage;
    t->path_capacity = capacity;
    return 1;
}

static int record_path_is(const hop_xfer_tracking *t,
        const hop_xfer_file_record *record, const char *path, size_t length) {
    if (!path || !record->path_length || record->path_length != length ||
        !t->path_storage || record->path_offset >= t->path_used)
        return 0;
    size_t offset = record->path_offset;
    if (length >= t->path_used - offset) return 0;
    return t->path_storage[offset + length] == '\0' &&
           memcmp(t->path_storage + offset, path, length) == 0;
}

static void record_path_set(hop_xfer_tracking *t,
        hop_xfer_file_record *record, const char *path, size_t length) {
    record->path_offset = 0;
    record->path_length = 0;
    if (!path || !length) return;
    size_t bytes = length + 1;
    if (t->path_used > UINT32_MAX - bytes ||
        !reserve_path_storage(t, t->path_used + bytes))
        return;
    record->path_offset = (uint32_t)t->path_used;
    record->path_length = (uint16_t)length;
    memcpy(t->path_storage + t->path_used, path, length);
    t->path_storage[t->path_used + length] = '\0';
    t->path_used += bytes;
}

void hop_xfer_tracking_file(hop_xfer_tracking *t, uint32_t batch,
        uint32_t id, const char *path, uint64_t size, int directory, int phase) {
    if (!hop_xfer_tracking_matches(t, batch) || id == 0 || id > HOP_XFER_FILE_LIMIT) return;
    int i = (int)id - 1;
    if (i >= t->capacity && !hop_xfer_tracking_reserve(t, id)) return;
    int first = i;
    hop_xfer_file_record *f = &t->files[i];
    uint32_t old_path_offset = 0;
    uint16_t old_path_length = 0;
    int preserve_path = 0;
    size_t path_length = tracking_path_length(path);
    if (i < t->count) {
        /* Re-registering an existing id is a retry: roll back only that
         * file's progress and restart the throughput sample window. New files
         * must not reset the batch-wide speed estimate. */
        old_path_offset = f->path_offset;
        old_path_length = f->path_length;
        preserve_path = record_path_is(t, f, path, path_length);
        t->status.bytes_total -= f->bytes_total;
        t->status.bytes_done -= f->bytes_done;
        if (f->phase == HOP_XFER_FILE_COMPLETED && t->status.files_done) t->status.files_done--;
        t->sample_ms = 0;
        t->sample_bytes = t->status.bytes_done;
        t->status.speed_bytes_per_sec = 0;
        t->status.last_progress_ms = 0;
        t->status.eta_seconds = 0;
    } else {
        /* Clear any gap before exposing it through the file snapshot. */
        first = t->count;
        t->count = i + 1;
    }
    memset(&t->files[first], 0, (size_t)(i - first + 1) * sizeof(*f));
    f->index = (uint32_t)i; f->phase = phase;
    f->is_directory = directory; f->bytes_total = size;
    if (preserve_path) {
        f->path_offset = old_path_offset;
        f->path_length = old_path_length;
    } else {
        record_path_set(t, f, path, path_length);
    }
    t->status.bytes_total += size;
    if (t->status.files_total < id) t->status.files_total = id;
    bump_revision(t);
}

void hop_xfer_tracking_chunk(hop_xfer_tracking *t, uint32_t batch,
        uint32_t id, uint64_t off, uint32_t size, uint64_t now_ms) {
    if (!hop_xfer_tracking_matches(t, batch) || id == 0 || id > (uint32_t)t->count) return;
    hop_xfer_file_record *f = &t->files[id - 1];
    if (off > f->bytes_total || size > f->bytes_total - off) return;
    uint64_t end = off + size;
    if (end > f->bytes_done) {
        t->status.bytes_done += end - f->bytes_done;
        f->bytes_done = end;
        bump_revision(t);
        t->status.last_progress_ms = now_ms;
        if (!t->sample_ms || now_ms <= t->sample_ms ||
            now_ms - t->sample_ms > 2000u) {
            /* Do not average an idle pause into the next active throughput sample. */
            t->sample_ms = now_ms;
            t->sample_bytes = t->status.bytes_done;
            t->status.speed_bytes_per_sec = 0;
            t->status.eta_seconds = 0;
        } else if (t->status.bytes_done > t->sample_bytes) {
            uint64_t elapsed_ms = now_ms - t->sample_ms;
            uint64_t delta_bytes = t->status.bytes_done - t->sample_bytes;
            if (elapsed_ms) {
                uint64_t instant = delta_bytes > UINT64_MAX / 1000u ?
                    UINT64_MAX : (delta_bytes * 1000u) / elapsed_ms;
                if (!t->status.speed_bytes_per_sec)
                    t->status.speed_bytes_per_sec = instant;
                else
                    t->status.speed_bytes_per_sec =
                        t->status.speed_bytes_per_sec -
                        t->status.speed_bytes_per_sec / 4u + instant / 4u;
                t->sample_ms = now_ms;
                t->sample_bytes = t->status.bytes_done;
                uint64_t remaining = t->status.bytes_total > t->status.bytes_done ?
                    t->status.bytes_total - t->status.bytes_done : 0;
                t->status.eta_seconds = t->status.speed_bytes_per_sec ?
                    remaining / t->status.speed_bytes_per_sec +
                    (remaining % t->status.speed_bytes_per_sec != 0) : 0;
            }
        }
    }
}

void hop_xfer_tracking_phase(hop_xfer_tracking *t, uint32_t batch,
        int index, int phase, uint32_t attempts) {
    if (!hop_xfer_tracking_matches(t, batch) || index < 0 || index >= t->count) return;
    hop_xfer_file_record *f = &t->files[index];
    int changed = f->phase != phase || f->attempts != attempts;
    if (f->phase == HOP_XFER_FILE_COMPLETED && phase != HOP_XFER_FILE_COMPLETED && t->status.files_done)
        t->status.files_done--;
    if (phase == HOP_XFER_FILE_COMPLETED && f->phase != HOP_XFER_FILE_COMPLETED)
        t->status.files_done++;
    f->phase = phase; f->attempts = attempts;
    if (changed) bump_revision(t);
}

int hop_xfer_tracking_files_page(const hop_xfer_tracking *t, uint32_t batch,
        uint32_t offset, hop_xfer_file_status *out, int capacity,
        int *total, uint64_t *revision) {
    if (capacity < 0 || (capacity && !out) || !t || !batch) return -1;
    if (total) *total = 0;
    if (revision) *revision = 0;
    if (t->status.batch != batch) return 0;
    if (total) *total = t->count;
    if (revision) *revision = t->revision;
    if (capacity == 0 || offset >= (uint32_t)t->count) return 0;
    if (!t->files || t->count > t->capacity) return -1;
    uint32_t remaining = (uint32_t)t->count - offset;
    int count = remaining < (uint32_t)capacity ? (int)remaining : capacity;
    for (int i = 0; i < count; ++i) {
        const hop_xfer_file_record *source = &t->files[offset + (uint32_t)i];
        hop_xfer_file_status *destination = &out[i];
        memset(destination, 0, sizeof(*destination));
        destination->index = source->index;
        destination->phase = source->phase;
        destination->is_directory = source->is_directory;
        destination->attempts = source->attempts;
        destination->bytes_done = source->bytes_done;
        destination->bytes_total = source->bytes_total;
        size_t path_offset = source->path_offset;
        size_t path_length = source->path_length;
        if (t->path_storage && path_length < sizeof(destination->relative_path) &&
            path_offset < t->path_used && path_length < t->path_used - path_offset &&
            t->path_storage[path_offset + path_length] == '\0') {
            memcpy(destination->relative_path,
                   t->path_storage + path_offset, path_length);
            destination->relative_path[path_length] = '\0';
        }
    }
    return count;
}

int hop_xfer_tracking_end(hop_xfer_tracking *t, uint32_t batch,
        int phase, const char *detail) {
    if (!hop_xfer_tracking_matches(t, batch)) return 0;
    t->status.active = 0; t->status.phase = phase;
    t->status.speed_bytes_per_sec = 0;
    t->status.eta_seconds = 0;
    if (detail) snprintf(t->status.detail, sizeof(t->status.detail), "%s", detail);
    bump_revision(t);
    return 1;
}

hop_xfer_tracking *hop_xfer_tracking_visible(hop_xfer_tracking *send,
        hop_xfer_tracking *receive) {
    if (send->status.active != receive->status.active)
        return send->status.active ? send : receive;
    return send->revision >= receive->revision ? send : receive;
}

void hop_xfer_tracking_snapshot_pair(const hop_xfer_tracking *send,
        const hop_xfer_tracking *receive, uint32_t queued, hop_xfer_status_pair *out) {
    if (!out || !send || !receive) return;
    out->send = send->status;
    out->receive = receive->status;
    out->send_revision = send->revision;
    out->receive_revision = receive->revision;
    out->send.queued_count = queued;
    out->receive.queued_count = queued;
    if (send->status.active != receive->status.active)
        out->primary_direction = send->status.active ? HOP_XFER_DIR_SEND : HOP_XFER_DIR_RECEIVE;
    else
        out->primary_direction = send->revision >= receive->revision ? HOP_XFER_DIR_SEND : HOP_XFER_DIR_RECEIVE;
}
