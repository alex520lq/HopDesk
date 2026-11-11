#ifndef HOP_XFER_RX_FS_H
#define HOP_XFER_RX_FS_H

#ifdef _WIN32
#include <wchar.h>
wchar_t *hop_xfer_rx_fs_path_for(const wchar_t *root, const char *relative_utf8);
int hop_xfer_rx_fs_create_directory_if_missing(const wchar_t *path);
int hop_xfer_rx_fs_ensure_dirs(wchar_t *path);
int hop_xfer_rx_fs_remove_tree(const wchar_t *path, const wchar_t *required_path_fragment);
#else
char *hop_xfer_rx_fs_path_for(const char *root, const char *relative_path);
int hop_xfer_rx_fs_mkdir_p(const char *path);
int hop_xfer_rx_fs_remove_file(const char *path);
int hop_xfer_rx_fs_remove_tree(const char *path, const char *required_path_fragment);
#endif

#endif
