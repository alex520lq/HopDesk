# Sender / Receiver 当前职责边界

> 基线：`main` @ `6e94d8699be6f11730fdf9b14a6df99ad90090d2`（2026-11-20）。PR #17/#18 已合并。
> 本文描述当前生产边界；不把历史架构清单或“文件对称”当成必须继续抽取的目标。

## 1. 职责与唯一所有者

| 状态/资源 | 当前所有者 | 共享 Core 的职责 | 平台 Adapter 的职责 |
|---|---|---|---|
| Sender Job | Windows `xfer_job` / macOS `xf_job` | 借用视图、队列/调度、active marker、Job Lifecycle 与 Task/Reservation 终态规则 | 实际 Job 内存、文件/图片资源、锁和所有权转移 |
| Sender Queue / Scheduler | 平台队列锁 + 共享 Queue/Scheduler | FIFO、等待-重新采样-摘取决策、取消/离线期限策略 | Win32 Event / pthread condvar、线程唤醒和平台锁 |
| ACK / checkpoint | 平台 ACK Coordinator 实例 | ACK Window/Wait、deadline、RTT、连续 checkpoint 和 Engine 决策 | ACK 锁、等待原语、单调时钟采样、socket 回执 |
| Sender Engine | Worker 调用共享 Engine | FILE/IMAGE 发送策略、Chunk/CRC、ACK/checkpoint、DONE/DONE_ACK、重试策略 | 文件读取、原子帧写入、连接快照、Task/日志回调 |
| Receiver Batch/File/Image | Receiver 线程持有 `xfer_rx_context` | Frame Gate、Dispatch、Handlers、Batch/File/Image state、Lifecycle、Receipt、Entry Reservation | socket/线程、路径和目录、文件系统、剪贴板与状态 Services |
| Writer Queue / Buffer | 平台 Writer 持有实际资源 | RX queue slot/bytes 记账、Buffer Pool 额度与借还规则 | 原生锁、Worker 线程、文件句柄和随机偏移写入 |
| Task / Reservation | Task 持有 Reservation handle | Task 状态、资源账本、快照/历史规则 | 调用方同步和终态提交；不得复制/重复释放 handle |

## 2. Sender 生产路径

1. Submit 完成路径扫描和 Task/Reservation 准入。
2. 平台 Queue 在平台锁下将 Job 所有权入队；共享 Scheduler 决定等待、重新采样 peer 状态和摘取哪个 Job。
3. Worker 获取连接/session 快照；共享 Worker Flow/Engine 驱动文件和图片发送、ACK window、checkpoint 与 DONE/DONE_ACK。
4. 取消、peer ABORT、断线和重试由共享规则给出决策；平台负责执行 socket/文件/线程动作。
5. Job Lifecycle 在平台锁与 owner 约束下结束 Job、Task 和 Reservation，确保恰好一次释放。

保留 Engine Adapter 是有意选择：它承担帧编码、ACK 结果映射以及文件发送与窗口策略组合，不是只转发一个指针的空壳。

## 3. Receiver / Writer 生产路径

1. 平台数据连接完成 HELLO/token/session 验证，并提供当前连接 epoch/session 快照。
2. Frame Gate 校验帧身份和 batch；Dispatch 直接选择共享 Message Handler。
3. Handler 调用 Batch/File/Image state 与 Reservation 规则，再通过 Services 请求平台路径、剪贴板、Writer、状态和 ACK 操作。
4. Writer Queue 以 slot + bytes 双重上限做准入；共享 Buffer Pool 管理有界缓冲，平台 Worker/IO 负责实际落盘。
5. 终结路径按序 drain/关闭 Writer、清理 staging、建立 tombstone/receipt、完成 Task 并发布终态；迟到 ABORT/DONE 不得重复释放。

PR #17 已删除 Receiver Protocol Adapter、Sender ACK Adapter 和 Receiver ABORT Executor 回调转发层；Receiver 生产分发从 `Frame Gate → Adapter → Dispatch → Handler` 收敛为 `Frame Gate → Dispatch → Handler`。PR #18 增加 Receiver ABORT 终结副作用顺序的生产路径回归。

## 4. 已知保留项（不是遗漏的“待抽象层”）

- **Sender Retry 状态快照/写回仍存在。** 平台 Job 拥有 `attempt`、`retry_at`、`give_up_at`；Worker Flow 使用决策快照并回写。直接借用平台字段会引入指针生命周期、类型转换和锁约束，目前没有证据证明更简单，因此暂时保留。
- Windows 与 macOS 的 socket/连接循环拓扑不同；共享协议/session 规则，不等于要合并线程模型。
- 平台 `xfer_job`/`xf_job`、ACK Coordinator、Writer 锁/等待原语仍由平台持有；不要将 OS 同步对象搬进纯 C Core。
- 不为文件行数、回调数量或平台对称性继续增加 Coordinator、Action、Adapter 或第二份可变状态。

## 5. 验证状态

- PR #22 的 [Actions #44662680422](https://github.com/alex520lq/HopDesk/actions/runs/44662680422) 中 Windows MSVC 与 macOS build/test/package 均成功；该 run head Tree 与当前 main Tree 一致。Release job 因非 tag 构建而 skipped。
- 自动化覆盖包括 Core、传输协议、TCP loopback、平台连接/角色/重连及接收恢复集成测试。
- 真实双设备断线、取消、重连、双向文件/图片传输、弱网/慢盘、剪贴板争用和长时间运行仍需实机记录；CI/loopback 不可替代。

## 6. 后续修改准则

- 一次只解决一个可独立验收的问题，一个问题一个完整提交。
- 每次修改同时检查 Windows/macOS 生产路径、资源 Owner、锁顺序、失败/取消/Shutdown 竞争和测试。
- 如果无法减少总状态、转发层或理解成本，则保留当前实现并记录证据；不为对称而重构。