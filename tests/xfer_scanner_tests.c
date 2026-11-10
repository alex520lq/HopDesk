#define _POSIX_C_SOURCE 200809L
#include "xfer_scanner.h"
#include "../src/core/hop.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int reserve_calls;
static int reject_after = -1;
static int cancel_requested;
static int allocation_fail;

static int scan_reserve(void *context, uint64_t bytes) {
    (void)context; (void)bytes;
    if (reject_after >= 0 && reserve_calls >= reject_after) return 0;
    ++reserve_calls;
    return 1;
}
static int scan_cancel(void *context) {
    (void)context;
    return cancel_requested;
}
static void *scan_realloc(void *context, void *memory, uint64_t bytes) {
    (void)context;
    if (allocation_fail) return NULL;
    return realloc(memory, (size_t)bytes);
}
static int scan_watchdog_begin(void *context, const char *operation, unsigned timeout_ms) {
    (void)context; (void)operation; (void)timeout_ms;
    return 1;
}
static void scan_watchdog_end(void *context, int ticket) {
    (void)context; (void)ticket;
}
static const hop_xfer_scan_services scan_services = {
    NULL, scan_reserve, scan_cancel, scan_realloc, scan_watchdog_begin, scan_watchdog_end
};

static void reset_scan(elist *list) {
    memset(list, 0, sizeof(*list));
    reserve_calls = 0;
    reject_after = -1;
    cancel_requested = 0;
    allocation_fail = 0;
}

#ifdef _WIN32
#include <windows.h>

static void write_file(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    const char data[] = "x";
    DWORD written = 0;
    assert(WriteFile(h, data, 1, &written, NULL) && written == 1);
    CloseHandle(h);
}

int main(void) {
    wchar_t temp[MAX_PATH], root[MAX_PATH], sub[MAX_PATH], a[MAX_PATH], b[MAX_PATH], missing[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    assert(n > 0 && n < MAX_PATH);
    assert(swprintf(root, MAX_PATH, L"%lsHopDeskScanner-%lu", temp,
                    (unsigned long)GetCurrentProcessId()) > 0);
    assert(CreateDirectoryW(root, NULL));
    assert(swprintf(sub, MAX_PATH, L"%ls\\nested", root) > 0);
    assert(CreateDirectoryW(sub, NULL));
    assert(swprintf(a, MAX_PATH, L"%ls\\a.txt", root) > 0);
    assert(swprintf(b, MAX_PATH, L"%ls\\b.txt", sub) > 0);
    assert(swprintf(missing, MAX_PATH, L"%ls\\missing", root) > 0);
    write_file(a); write_file(b);

    elist list;
    reset_scan(&list);
    const wchar_t *paths[] = {root};
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(!list.failed && list.n == 4 && list.error == HOP_XFER_SCAN_OK);
    int dirs = 0, files = 0;
    for (int i = 0; i < list.n; ++i) {
        if (list.v[i].flags & HOP_FILE_FLAG_DIR) ++dirs;
        else ++files;
        assert(list.v[i].name && list.v[i].rel);
    }
    assert(dirs == 2 && files == 2);
    elist_free(&list);

    reset_scan(&list); reject_after = 2;
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.budget_rejected && list.error == HOP_XFER_SCAN_BUDGET_REJECTED);
    elist_free(&list);

    reset_scan(&list); allocation_fail = 1;
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_OUT_OF_MEMORY);
    elist_free(&list);

    reset_scan(&list); cancel_requested = 1;
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_CANCELLED);
    elist_free(&list);

    reset_scan(&list);
    const wchar_t *missing_paths[] = {missing};
    hop_xfer_scan_paths(&list, missing_paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_SOURCE_UNAVAILABLE);
    elist_free(&list);

    DeleteFileW(b); DeleteFileW(a);
    RemoveDirectoryW(sub); RemoveDirectoryW(root);
    puts("PASS: Windows scanner snapshots directories/files and rejects budget, cancel and missing paths");
    return 0;
}
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    close(fd);
}

int main(void) {
    char root_template[] = "/tmp/hopdesk-scanner-XXXXXX";
    char *root = mkdtemp(root_template);
    assert(root != NULL);
    char sub[1024], a[1024], b[1024], link_path[1024], missing[1024];
    snprintf(sub, sizeof(sub), "%s/nested", root);
    snprintf(a, sizeof(a), "%s/a.txt", root);
    snprintf(b, sizeof(b), "%s/b.txt", sub);
    snprintf(link_path, sizeof(link_path), "%s/link", root);
    snprintf(missing, sizeof(missing), "%s/missing", root);
    assert(mkdir(sub, 0700) == 0);
    write_file(a); write_file(b);

    elist list;
    reset_scan(&list);
    const char *paths[] = {root};
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(!list.failed && list.n == 4);
    int dirs = 0, files = 0;
    for (int i = 0; i < list.n; ++i) {
        if (list.v[i].flags & HOP_FILE_FLAG_DIR) ++dirs;
        else ++files;
        assert(list.v[i].name && list.v[i].rel);
    }
    assert(dirs == 2 && files == 2);
    elist_free(&list);

    reset_scan(&list); reject_after = 2;
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.budget_rejected && list.error == HOP_XFER_SCAN_BUDGET_REJECTED);
    elist_free(&list);

    reset_scan(&list); cancel_requested = 1;
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_CANCELLED);
    elist_free(&list);

    reset_scan(&list);
    const char *missing_paths[] = {missing};
    hop_xfer_scan_paths(&list, missing_paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_SOURCE_UNAVAILABLE);
    elist_free(&list);

    assert(symlink(a, link_path) == 0);
    reset_scan(&list);
    hop_xfer_scan_paths(&list, paths, 1, &scan_services);
    assert(list.failed && list.error == HOP_XFER_SCAN_LINK_REJECTED);
    elist_free(&list);

    unlink(link_path); unlink(b); unlink(a);
    rmdir(sub); rmdir(root);
    puts("PASS: macOS scanner snapshots directories/files and rejects symlink, budget, cancel and missing paths");
    return 0;
}
#endif
