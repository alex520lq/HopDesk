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
- Sender 本轮保留现有生产实现。`hop_xfer_sender_worker_job_view` 借用平台 Job 内的指针供运行中的 Worker 修改；`hop_xfer_sender_worker_job` 是交给 Worker Flow 作重试和终态决策的值快照，`completion_path`、`peer_cancelled` 在其决策路径中确实被读取。两者名称相近，但生命周期和访问权限不同，合并会扩大共享层对平台 Job 的可变访问，当前没有证据证明能减少维护成本。Sender 的平台桥接还承担锁、IO、状态交接；仅凭回调数量删除会破坏边界。
- 因而本轮实际减少一个 Receiver 公开模块、一个生产调用层级和两端各一个无用全局对象；Sender 的概念数和调用层级没有变化。维护成本的准确工时变化没有基线数据，不作百分比宣称。后续只有找到能完整替代 Sender 某条生产路径且保持唯一状态所有者的设计，才继续动它。
- 本地已通过 `python3 scripts/test-regressions.py`、`bash scripts/test-core.sh`、`bash scripts/build-mac.sh`、`bash scripts/test-xfer-protocol.sh`、`bash scripts/test-network.sh`。Windows 原生构建和测试待本轮 PR CI；真实双机、剪贴板及弱网实测未执行。
