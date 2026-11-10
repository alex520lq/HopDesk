# core —— 平台无关核心（纯 C）

两端共享，禁止包含任何平台头文件（windows.h / CoreGraphics / Cocoa 等）。

传输领域已逐步拆出独立生产模块：

- `xfer_ack_window.c/.h`：ACK window、deadline/RTT 和连续 checkpoint 规则。
- `xfer_tracking.c/.h`：每方向传输状态、文件进度、阶段耗时、速度/ETA 和快照规则；调用方负责加锁。
- `xfer_buffer_pool.c/.h`：Windows/macOS 接收端共用的有界 best-fit 缓冲池；调用方在 writer queue lock 下调用，pool 自身负责 slot 和全局 pool-byte ledger。
- `xfer_sender_engine.c/.h`：共享 Sender 文件 ACK 窗口/连续 checkpoint、单文件 BEGIN/CHUNK/END 与 CRC/文件 IO 策略、图片分块/CRC 顺序；平台只提供文件句柄、帧发送、ACK、时钟和状态回调。
- `xfer_sender_context.h`：Sender 平台适配器与执行层之间的完整依赖注入契约，覆盖时钟/可中断等待、停止/取消/ABORT、Session 校验、原子帧写入、Task 生命周期和可选遥测；不包含平台类型。Windows/macOS 生产路径已接入这些依赖。
- `src/win/xfer_sender_protocol.inc`、`src/mac/xfer_sender_protocol.inc`：仍是待拆除的兼容 include 边界；当前协议适配逻辑逐步迁入独立编译单元。
- `src/win/xfer_writer_io.c/.h`、`src/mac/xfer_writer_io.c/.h`：平台文件句柄和随机偏移写入 adapter；不持有 Task/Reservation，不负责 watchdog/status。

职责：

- 协议帧的打包 / 解包（`type + length + payload`）
- 连接状态机（握手、心跳、重连退避、键鼠归属）
- 配置加载与屏幕几何
- 日志
- Windows VK → macOS 键码映射表、修饰键位定义

平台层通过这里定义的接口注册回调（输入捕获/注入、剪贴板读写、显示控制、UI 提示）。
