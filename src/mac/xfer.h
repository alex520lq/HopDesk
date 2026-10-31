/* =====================================================================
 *  HopDesk - macOS 文件/图片数据通道（客户端侧）
 *
 *  连 Windows 的 data_port，一条连接全双工：
 *    Windows -> Mac  接收线程读，落盘后交给 clipboard.m 挂剪贴板
 *    Mac -> Windows  发送线程写，剪贴板轮询到文件/图片就入队
 *
 *  纯 C。挂剪贴板需要主队列，统一由 clipboard.m 里的 clipboard_apply_* 做。
 * ===================================================================== */
#ifndef MAC_XFER_H
#define MAC_XFER_H

#include "hop.h"
#include "../core/xfer_status.h"

void xfer_init(const hop_config *cfg);
void xfer_shutdown(void);

int  xfer_connected(void);

/* 有没有正在传或排队的批次（菜单项据此决定是否可点） */
int  xfer_busy(void);
/* Thread-safe snapshot for native transfer manager UI; caller owns the copy. */
void xfer_get_status(hop_xfer_status *out);
/* Most recent completed batches first; returns number of records copied. */
int xfer_get_history(hop_xfer_history_entry *out, int capacity);
/* Snapshot the current batch's per-file state; caller owns the output buffer. */
int xfer_get_files(hop_xfer_file_status *out, int capacity);

/* 取消当前批次：停发、通知对端、删掉接收侧半成品。
 * 发送端每块之后检查一次，所以响应延迟上限是一个分块（默认 256 KB）。 */
void xfer_cancel(void);

/* 提交本机路径（文件或目录，目录递归）。内部深拷贝，返回后调用方可释放。 */
int  xfer_submit_paths(const char *const *paths, int n);

/* 提交图片：裸 BGRA、自上而下、行距 stride 字节。内部拷贝像素。 */
int  xfer_submit_image(const uint8_t *bgra, int w, int h, int stride);

#endif
