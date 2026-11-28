# HopDesk 架构重构计划与当前状态

> 当前基线：`main` @ `6e94d8699be6f11730fdf9b14a6df99ad90090d2`（2026-11-20）。
> 应用版本 `0.5.0`；控制协议 v7；数据协议 v8；开放 PR：0。
> 本文以当前 main 为事实基线；早期分阶段规划和逐次提交记录已压缩为当前实现与后续工作，不再保留过期的“待提交/待 CI”状态。

## 1. 当前实现

| 子系统 | 已进入 main 的实现 | 当前边界 |
|---|---|---|
| Clipboard | `xfer_clipboard_txn.c/.h` 统一 Offer/Request/Paste 事务状态、身份、去重、准入和完成守卫（PR #15/#16） | 原生 clipboard sequence/changeCount、路径快照、图片转换、系统写入和 Worker 调度仍由平台 Adapter 负责 |
| Sender | 共享 ACK Window/Wait、Chunk、Engine、Queue、Scheduler、Active Job、Job Lifecycle、Worker Flow、Retry 与 Context Adapter | 平台持有 `xfer_job`/`xf_job` 实际内存、socket、文件句柄、锁、等待原语和线程 |
| Receiver | 共享 Frame Gate、Dispatch、Message Handlers、Batch/File/Image state、Lifecycle、Receipt、Entry Reservation、ACK handler | 平台 `xfer_receive.c` 持有连接线程、socket、原生路径和 Services；Protocol Adapter 与 ABORT 转发层已删除（PR #17/#18） |
| Writer | 共享 RX Queue accounting、Buffer Pool | 平台 Writer Queue/Worker/IO 持有原生锁、线程、文件句柄和实际磁盘操作 |
| UI | Windows Win32 与 macOS Cocoa 原生四页主窗口；托盘/菜单栏显示连接/共享/传输状态（PR #20/#21） | 不引入 Electron、Qt 或 WebView；空闲刷新避免触碰大块文件明细（PR #22） |

### 1.3 原生平台与共享 Core：显示器切换边界

共屏切换的顺序、失败回滚策略和事务帧校验由 `src/core/display_switch.c` 唯一实现。Windows Adapter 持有工作线程、事件、连接 epoch、目标设备、原模式文件和 DDC/显示拓扑 API；Mac Adapter 持有唤醒断言、命令子进程和 session 快照。两端原生 UI 复用原有按钮，Windows 为共屏拓扑唯一协调者。普通模式保留平台输入源行为。

本切片增加共享生产规则，删除切屏按钮在共屏场景下各自独立操作的路径；Windows 新增的原生拓扑操作留在 Adapter，未引入运行时或附加依赖，也未更改资源目标。自动化只证明流程/协议和编译正确，性能优劣与真实显示器效果须实测。详见 [共屏切换](SHARED_DISPLAY_SWITCH.md)。

## 2. 当前主干验证

