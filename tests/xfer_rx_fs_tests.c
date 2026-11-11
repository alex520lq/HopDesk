#ifndef _WIN32
#define _DARWIN_C_SOURCE 1
#endif
#include "xfer_rx_fs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
int main(void) {
    wchar_t temporary[MAX_PATH], root[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temporary) > 0);
    swprintf(root, MAX_PATH, L"%lsHopDesk-rx-fs-%lu", temporary, (unsigned long)GetCurrentProcessId());
    assert(CreateDirectoryW(root, NULL));
    wchar_t *path = hop_xfer_rx_fs_path_for(root, "sub/receiver-测试.txt");
    assert(path != NULL);
    assert(wcsstr(path, L"sub\\receiver-测试.txt") != NULL);
    assert(hop_xfer_rx_fs_ensure_dirs(path));
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    const char payload[] = "receiver-fs";
    DWORD written = 0;
    assert(WriteFile(file, payload, (DWORD)(sizeof(payload) - 1), &written, NULL));
    assert(written == sizeof(payload) - 1);
    CloseHandle(file);
    assert(hop_xfer_rx_fs_remove_file(path, NULL) == 1);
    assert(hop_xfer_rx_fs_remove_tree(root, L"HopDesk") == 0);
    assert(hop_xfer_rx_fs_remove_tree(temporary, L"__no_such_root_marker__") == -2);
    free(path);
    puts("PASS: Windows receiver filesystem adapter path, mkdir and guarded cleanup");
    return 0;
}
#else
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char temporary[] = "/tmp/hopdesk-rx-fs-XXXXXX";
    assert(mkdtemp(temporary) != NULL);
    char parent_root[4096], root[4096];
    snprintf(parent_root, sizeof(parent_root), "%s/HopDesk", temporary);
    assert(mkdir(parent_root, 0755) == 0);
    snprintf(root, sizeof(root), "%s/Batch", parent_root);
    assert(mkdir(root, 0755) == 0);
    char *path = hop_xfer_rx_fs_path_for(root, "sub/receiver-测试.txt");
    assert(path != NULL);
    assert(strstr(path, "/HopDesk/Batch/sub/receiver-测试.txt") != NULL);
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", path);
    char *separator = strrchr(parent, '/');
    assert(separator != NULL);
    *separator = '\0';
    assert(hop_xfer_rx_fs_mkdir_p(parent) == 0);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    const char payload[] = "receiver-fs";
    assert(fwrite(payload, 1, sizeof(payload) - 1, file) == sizeof(payload) - 1);
    assert(fclose(file) == 0);
    assert(hop_xfer_rx_fs_remove_file(path) == 0);
    assert(hop_xfer_rx_fs_remove_tree(root, "/HopDesk/") == 0);
    assert(hop_xfer_rx_fs_remove_tree(temporary, "/__no_such_root_marker__/") == -2);
    free(path);
    assert(rmdir(parent_root) == 0);
    assert(rmdir(temporary) == 0);
    puts("PASS: macOS receiver filesystem adapter path, mkdir and guarded cleanup");
    return 0;
}
#endif
