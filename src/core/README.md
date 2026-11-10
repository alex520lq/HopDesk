# core —— 平台无关核心（纯 C）

两端共享，禁止包含任何平台头文件（windows.h / CoreGraphics / Cocoa 等）。

传输领域已逐步拆出独立生产模块：

- `xfer_ack_window.c/.h`：ACK window、deadline/RTT 和连续 checkpoint 规则。
- `xfer_tracking.c/.h`：每方向传输状态、文件进度、阶段耗时、速度/ETA 和快照规则；调用方负责加锁。
- `xfer_buffer_pool.c/.h`：Windows/macOS 接收端共用的有界 best-fit 缓冲池；调用方在 writer queue lock 下调用，pool 自身负责 slot 和全局 pool-byte ledger。\n- `xfer_sender_context.h`：Sender 平台适配器与协议执行层之间的依赖注入契约；不包含平台类型。当前已在 Windows/macOS 生产发送路径接入时钟、等待与停止观察回调；Socket、取消/ABORT、Task 生命周期等回调仍待逐步接入。
- `src/win/xfer_writer_io.c/.h`、`src/mac/xfer_writer_io.c/.h`：平台文件句柄和随机偏移写入 adapter；不持有 Task/Reservation，不负责 watchdog/status。

职责：

- 协议帧的打包 / 解包（`type + length + payload`）
- 连接状态机（握手、心跳、重连退避、键鼠归属）
- 配置加载与屏幕几何
- 日志
- Windows VK → macOS 键码映射表、修饰键位定义

平台层通过这里定义的接口注册回调（输入捕获/注入、剪贴板读写、显示控制、UI 提示）。
