# core —— 平台无关核心（纯 C）

`src/core` 只承载平台无关的协议、状态转换、资源策略和业务生命周期。禁止包含 `windows.h`、CoreGraphics、Cocoa、Winsock、pthread 等平台头文件或暴露 OS 类型。平台锁、线程、socket、文件句柄和系统剪贴板仍由各自 Adapter 管理。

## 核心领域

### 协议、会话与基础能力

- `hop.h` / `protocol.c`：公共消息类型、配置/状态契约与 TCP 帧编解码。当前控制协议 v7、数据协议 v8。
- `session.c`：共享 session 身份与 compare-and-clear 规则。
- `config.c` / `config_migrate.h`：配置读取、校验、迁移和原子保存规则。
- `crc32.c`、`clock.c`、`watchdog.c`、`log.c`、`keymap.c`：校验、时钟、看门狗、日志与键码映射等基础能力。

### Clipboard

- `xfer_clipboard_txn.c/.h`：Offer generation/identity、remote Offer、Paste Request 去重/准入、完成版本守卫、Worker admission、Offer 退役/重播意图和 Paste Worker admission。
- 平台层仍负责 Windows clipboard sequence/HGLOBAL 与 macOS NSPasteboard/changeCount、路径快照、图片转换、主线程/Worker 调度和系统写入。
- `src/win` 与 `src/mac` 仍有 `clipboard_offer.inc`、`clipboard_paste.inc` 兼容实现片段；它们属于平台 Adapter，不是 Core 模块，也不意味着两端 OS 细节已统一。

### Transfer 公共状态与资源策略

- `xfer_task.c/.h`、`xfer_task_history.c/.h`：Task 生命周期、身份、快照与历史。
- `xfer_budget.c/.h`：全局资源预算和 Reservation 记账。
- `xfer_tracking.c/.h`、`xfer_status.h`：发送/接收状态、文件进度、阶段计时、速度/ETA 与快照规则；调用方负责加锁。
- `xfer_rate.c/.h`：速率控制策略。
- `xfer_ack_window.c/.h`、`xfer_ack_coordinator.h`：ACK window、deadline/RTT 与连续 checkpoint 规则；实际平台锁和 ACK Coordinator 实例仍在 Adapter。
- `xfer_buffer_pool.c/.h`：有界 best-fit 缓冲池及 pool-byte ledger。

### Sender

- `xfer_sender_engine.c/.h`：文件/图片发送策略、CRC/分块顺序、ACK checkpoint 和 DONE/DONE_ACK 提交/重试策略。
- `xfer_sender_chunk.c/.h`、`xfer_sender_ack_wait.c/.h`、`xfer_sender_retry.c/.h`：Chunk 处理、ACK 等待和重试决策。
- `xfer_sender_queue.c/.h`、`xfer_sender_scheduler.c/.h`、`xfer_sender_active_job.c/.h`：FIFO、等待/摘取 Job、取消与 active marker 规则。
- `xfer_sender_job_lifecycle.c/.h`、`xfer_sender_lifecycle.c/.h`：Job/Task/Reservation 终态和释放顺序。
- `xfer_sender_worker.c/.h`、`xfer_sender_worker_flow.c/.h`：共享 Worker 执行流程与结果决策。
- `xfer_sender_context.h`、`xfer_sender_context_adapter.c/.h`、`xfer_sender_engine_adapter.c/.h`：显式依赖注入与 Engine/Context 边界。平台 Adapter 提供文件句柄、socket 写帧、锁/等待原语、时钟和回调。

### Receiver / Writer

- `xfer_rx_core.c/.h`：共享 Receiver 核心状态与策略（Frame Gate、消息分发、DONE Receipt、Session/Tombstone Lifecycle、Batch Rules/State、Entry Reservation、File/Image 状态与缓冲、入站 ACK 解码，以及 `xfer_rx_context` / `hop_xfer_rx_services` 契约）。
- `xfer_rx_message_handlers.c/.h`：共享协议消息处理入口与默认分发表 `hop_xfer_rx_default_dispatch_handlers`。
- `xfer_rx_queue.c/.h`、`xfer_validate.c`：Writer 队列容量/记账与输入校验。
- Writer 的锁、线程、文件句柄与平台 IO 分别由 `src/win` / `src/mac` 的 `xfer_writer_queue`、`xfer_writer_worker`、`xfer_writer_io` Adapter 实现；Core 只定义共享容量和 buffer 规则。

## 边界约束与未完成事项

- Core 不创建线程、不等待、不访问 socket/文件系统/系统剪贴板，也不获取平台锁；需要同步的调用由 Adapter 在明确的锁范围内完成。
- 平台 `xfer_job` / `xf_job` 仍拥有实际 Job 资源；Core 接收借用视图/快照，不持有平台 Job 内存。
- Sender Retry 目前仍有平台 Job 字段与 Core 决策快照之间的状态写回；是否进一步统一须先证明不会增加指针生命周期或锁复杂度。
- Windows/macOS 的连接循环与部分协议 transport `.inc` 仍是平台实现；不要为了文件形式对称而机械抽象。
- `xfer_sender_protocol.inc` 已不在当前平台源码树中；不要再将其列为待拆除文件。
- 模块测试见 `tests/` 和 `scripts/test-core.*`、`test-xfer-protocol.*`、`test-network.*`。新增 Core 模块必须进入两端生产构建及适用测试目标。