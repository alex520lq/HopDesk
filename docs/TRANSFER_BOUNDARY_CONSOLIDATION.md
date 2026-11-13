# Sender / Receiver 边界收敛计划

> 基线：`origin/main` @ `a293292fbb010713c7caaf3c825fe9d01cba7dba`。本轮使用同一活动分支和 PR；先证明生产路径变简单，再修改代码。此文是执行契约，不把历史架构清单当成必须继续抽取的目标。

## 状态与职责

| 状态或资源 | 唯一所有者 | 其他模块的权限 |
|---|---|---|
| Sender Job（batch、Task handle、entry snapshot、retry 时间、已确认 checkpoint、像素所有权） | 平台 `xfer_job` / `xf_job`；Core 操作其借用视图 | Queue 仅借用链表节点；Worker 按所有权状态转移；Job Lifecycle 最终释放 |
| Sender Queue 与 active marker | 共享 Queue / Active Job 规则，平台锁序列化 | Scheduler 在同一锁内选取并认领 Job；平台条件变量/Event 只负责等待 |
| FILE_ACK 窗口 | 共享 ACK Window，由平台 ACK Coordinator 持有并加锁 | Receiver 只投递回执；Sender 等待并按 ACK 推进 Job checkpoint |
| Receiver Batch、文件/图片连续性、Task | Receiver 线程持有 `xfer_rx_context` 及其子状态 | 帧准入和 Handler 只在 Receiver 所属线程改变状态；Writer 不决定 Task 终态 |
| Receiver session/tombstone 与 DONE receipt | Receiver lifecycle / receipt，借用到 context | 新 session 使旧 tombstone/receipt 失效；旧帧不得污染新 Batch |
| Writer Queue、缓冲和文件句柄 | 平台 Writer；共享 Queue/Pool 只计算有界额度 | Receiver 提交写入并在终结前 drain；Writer 不发布协议终态 |
| Reservation 与全局额度 | Task 持有 Reservation handle，Budget 是账本 | Sender/Receiver 通过 Task API 预留和释放；不得复制 handle 或绕过 Task |

## 生产路径与边界

1. **Sender**：Submit 完成扫描和 Task 准入 → Queue/Scheduler 在平台锁内转移 Job 所有权 → Worker 读取会话快照 → Engine 负责 FILE/IMAGE 发送顺序、ACK 窗口和 checkpoint → Worker Flow 决定重试或终态 → Job Lifecycle 完成 Task/Reservation 释放。平台保留 socket、文件、线程、锁和时钟。
2. **Receiver**：数据通道认证与 Session 快照 → Frame Gate 验证 token/session/batch → Dispatch 选择 Handler → Handler 驱动 Batch/File/Image 规则及有界 Writer → ACK/DONE 回送 → 终结时 drain Writer、结束 Task、清理 staging。平台保留原生路径、磁盘、socket 和剪贴板提交。
3. **跨端交接**：ACK 以 `(batch,id)` 进入 Coordinator；只有成功远端 ACK 推进 Sender checkpoint。DONE receipt 用于幂等重放，不是第二个 Task 所有者。

## 本轮改动判据

- **确定收敛**：Receiver Protocol Adapter 只复制 handlers/context 并转调 Dispatch；两端还有未用于分发的全局 adapter。删除该对象和中转调用，让 Frame Gate 后直接走既有 Dispatch。保留已覆盖的未知消息、缺失 handler、session、旧 batch 与认证失败用例。
- **Sender 先证后改**：一个 Job 目前经过平台 Worker Adapter、共享 Worker、平台 run/finish 回调、Engine Adapter、Engine。逐个核对回调桥：若包含锁、所有权转移、错误映射或 OS 语义则保留；若同一 Job 字段仅为接口形状被复制和回写，给出合并后的唯一视图、调用路径和删除清单后才实施。
- 不新增 Coordinator、Action、长期转发 API 或第二份可变状态。不以行数或回调数量作为单独验收指标。

## 必须保持的行为与验证

- 取消、peer ABORT、断线、Shutdown 与 DONE 的竞争；Job 恰好一次释放，Task/Reservation 不泄漏、不重复释放。
- ACK 早到、迟到、重复与旧 Batch 拒绝；只有确认过的连续文件推进 checkpoint。Receiver 的 CRC、路径/长度校验、Writer 背压和失败清理不得削弱。
- 每个改动同时核对 Windows/macOS 生产路径。运行 `bash scripts/test-core.sh`、`python3 scripts/test-regressions.py`、`bash scripts/build-mac.sh`、`bash scripts/test-xfer-protocol.sh`、`bash scripts/test-network.sh`；Windows MSVC 构建与测试由本轮 PR CI 验证。真实双机、弱网和长时间运行只有实际执行后才能标记通过。
- 完成时列出删除的接口/对象、改动前后生产调用层级、状态所有者是否仍唯一；如果 Sender 找不到能降低整体理解成本的完整改法，本轮不为对称而修改 Sender，并记录证据。

