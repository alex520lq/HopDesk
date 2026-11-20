# 文件剪贴板与按需传输

> 当前基线：`main` @ `6e94d8699be6f11730fdf9b14a6df99ad90090d2`（2026-11-20）。Clipboard transaction Core 已通过 PR #15/#16 合并。
> 本文是当前产品行为与实现边界说明；未执行的实机/性能验收不得标记完成。

## 1. 产品行为不变量

1. 本机复制文件/文件夹时保留操作系统原生剪贴板和本机路径；普通复制不创建 HopDesk 传输任务。
2. 本机 Ctrl+V / Cmd+V 始终走系统原生行为，不消耗跨端 Offer。
3. 一次复制允许先本机粘贴，再跨设备粘贴；本机粘贴不使跨端 Offer 失效。
4. 复制阶段只发布轻量 File Offer；不递归扫描目录、不读取文件内容、不创建发送任务、不写“发送/完成”历史。
5. 只有目标设备真正发起 Paste Request 后，发送端才扫描/读取文件并创建传输任务。
6. Offer、Request、Paste 必须受 generation、session、request ID 和剪贴板版本约束；重复、迟到或过期消息不得启动或覆盖新任务。
7. 历史记录只记录真实 Paste Transaction；普通复制、Offer 广播和未粘贴不算传输。

## 2. 当前共享事务模块

`src/core/xfer_clipboard_txn.c/.h` 是唯一共享 Clipboard transaction production module，负责：

- 本地 Offer generation 与身份发布/替换/退役；
- remote Offer 身份接收、清除与重连时重播决策；
- Paste Request ID 创建、重复请求识别、busy/stale admission；
- Paste 完成时的 generation/clipboard version 校验；
- Paste Worker admission 与完成状态；
- 外部剪贴板变化、应用自身写剪贴板和链路变化时的 Offer 退役/通知意图。

Windows 与 macOS 生产路径都调用相同 Core 状态转换。Core 不包含 OS clipboard API、路径类型、线程或锁。

## 3. 平台 Adapter 的职责

| Windows | macOS |
|---|---|
| `clipboard.c` / offer/paste includes | `clipboard.m` / offer/paste includes |
| `AddClipboardFormatListener`、clipboard sequence、HGLOBAL | `NSPasteboard`、changeCount、主队列 |
| Win32 路径/文件快照与 Worker/Event 生命周期 | POSIX 路径/文件快照、dispatch/pthread 生命周期 |
| DIB 图片采集/转换与原生剪贴板写入 | TIFF/PNG/CGImage 采集/转换与 NSPasteboard 写入 |

平台锁负责串行调用 Core transaction API；Core 只返回平台无关决策。当前 `clipboard_offer.inc` 与 `clipboard_paste.inc` 仍是平台 include 片段，不应误记为共享 Core，也不能仅改扩展名就称为完成模块化。

## 4. 文件/目录粘贴路径

- Offer 只携带轻量文件/目录描述和相对路径，不传本机绝对路径。
- 目标端发起 Paste Request 后才开始实际扫描、文件枚举和传输；目录递归包含空目录和相对路径。
- Windows 与 macOS 非 Finder 目标使用 HopDesk staging 目录；收到“文件已准备好”提示后，用户在目标位置再次粘贴。
- macOS Finder 目标可记录当前目录并直接创建目标文件/目录；需要相应 Finder 自动化权限。无法取得目标目录时回退到 staging/第二次粘贴路径。
- 直接写入遇到同名顶层条目时不覆盖；相对路径中的 `..`、绝对路径和不允许的符号链接必须拒绝。
- 文件传输使用数据 TCP 通道、分块/CRC/FILE_ACK 和批次 DONE/DONE_ACK；图片按像素数据分块，不依赖 PNG 重编码。

## 5. 任务、资源与历史

- 真实 Paste Request 创建独立 Task ID；Task/Reservation 负责资源准入与恰好一次释放。
- 发送与接收方向分别保留状态快照；取消/重试按任务身份处理，不得因 UI 快照而混淆两方向。
- Offer 广播、本机复制、本机粘贴、未粘贴 Offer 不写传输历史。
- UI 的任务列表、文件明细和历史消费只读快照；空闲状态刷新不得反复触碰大块文件表。

## 6. 自动化覆盖与待验收

- Core transaction tests：`tests/xfer_clipboard_txn_tests.c`；两端 Core 测试脚本编译并运行该模块。
- 源码回归检查两端生产路径使用共享 transaction API，并覆盖 Offer/Request 身份、重复/过期请求、完成版本守卫、失败重挂、重连重播、停止和迟到回调。
- PR #22 [Actions #44662680422](https://github.com/alex520lq/HopDesk/actions/runs/44662680422) 的 Windows 与 macOS build/test/package 成功；这不是物理双机剪贴板验收。

以下项目仍为 BLOCKED，必须在真实 Windows + macOS 双设备环境记录：

- 两种 server/client 角色组合下的双向文件、目录、空目录和图片粘贴；
- 连续复制/本机粘贴/跨端粘贴、重复 Paste Request、剪贴板争用；
- 断线、取消、重连、应用退出与迟到回调竞态；
- Finder 目标目录权限、同名文件冲突、staging 回退和清理；
- 吞吐、RSS、峰值内存、CPU、键鼠 RTT、慢盘/弱网和长时间运行。