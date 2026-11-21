# HopDesk 当前架构审计

> 审计基线：`main` @ `6e94d8699be6f11730fdf9b14a6df99ad90090d2`（2026-11-20）。
> 应用版本 `0.5.0`；控制协议 v7；数据协议 v8。
> 本文替代旧版 `main @ c00bf9d9` 的行号/职责审计；旧行数和早期模块候选不再代表当前实现。

## 1. 总体结论

HopDesk 当前采用 **共享纯 C Core/Application + Windows Win32/Winsock Adapter + macOS POSIX/Cocoa Adapter**。传输与剪贴板的主要业务状态和策略已从平台大文件迁入独立 Core 生产模块；系统调用、同步原语、线程、socket、文件句柄、原生剪贴板、键鼠与 UI 仍由平台层持有。

当前不应继续以“平台文件行数多”或“两个平台文件相似”为理由机械抽象。下一轮应针对真实重复状态、跨层转发、锁/所有权不清晰和缺少生产路径测试的问题逐项审计。

## 2. 当前协议与产品边界

- 控制协议版本：`HOP_CTRL_PROTO_VER = 7`。
- 数据协议版本：`HOP_XFER_PROTO_VER = 8`；数据帧绑定控制 session，并在非 HELLO 帧中携带 `session(16) + batch(4)`。
- 数据 v7 增加带目标方向的 ABORT；数据 v8 增加 `XFER_DONE_ACK` 批次终结回执。
- 控制通道默认 TCP 24817，数据通道默认 TCP 24818；当前没有 TLS，不应暴露到公网或不可信网络。
- 产品目标仍是双设备局域网；不提供多设备发现、公网中继或断点续传。

## 3. Core 当前模块

### Clipboard

- `src/core/xfer_clipboard_txn.c/.h` 是单一共享 Clipboard transaction production module。
- Core 拥有 Offer generation/identity、remote Offer、Paste Request ID/去重/准入、完成版本守卫、Worker admission 和 Offer 退役/重播决策。
- Windows/macOS Adapter 保留原生 clipboard sequence/changeCount、HGLOBAL/NSPasteboard、路径快照、图片转换、系统写入、锁和线程/主队列调度。
- `clipboard_offer.inc` 与 `clipboard_paste.inc` 仍存在于两个平台目录；它们是平台 Adapter 的兼容 include 片段，不应误写为 Core 模块，也不应只改扩展名就宣称完成抽取。

### Sender

- 共享模块覆盖 ACK Window/Wait、Chunk、Engine、Queue、Scheduler、Active Job、Job Lifecycle、Worker/Worker Flow、Retry 和 Context/Engine Adapter。
- 共享 Core 负责平台无关的发送顺序、ACK/checkpoint、重试时机/队列决策、取消与 Job/Task/Reservation 生命周期规则。
- Windows/macOS 仍拥有各自实际 `xfer_job` / `xf_job` 内存、socket、文件句柄、平台锁、等待原语和线程。
- `xfer_sender_protocol.inc` 已不在当前 `src/win` / `src/mac` 文件树中；不要继续把它列为待拆除文件。

### Receiver / Writer

- Receiver Core 包含 Frame Gate、Dispatch、Message Handlers、Batch/File/Image state、Lifecycle、Receipt、Entry Reservation、ACK handler 和 RX queue rules。
- 平台 `xfer_receive.c` 仍负责 Receiver 线程/连接生命周期、平台路径与文件系统、socket、剪贴板/状态 Services。
- 共享 `xfer_rx_queue` 和 `xfer_buffer_pool` 提供有界 slot/bytes 与缓冲池记账；平台 Writer Queue/Worker/IO 负责原生锁、线程、句柄和实际磁盘操作。
- PR #17 已删除 Receiver Protocol Adapter 和 ABORT 执行回调转发层；PR #18 增加终结副作用顺序回归。

### 其他共享 Core

- `xfer_task` / `xfer_task_history`：任务身份、生命周期和历史。
- `xfer_budget`：Reservation 与全局预算账本。
- `xfer_tracking` / `xfer_status`：方向状态、文件进度、计时、速度/ETA 和快照。
- `xfer_rate`：速率控制策略。
- `session` / `protocol` / `config`：会话、帧协议和配置基础能力。

完整模块职责清单见 [`src/core/README.md`](../src/core/README.md)。

## 4. 平台目录职责