## 本轮执行结论

- Receiver 已删除 `xfer_rx_protocol_adapter.[ch]`、两端未使用的 adapter 全局对象，以及两端各一处重复的 `s_done_receipt` 声明。生产分发由 `Frame Gate → Protocol Adapter → Dispatch → Handler` 收敛为 `Frame Gate → Dispatch → Handler`；`Dispatch` 仍负责类型选择和缺失 handler 拒绝。没有增加新状态或公开接口。
- Sender 的独立 ACK Adapter 确认为纯包装：它持有平台 ACK Coordinator 和时钟，再把 Context 的八个 ACK 回调逐一转发给 Coordinator。现改为 Context 借用一个 `hop_xfer_ack_coordinator *`；Core 的文件、DONE commit 与 ACK wait 直接调用 Coordinator。两端仍各自持有锁、等待原语、创建/销毁和单调时钟采样。删除 `xfer_sender_ack_adapter.[ch]`、八个 Context 回调和 Engine/File/Commit 中对应的 ACK 回调字段；`Sender → ACK Adapter → Coordinator` 变为 `Sender → Coordinator`。Shutdown 仍先唤醒、join Worker、drain Job，最后销毁 Coordinator。
- Receiver 的 ABORT 决策仍由共享 Batch Rules 提供，终结的有序副作用改由唯一持有 `xfer_rx_context` 的 Message Handlers 直接执行。删除 `hop_xfer_rx_abort_ops` 的七个回调、`hop_xfer_rx_abort_execute`、结果枚举和三处同文件转发函数；`Handler → Abort Executor → 七个回调 → Receiver services` 变为 `Handler → Receiver services`。先取消 Paste、再通知对端、关闭 Writer/清理 staging、立墓碑、结束 Task、清空 Batch、发布终态的顺序保持。
- Sender Engine Adapter 保留。它除接口转换外还负责文件和图片帧编码、ACK 结果映射以及文件发送与窗口策略的组合；仅将其挪进 Engine 文件不会消除这些职责。其中仅包着 `ops` 指针的 `sender_file_adapter_call` 已删除，File Ops 直接借用现有执行参数。`hop_xfer_sender_worker_job_view` 与 `hop_xfer_sender_worker_job` 分别是运行时借用视图和终态决策快照，不是两个 Job 所有者。
- 此轮净删除两个共享包装接口（Receiver Protocol Adapter、Sender ACK Adapter）及 Receiver ABORT 执行回调表，三条生产路径各少一层转发；没有增加业务状态或 OS 抽象。工程维护时间和 CPU/RSS/包体的前后差值尚未测量，不能据源码删减宣称资源改善。
- 本地已通过 `python3 scripts/test-regressions.py` 和 `bash scripts/test-core.sh`；Sender 改动后的 `bash scripts/build-mac.sh`、`bash scripts/test-xfer-protocol.sh`、`bash scripts/test-network.sh` 也通过。最终 Windows 原生构建与测试待本轮 PR 最新提交 CI；真实双机、剪贴板和弱网实测未执行。

## 后续小范围审计：Sender Retry 与 Receiver ABORT

- **Sender Retry：保留现状，问题仍存在。** Windows `xfer_job` 与 macOS `xf_job` 都独占 `attempt`、`retry_at`、`give_up_at`；Queue 读取其时间，Worker Flow 在断线时更新决策快照，再通过 `store_retry_state` 写回，随后才在平台队列锁内转移 Job 所有权。没有第二个长期 Job，但确有一次可变状态复制和回写。平台 `attempt` 是 `int`，Core 快照是 `uint32_t`；直接借用三个字段需要让 Core 持有可变平台指针并处理类型转换与有效期，或者同时改两端 Job 布局和 Queue 接口。它只消除一个写回回调，却增加指针契约或扩大重构范围，当前没有证明整体更简单。因此本项**未收敛**，不为消除复制而改代码。若未来统一 Job 中的重试状态布局，应再以所有权转移、类型范围、Shutdown 竞争和重入队行为为验收条件评估唯一视图。
- **Receiver ABORT：补回生产路径顺序回归。** `receive_abort_fixture` 继续提取真实 `hop_xfer_rx_terminate_batch`，在两端平台服务回调及 Task 终态观察口记录顺序：取消 Paste → 发送 ABORT → 关闭 Writer → 清理 staging → Task 终态 → 发布状态；Task 观察口确认墓碑已建立，状态发布口确认 Batch 重置且 Task handle 已释放。另核对迟到 ABORT 只回执、不重复终结。测试覆盖了可观察副作用及关键状态边界，不代表真实双机、原生剪贴板或文件系统验证。
