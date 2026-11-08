#include "xfer_rx_fs.h"

#include <errno.h>
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *hop_xfer_rx_fs_path_for(const char *root, const char *relative_path) {
    if (!root || !relative_path) return NULL;
    size_t root_length = strlen(root), relative_length = strlen(relative_path);
    if (!relative_length || root_length + relative_length + 2u > 4096u) return NULL;
    size_t separator = root_length && root[root_length - 1] != '/' ? 1u : 0u;
    char *path = (char *)malloc(root_length + separator + relative_length + 1u);
    if (!path) return NULL;
    memcpy(path, root, root_length);
    size_t offset = root_length;
    if (separator) path[offset++] = '/';
    memcpy(path + offset, relative_path, relative_length);
    path[offset + relative_length] = '\\0';
    return path;
}

static int mkdir_component(const char *path) {
    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode) ? 0 : -1;
}

int hop_xfer_rx_fs_mkdir_p(const char *path) {
    if (!path) return -1;
    char temporary[4096];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(temporary)) return -1;
    memcpy(temporary, path, length + 1u);
    for (char *p = temporary + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\\0';
            int result = mkdir_component(temporary);
            *p = '/';
            if (result != 0) return -1;
        }
    }
    return mkdir_component(temporary);
}

static int remove_tree_entry(const char *path, const struct stat *status,
                             int flag, struct FTW *walk) {
    (void)status;
    (void)walk;
    if (flag == FTW_DP) return rmdir(path);
    return unlink(path);
}

int hop_xfer_rx_fs_remove_tree(const char *path, const char *required_path_fragment) {
    if (!path || !*path || !required_path_fragment ||
        !strstr(path, required_path_fragment)) return -2;
    return nftw(path, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS) == 0 ? 0 : -1;
}
