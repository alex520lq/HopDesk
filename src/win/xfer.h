/* =====================================================================
 *  HopDesk - Windows 文件/图片数据通道（port+1）
 *
 *  与控制通道分开的理由：单条 TCP 流上的 1 MB 分块会造成队头阻塞，
 *  鼠标事件要等整块发完才轮到 —— 分通道后两者互不干扰。
 *
 *  线程模型：
 *    listen 线程  accept + 接收对端帧（Mac -> Windows 方向）
 *    send 线程    从作业队列取任务，流式读盘发送（Windows -> Mac 方向）
 *  两个线程共用一个 socket，但只有 send 线程写，全双工不冲突。
 *
 *  提交侧（剪贴板回调）只入队，立即返回 —— 绝不在主线程读盘或发网络。
 * ===================================================================== */
#ifndef WIN_XFER_H
#define WIN_XFER_H

#include <windows.h>
#include "hop.h"
#include "../core/xfer_status.h"

/* 起监听 + 发送线程。失败返回 0。 */
int  xfer_init(HWND hwnd, const hop_config *cfg);
void xfer_shutdown(void);

/* 数据通道是否已连接 */
int  xfer_connected(void);

/* 有没有正在传或排队的批次（菜单项据此决定是否可点） */
int  xfer_busy(void);
/* Thread-safe snapshot for native transfer manager UI; caller owns the copy. */
void xfer_get_status(hop_xfer_status *out);
/* Most recent completed batches first; returns number of records copied. */
int xfer_get_history(hop_xfer_history_entry *out, int capacity);
/* Snapshot an explicitly identified task; stale snapshots return no files. */
int xfer_get_files(int direction, uint32_t batch, hop_xfer_file_status *out, int capacity);
/* Re-enqueue one failed source file from the latest visible batch. */
int xfer_retry_file(uint32_t batch, int index);

/* 取消当前批次：停发、通知对端、删掉接收侧半成品。
 * 发送端每块之后检查一次，所以响应延迟上限是一个分块（默认 256 KB）。 */
void xfer_cancel(void);

/* 提交一批本机路径（文件或目录，目录递归）。内部深拷贝路径，
 * 调用方返回后可以自由释放自己的数组。返回 0 = 未连接，已丢弃。 */
int  xfer_submit_paths(const wchar_t *const *paths, int n);

/* 提交一张图片：裸 BGRA、自上而下、行距 stride 字节。内部拷贝像素。 */
int  xfer_submit_image(const uint8_t *bgra, int w, int h, int stride);

#endif