- PR #22 合并提交：`6e94d8699be6f11730fdf9b14a6df99ad90090d2`。
- 对应 Actions：[Build HopDesk #44662680422](https://github.com/alex520lq/HopDesk/actions/runs/44662680422)。PR head `2b3530f2e23d297fb98cf7d929dfd52e71f1e673` 的 Git Tree 与 main Tree 一致。
- Windows MSVC 与 macOS build/test/package 均 success；Release publication job skipped（非 tag 构建）。
- CI 覆盖 Core、文件传输协议、TCP loopback、平台角色/连接/重连及接收恢复集成测试；不等于真实双机、交互 UI、系统权限、显示器硬件或长稳验收。

## 3. 下一阶段工作

| 优先级 | 工作 | 完成标准 |
|---|---|---|
| P0 | 真实双设备端到端验收 | Windows server → macOS client、macOS server → Windows client；文件/目录/图片、取消、断线、重连、剪贴板争用均有结果记录 |
| P1 | Sender Retry 状态快照/写回审计 | 比较现有快照/写回与借用平台 Job 字段的复杂度；只有能减少总状态和生命周期复杂度才改 |
| P1 | Clipboard/Network `.inc` 边界审计 | 找出真实重复业务决策；只在能删除重复状态/转发时抽取，不为文件后缀或对称性重构 |
| P1 | Connection/Session 边界审计 | 共享协议/session 规则，保留 Windows/macOS 不同 socket 与线程拓扑；不强行统一 actor |
| P2 | 完整 Owner/Lock 清单 | 记录全局状态、锁顺序、线程退出、迟到回调、句柄和动态分配的所有权闭环 |
| P2 | 性能与资源测量 | 同设备/数据集测量 RSS、CPU、吞吐、键鼠 RTT、启动和包体；提供前后对照 |

## 4. 自动化与实机验收边界

- 自动化命令和 CI 目标见 `scripts/test-core.*`、`scripts/test-xfer-protocol.*`、`scripts/test-network.*` 与 `.github/workflows/build.yml`。
- 真实双机、弱网、慢盘、权限、DDC、长时间传输和空闲稳定性必须在真实设备上执行并记录 SHA、OS、网络、数据集、命令、结果。
- 未执行项目标记 `BLOCKED`；不得用本机 loopback、mock 或 CI 通过推断真实设备通过。

## 5. Git 与提交纪律

- `main` 是唯一长期主干；默认最多一条短期活动分支和一个 PR。
- **一个问题 = 一个完整提交**：Core、两端 Adapter、调用方、测试、构建配置、文档和旧代码清理应在同一提交内闭环。
- 禁止把一个问题拆成接口先行、平台分批、调用方迁移、包装清理和补测试等碎片提交；多个无关问题也不得混在同一提交。
- 默认通过 PR 集成；用户明确授权直接提交 main 时，多个文件必须组成一个 Git Tree 和一个 commit，并使用预期 HEAD 防止覆盖新提交。
- 合并后核实内容进入 main，再按授权删除已合并分支；不得删除未合并或有独有工作的分支。

## 6. 最近合并记录

| PR | 合并日期（UTC） | 合并提交 | 摘要 |
|---:|---|---|---|
| #15 | 2026-11-12 | `db40605bf90f04de92200b015aa00a0e6d3e2bab` | Clipboard paste transaction coordination |
| #16 | 2026-11-12 | `a293292fbb010713c7caaf3c825fe9d01cba7dba` | Clipboard transaction domain consolidation |
| #17 | 2026-11-13 | `cef5d745f4bfdafc3f7a10ed3f4b9322c0ed1fef` | Transfer boundary consolidation |
| #18 | 2026-11-13 | `e01958faf9be14425ca85f214cd0e7b20baadce9` | Receiver ABORT terminal-order regression |
| #19 | 2026-11-15 | `34a0c4bd7eabd62b367875d131ea811858b0335f` | macOS local paste permission onboarding |
| #20 | 2026-11-15 | `625bd0e68ec3119a548dc9333dd39c09f9dc4404` | Status-aware tray/menu-bar icon |
| #21 | 2026-11-19 | `eab0a9327dc528886cfbc0c5aad3a4f33b293a41` | Native dashboard, transfer and settings UI polish |
| #22 | 2026-11-20 | `6e94d8699be6f11730fdf9b14a6df99ad90090d2` | Avoid touching idle transfer file tables; macOS log menu memory fix |

## 7. 分支清理核验

已核实以下七个分支均对应已合并 PR，且通过 Git 对象 API 读取 Tree SHA 后，分支 tip Tree 与对应 merge commit Tree 完全一致：

- `feat/status-aware-tray`（PR #20）
- `fix/clipboard-domain-consolidation`（PR #16）
- `fix/mac-permission-onboarding`（PR #19）
- `fix/transfer-boundary-consolidation`（PR #17）
- `fix/transfer-retry-abort-audit`（PR #18）
- `refactor/clipboard-coordinator`（PR #15）
- `refactor/native-ui-first-pass`（PR #21）

用户已授权删除。当前 GitHub 工具连接没有删除 Git ref/branch 的操作，因此这些分支尚未实际删除；在重新读取远程分支列表确认之前，不得声称仓库只剩 `main`。