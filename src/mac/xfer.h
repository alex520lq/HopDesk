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

void xfer_init(const hop_config *cfg);
void xfer_shutdown(void);

int  xfer_connected(void);

/* 提交本机路径（文件或目录，目录递归）。内部深拷贝，返回后调用方可释放。 */
int  xfer_submit_paths(const char *const *paths, int n);

/* 提交图片：裸 BGRA、自上而下、行距 stride 字节。内部拷贝像素。 */
int  xfer_submit_image(const uint8_t *bgra, int w, int h, int stride);

#endif