| Windows `src/win` | macOS `src/mac` | 共同职责/差异 |
|---|---|---|
| `win_main.c` / `ui.c` / `main_ui.c` | `main.c` / `statusbar.m` / `main_ui.m` | 原生进程、托盘/菜单栏、四页主窗口；不得把 OS 控件搬入 Core |
| `net_server.c` / transport includes | `net_client.m` / transport includes | 各自 socket、连接循环、重连和线程拓扑；共享协议/session 规则。macOS 控制线程在每批解码结束时清空临时 Cocoa 对象。 |
| `clipboard.c` + offer/paste includes | `clipboard.m` + offer/paste includes | 共享事务 Core；原生剪贴板与图片/路径桥接各自实现 |
| `xfer.c` / `xfer_sender*.c` / `xfer_receive.c` | 对应 macOS 实现 | 平台 Job/锁/socket/线程与 Core 规则组合 |
| `xfer_writer_queue/worker/io` | 对应 macOS 实现 | 平台 writer 与文件句柄；共享队列记账和 buffer pool |

## 5. UI 当前实现

- 两端主窗口均采用四个一级入口：概览、设备、传输、设置；日志与软件更新归入设置。
- Windows 使用 Win32 原生窗口；macOS 使用 Cocoa 原生窗口。
- Windows 托盘图标：断开时灰度/灰点，已连接蓝点，共享中绿点；传输时显示进度环，tooltip 显示方向与百分比。左键打开主窗口，右键显示打开/退出菜单。
- macOS 菜单栏图标：连接/共享状态使用灰/蓝/绿样式；传输时显示进度环和百分比。左键打开主窗口，右键显示打开/退出菜单。
- UI 使用传输状态快照；空闲刷新不应触碰大块文件明细表。PR #22 对 idle tracking reset 改为只重置文件表之前的元数据，并修复稀疏 file ID 初始化范围。

## 6. 自动化 CI 证据与覆盖

当前主干最近的 PR #22 构建验证为 [Actions #44662680422](https://github.com/alex520lq/HopDesk/actions/runs/44662680422)。其 head Tree 与 main Tree 相同。

- Windows：MSVC app build/package、Core tests、file-transfer protocol tests、TCP loopback、native control/data role + reconnect integration。
- macOS：Apple Clang app build/package、Core tests、file-transfer protocol tests、TCP loopback、control/listener loopback、native receive/resume integration。
- Windows 与 macOS build/test/package jobs 均 success；Release publication job skipped（非 tag 构建）。

CI 不覆盖所有实机行为，尤其不证明真实双机剪贴板争用、两角色组合完整文件传输、权限提示、DDC 显示器、弱网/慢盘、8 小时空闲和长时间大文件传输。

## 7. 当前已知剩余问题与审计边界

1. **Sender Retry 状态快照/写回**：平台 `xfer_job` / `xf_job` 仍拥有 attempt/retry deadline；Core Worker Flow 使用决策快照并回写。当前没有证明直接借用平台字段会更简单，进一步重构前必须比较指针生命周期、类型转换、锁序和 Shutdown 竞态。
2. **Connection/session 平台循环**：Windows 与 macOS 的连接/监听拓扑不同。共享认证/session 语义，不代表应强行统一 socket loop 或线程模型。
3. **平台 `.inc`**：Clipboard offer/paste 与 network protocol/transport 仍有 include 片段。逐个审计其重复业务规则；只搬文件或改扩展名不算模块化。
4. **全量 Owner/lock 审计**：当前模块边界和关键生命周期已有契约与回归，但尚未完成所有静态变量、锁顺序、迟到回调、线程退出与动态分配的全量清单。
5. **资源与性能测量**：当前没有可比的 Windows/macOS RSS、CPU、吞吐、键鼠 RTT、启动时间和包体前后数据。不能凭源码行数或模块拆分宣称性能提升。
6. **实机验收**：真实双机文件/文件夹/图片粘贴、取消/断线/重连、剪贴板争用、权限、显示器、弱网/慢盘和长稳项目，必须按 `MANUAL_TEST_RUNBOOK.md` 逐项记录 PASS/FAIL/BLOCKED。

## 8. 后续重构准则

- 一次只解决一个问题，一个问题形成一个完整提交。
- 先审计两端生产调用链、Core、资源 Owner、锁/线程和失败路径，再设计边界。
- 只有共享同一业务规则且能删除重复状态/转发时才抽取；真实 OS 差异留在 Adapter。
- 每个新 Core 模块必须进入 Windows/macOS 生产构建和对应测试目标。
- 重构完成后列出删除的旧接口/包装、生产调用链变化、Owner 变化和双平台测试证据。
- 未执行的真实双机和性能测试明确标记 BLOCKED，不以 CI 或 loopback 替代。
