# HopDesk 架构重构实施规划

> 状态：实施基线已建立；按本文件逐项推进。未在“实施记录”中附提交和测试证据的工作，不得标记完成。
> 基线主干：`main` @ `c00bf9d9fc8f0bf89f1bd2a99b8728b5cefd48e8`
> 活动分支：`refactor/architecture-modularization`
> 目标：在不改变产品行为、网络协议和用户工作流的前提下，将 HopDesk 从平台文件堆叠式组织演进为边界清晰、可独立测试、跨平台规则一致的模块化工程。

## 1. 重构目标与非目标

### 1.1 目标

1. 每个模块有唯一、可描述的职责和明确公开接口。
2. 共享业务规则放在纯 C Core/Application；Win32、POSIX、Cocoa 细节留在平台适配层。
3. 任务、Reservation、缓冲区、线程和 socket 的所有权明确，避免双重释放、悬空引用、重复释放和状态分叉。
4. Windows/macOS 对等功能遵循相同状态机、错误语义和验收用例。
5. 核心模块可独立编译/测试；集成测试覆盖真实生产编译单元，而非仅依赖源码提取拼接。
6. 逐步降低超大文件复杂度，不以行数作为唯一拆分依据。
7. 每个重构增量可单独审查、构建、测试和回滚。

### 1.2 非目标

- 不重写为 C++、Rust 或其他语言；保留现有 C + Win32 / C + Objective-C 技术栈。
- 不在本轮无必要地改变协议版本、帧格式、端口、身份认证和会话语义。
- 不改变键鼠共享、双角色连接、剪贴板、Lazy File Paste 的既有产品行为。
- 不做无基准的性能“优化”，不以更大 chunk、更高线程优先级或无界并发换取吞吐。
- 不为追求目录整齐而机械搬移文件或创建过度抽象层。
- 不将 Windows 与 macOS 的系统 API 强行统一；只统一业务契约和状态规则。

## 2. 当前基线与审计结论

### 2.1 仓库状态

- 默认分支：`main`。
- 基线提交：`c00bf9d9fc8f0bf89f1bd2a99b8728b5cefd48e8`。
- 开始本轮时未发现开放 PR，远程仅有 `main` 长期分支；据此创建唯一活动分支 `refactor/architecture-modularization`。
- 仓库已有 `AGENTS.md`，本轮必须遵守其单活动分支、PR、跨平台同步和测试证据要求。
- 已有 `docs/architecture.md` 与 `docs/FILE_CLIPBOARD_TRANSFER_REFACTOR.md`。本规划补充“代码模块化/依赖边界”议题，不取代既有 Lazy File Paste 与传输可靠性验收清单；已有实现和历史记录必须保留，状态以当前主干代码和本轮 CI 重新核实为准。

### 2.2 文件规模快照

以下是基于基线主干的源码行数/文件规模检查结果（行数用于定位审计对象，不作为强制拆分阈值）：

| 文件 | 基线规模 | 审计关注点 |
|---|---:|---|
| `src/win/xfer.c` | 约 1,913 行 | 发送、任务队列、状态/历史、限速、ACK、生命周期混杂 |
| `src/mac/xfer.c` | 约 1,860 行 | 与 Windows 对应逻辑并行演进，跨平台规则重复 |
| `src/win/xfer_receive.inc` | 约 1,415 行 | 接收协议、写盘队列、文件/会话生命周期耦合 |
| `src/mac/xfer_receive.inc` | 约 1,278 行 | 与 Windows 接收实现相似但存在平台差异 |
| `src/win/main_ui.c` | 约 860 行 | 页面、配置、任务展示、日志刷新 |
| `src/mac/main_ui.m` | 约 805 行 | Cocoa 页面、配置、任务展示混合 |
| `src/win/clipboard.c` | 886 行 | 剪贴板协议、图片工作线程、粘贴事务混合 |
| `src/mac/clipboard.m` | 766 行 | NSPasteboard、图片转换、异步任务和协议混合 |
| `src/mac/statusbar.m` | 585 行 | 状态栏、传输窗口、更新流程混合 |
| `src/core/hop.h` | 474 行 | 协议常量、配置、多个消息结构/接口集中定义 |
| `tests/core_tests.c` | 约 1,500+ 行 | 多领域测试集中；需按模块逐步拆分 |
| `scripts/test-regressions.py` | 约 870+ 行 | 源码结构耦合的回归夹具较多 |

### 2.3 已确认的结构性问题

1. `xfer.c` 通过 `#include "xfer_receive.inc"` 引入接收端实现；这不是独立编译模块，接收代码依赖同一翻译单元中的静态状态和函数。
2. Windows/macOS 各自维护发送、接收、剪贴板、状态展示代码。平台 API 差异合理，但任务状态转换、ACK/checkpoint、预算/释放语义应有共享规则和跨平台一致测试。
3. 传输层同时承担任务队列、扫描、网络 IO、ACK、速率控制、历史和 UI 状态桥接，职责过载。
4. 剪贴板层同时承担平台 API、Offer/Request 协议、图片解码/转换、异步任务调度和结果提交，生命周期边界复杂。
5. UI 文件包含配置、状态快照处理和页面构建；业务状态计算应从原生控件中分离。
6. 部分回归测试依赖源码提取/拼接，重构时容易因文件布局变化而失效；需要增加链接真实生产模块的测试。
7. 当前构建脚本按目录顶层枚举源码（Windows PowerShell 的 `Get-ChildItem -Filter *.c`，macOS 的 `find -maxdepth 1`）。在引入嵌套模块目录前，必须先让本地构建和 CI 对新目录可见，并检查源文件是否重复编译。
8. 现有 `docs/FILE_CLIPBOARD_TRANSFER_REFACTOR.md` 记录了此前多阶段传输功能工作和待验收事项；本轮不能把历史阶段误当成当前架构已经模块化，也不能重复实现已存在的预算/任务/限速功能。

### 2.4 需在阶段 0 完成的深度审计

- 建立完整文件清单、行数、函数/类型清单。
- 为每个模块绘制调用关系和数据流。
- 搜索所有跨文件静态依赖、全局可变状态、线程入口、锁、条件变量、socket、句柄和动态分配。
- 对每个资源标注创建者、所有者、转移点、释放者及终态。
- 比较 Windows/macOS 对应函数的语义差异，不只比较文件名。
- 读取并核对当前 CI workflow、构建脚本和所有测试入口。
- 建立基线构建/测试记录；CI 未实际执行的项目标记 BLOCKED/NOT RUN。

## 3. 目标分层与依赖规则

目标逻辑分层：

1. **UI / Presentation**：Win32 与 Cocoa 页面、控件、用户交互；只消费快照、发出命令。
2. **Application**：连接协调、剪贴板协调、传输协调、输入协调；负责用例编排和生命周期。
3. **Domain / Core**：协议编解码、Task 状态机、预算账本、ACK/checkpoint、限速策略、会话标识、验证规则；纯 C、无平台头文件。
4. **Platform Infrastructure**：socket、线程/锁、文件系统、剪贴板、图片转换、输入捕获/注入、系统权限；按 Windows/macOS 实现。

依赖方向只能是 UI → Application → Core；Application/Core 通过显式接口调用 Platform Adapter。Core 不得包含或暴露 HWND、HANDLE、SOCKET、NSWindow、NSPasteboard、NSURL、CGImageRef 等平台类型。平台适配层不得复制一份 Task/Budget 状态机。

### 3.1 目标目录（渐进迁移，不一次性搬空）

```text
src/
  core/
    protocol/       # 帧、消息 codec、协议验证
    session/        # session id、通用连接状态规则
    transfer/       # task、lifecycle、budget、ACK、rate、tracking、history
    clipboard/      # Offer/Request 的平台无关数据模型与 codec
    common/         # clock、config、log、crc、watchdog
  application/
    connection/     # 控制/数据连接协调
    transfer/       # service、sender/receiver orchestration
    clipboard/      # clipboard coordinator / paste transaction
    input/          # 输入路由与按下/释放生命周期协调
  platform/
    windows/
      network/ filesystem/ threading/ clipboard/ input/ display/
    macos/
      network/ filesystem/ threading/ clipboard/ input/ display/
  ui/
    windows/
    macos/
tests/
  unit/ module/ integration/ fixtures/
```

迁移期间允许保留现有 `src/core`、`src/win`、`src/mac` 顶层文件；每次只移动一个有明确边界的模块。目录目标不构成一次性批量移动授权。

## 4. 模块责任、接口与所有权

### 4.1 Transfer Domain

| 模块 | 唯一职责 | 不应承担 |
|---|---|---|
| Task / Lifecycle | Task ID、状态转换、取消/完成幂等性 | socket、文件句柄、UI |
| Budget / Reservation | jobs/entries/bytes/image/RX pool 资源预留和释放 | 自己结束 Task |
| ACK Window | 在途文件窗口、ACK、超时、已确认 checkpoint | 发送 socket、文件读盘 |
| Rate Controller | Token Bucket、RTT/队列反馈策略 | sleep、socket、UI |
| Tracking / History | 不可变快照、统计和持久历史 | 传输控制 |
| Sender | 文件枚举、读取、组帧、发送和重试编排 | 接收端写盘 |
| Receiver | 帧校验、批次状态、接收生命周期 | 实际磁盘写入 |
| Writer | 有界 slot + bytes 队列、缓冲池、文件写入 | 任务终态决策、协议 ACK 决策 |
| Transfer Service | 对外 API、任务协调、初始化/关闭 | 平台文件系统细节 |

### 4.2 Clipboard Domain / Platform

- Core 保存 Offer ID、Request ID、generation、origin、状态和 codec；不保存平台对象。
- Platform Clipboard Adapter 负责获取系统快照、写入文件/文本/图片、读取 sequence/changeCount。
- Clipboard Coordinator 负责 Offer 生命周期、Paste Transaction、版本校验、异步重试和完成/取消。
- 图片读取/解码/格式转换不得在 UI 主线程执行；所有临时内存必须先预留预算，释放或所有权转移必须可追踪。
- 传输完成写入剪贴板前必须校验 Request ID、session 和本机 clipboard generation，旧任务不得覆盖用户新复制的内容。

### 4.3 Connection / Input / UI

- Connection Service 负责控制/数据 socket 的启动、认证、会话绑定、重连和关闭；协议 codec 留在 Core，系统 socket 留在 Platform。
- Input Domain 负责按键/鼠标事件语义、修饰键映射、按下/释放配对与 release-all 规则；系统 hook/event injection 留在 Platform。
- UI 只消费 thread-safe immutable snapshot，通过 Application command 取消/重试/切角色/保存配置；不得直接修改 Core 内部状态。
- 配置读取、校验、迁移和持久化与控件布局分离。

### 4.4 资源与线程所有权规范

每个异步对象必须回答：
- 谁创建？谁拥有？所有权何时转移？
- 谁可以取消？取消是请求还是同步完成？
- 哪个线程访问？是否需要锁？锁保护哪些字段？
- 退出/断线/失败时谁负责 drain、close、release？
- 回调是否可能迟到？如何通过 generation/session/task id 拒绝过期结果？

硬性规则：
- Reservation Handle 只由一个明确 Task 所有；句柄移动转移所有权，复制句柄不能重复释放。
- Writer slot 由 writer queue/worker 管理；任务终态必须等待该任务排队写入完成或显式取消/排空后再释放相关资源。
- socket 和文件句柄必须有唯一关闭责任方；跨线程交接必须有状态和同步。
- 所有队列同时有 slot 和 bytes 上限；队列满时采用明确阻塞/拒绝策略，不得隐式无限增长。
- ACK 确认后才能推进正式 checkpoint；发送窗口局部游标不得覆盖未确认断点。
- callback/UI 更新使用 Task ID + generation/session 防止迟到事件污染新状态。

## 5. 分阶段实施路线与验收

### 阶段 0：基线审计与安全网

**目标**：获得可重复的构建、测试、模块依赖和所有权基线。

任务：
- [x] 读取 `AGENTS.md`、架构文档和既有传输重构计划。
- [x] 确认主干、开放 PR 和活动分支；创建唯一活动重构分支。
- [x] 初步统计超大文件和构建脚本的源码枚举限制。
- [ ] 完整模块/函数/全局状态/线程/资源所有权清单。
- [ ] Windows/macOS 当前主干 CI 与本地脚本基线。
- [ ] 将 Feature Test Matrix 映射到模块和重构阶段。
- [ ] 建立模块依赖图和平台实现差异表。

验收：
- 审计报告覆盖所有生产源文件和测试入口。
- 当前主干测试状态有真实 Actions 证据；无证据的测试明确标记未运行。
- 不修改生产行为。

### 阶段 1：构建系统与模块边界基础

**目标**：允许安全引入真正独立的模块文件。

任务：
- [x] Windows/macOS app 构建源文件发现支持嵌套目录；重复编译与显式模块清单仍需继续验证。
- [ ] 本地构建与 GitHub Actions 使用同一套明确源文件发现规则。
- [ ] 定义模块头文件规则：公开 API、内部 API、平台适配 API 分开。
- [ ] 增加 include 边界检查，确保 Core 不依赖平台头文件。
- [x] ACK Window 已从 header-only static inline 迁移为真实生产 `.c` 模块，并纳入 Windows/macOS Core 测试链接；其余源码提取测试仍待逐步替换。

验收：
- 两端 clean build 成功。
- 核心/协议/回归测试通过。
- 不改变二进制协议和用户行为。
- CI 中明确显示实际编译源文件，不存在遗漏/重复链接。

### 阶段 2：传输接收端与写盘模块化（P0）

**目标**：首先解除 `xfer_receive.inc` 对发送端翻译单元静态状态的隐式依赖。

当前进展：Windows/macOS writer queue 的 buffer pool 已抽取为共享 Core 模块；文件句柄和 open/write-at/close 文件操作已分别抽取到 `src/win/xfer_writer_io.c`、`src/mac/xfer_writer_io.c`。writer thread、队列同步、watchdog/status/task callback 仍留在 include 中，不能把阶段 2 标记为完成。

顺序：
1. 绘制 receiver include 的静态依赖表；明确接收状态、writer 状态和外部回调。
2. 提取 Writer Queue 契约：start / enqueue-open / enqueue-write / enqueue-close / drain / stop / snapshot。
3. Writer 以平台文件 IO adapter 执行写入；不自行做 Task 终态或 ACK 决策。
4. 将 Windows writer queue 提取为独立生产编译单元并链接测试。
5. 对照同一契约提取 macOS writer queue；保持两端 slot/bytes/pool 语义一致。
6. 将 Receiver Protocol / Batch Lifecycle 从 include 拆成独立模块；通过明确 callback/interface 获取 status、abort、clipboard dispatch 等能力。
7. 删除 `.inc`，不允许以新的 include 文件替代旧隐式依赖。

验收：
- 两端 writer 模块可独立编译/单测。
- 慢盘、磁盘满、权限拒绝、取消、断线、关闭 drain、队列满、slot/bytes 双重限制测试通过。
- 每个 buffer/slot/reservation 恰好释放一次；Task 终态与 writer 队列无竞态。
- 文件 CRC、长度、偏移、路径安全、ACK 语义不变。
- 两端原生 CI 全绿；真实双机测试未执行则单独标注。

### 阶段 3：发送端与传输协调器模块化（P0）

**目标**：从平台 xfer 文件中分离扫描、sender、ACK、rate、task orchestration 和 status。

顺序：
1. 定义 Transfer Service 对外 API，暂时兼容现有 `xfer.h`。
2. 抽离平台文件枚举/快照适配；异步扫描只在有效 Paste Request 后执行。
3. 将 ACK Window/checkpoint 状态机与 socket send 分离，ACK 是唯一正式断点推进依据。
4. 将 Rate Controller 纯策略与平台等待/时钟适配分离。
5. Sender 只负责按已确认 checkpoint 读取、发送、等待确认；断线从第一个未确认文件恢复。
6. Task/Reservation 生命周期由唯一 Task Owner 管理；Sender/Receiver/Writer 不各自创建平行生命周期。
7. 将 Status/History 查询变为快照接口，UI 不读内部可变状态。

验收：
- ACK window 局部游标、未确认文件恢复、已确认文件不重发测试通过。
- 并发 submit/cancel/retry、重连、迟到 ACK、旧 session、队列 drain 测试通过。
- 资源账本与 Task 集合可对账，终态释放恰好一次。
- 两端行为测试同一组测试向量。

### 阶段 4：跨平台 Transfer Domain 收敛（P0）

**目标**：统一规则，不强行统一系统调用。

任务：
- 将任务状态转换、错误码、重试策略、checkpoint 规则、资源 admission 规则整理为共享 C Core。
- 平台 sender/receiver 只提供文件、socket、线程和时钟适配。
- Windows/macOS 对应规则由同一组 Core 测试覆盖。
- 清理重复且已由共享规则覆盖的代码；保留平台差异注释和 adapter。

验收：
- Core 可脱离 Win32/Cocoa 头文件编译。
- 两端对相同输入事件产生相同状态和错误语义。
- 不降低任何既有校验、预算或背压上限。

### 阶段 5：剪贴板与图片流水线模块化（P1）

**目标**：分离平台 Pasteboard API、Offer/Request 协议、图片工作队列和事务协调。

顺序：
1. Clipboard Protocol / Offer Registry / Paste Transaction 规则纯 C 化。
2. Windows Clipboard Adapter 与 macOS Pasteboard Adapter 各自封装系统 API。
3. 图片 snapshot、decode/convert、owned buffer 和 transfer handoff 分层。
4. 预算不足时保留可重试状态；预算释放后重试，不能把变化标记成已处理而漏同步。
5. 远端文件完成时通过 Request ID + clipboard sequence/changeCount 做 compare-and-apply。
6. 删除剪贴板文件中直接管理传输队列内部状态的耦合。

验收：
- 新复制内容不会被旧任务覆盖。
- 首次拦截的粘贴可完成或明确失败，不静默丢失。
- 图片预算暂时不足后能重试。
- 图片转换不占 UI 主线程；临时内存全程入账。
- 本机原生复制粘贴、跨设备延迟粘贴和双向同步行为保持既有产品决策。

### 阶段 6：连接/会话协调模块化（P1）

**目标**：统一 session、认证、控制/数据通道配对、重连和 shutdown 规则。

验收：
- Windows Server / macOS Client 与 macOS Server / Windows Client 对称测试。
- 控制/数据通道重连、认证失败、session rollover、旧 socket 迟到事件测试通过。
- 断开时键鼠安全收回，任务状态按既有规则处理。

### 阶段 7：输入系统模块化（P1/P2）

**目标**：输入语义与系统 hook/event injection 分离。

验收：
- 左/中/右键、修饰键、重复键、按下/释放配对、release-all、鼠标边缘移交测试通过。
- macOS 辅助功能权限缺失/撤销提示正常。
- 不改变现有按键映射和用户配置。

### 阶段 8：UI、配置、日志、更新模块化（P2）

**目标**：业务状态与原生控件分离，配置/更新逻辑不再嵌入页面/状态栏。

验收：
- Windows/macOS 六个页面、任务列表、历史、日志、设置、更新入口行为一致。
- UI 只读快照并发出命令；后台线程不直接操作 UI 控件。
- DPI、窗口关闭、应用退出和任务刷新无竞态。

### 阶段 9：工程收尾与性能/稳定性验收

任务：
- 按职责拆分超大测试文件和脚本；保留必要的端到端/源码防回归测试。
- 清除旧 include、重复状态、死代码和无用兼容层。
- 更新 `docs/architecture.md`、`tests/FEATURE_TEST_MATRIX.md` 和模块 README。
- 执行 1GB 文件、10,000 小文件、4K/8K 图片、双向并发、弱网/断线、慢盘、8 小时空闲和长时间传输压测。
- 记录 CPU、RSS、高水位、队列积压、拒绝/释放次数、RTT、吞吐、控制输入 p95/p99。

验收：
- Windows/macOS 原生 CI 必需检查全部通过。
- 核心模块独立测试通过。
- 真实硬件项目标注 PASS / FAIL / BLOCKED，不用编译结果替代实机验证。
- 结构变化有架构图、接口说明、所有权说明和回滚记录。

## 6. 文件拆分优先级清单

| 顺序 | 当前文件 | 目标职责模块 | 阶段 |
|---:|---|---|---|
| 1 | `src/win/xfer_receive.inc` | writer queue + receiver protocol/lifecycle 独立模块 | 2 |
| 2 | `src/mac/xfer_receive.inc` | 与 Windows 同契约的 macOS writer/receiver 模块 | 2 |
| 3 | `src/win/xfer.c` | transfer service / sender / scanner / ACK / rate / status adapter | 3 |
| 4 | `src/mac/xfer.c` | 与 Windows 同业务契约的 macOS adapters | 3 |
| 5 | `src/win/clipboard.c` | clipboard protocol / coordinator / image worker / Win32 adapter | 5 |
| 6 | `src/mac/clipboard.m` | clipboard protocol / coordinator / image worker / NSPasteboard adapter | 5 |
| 7 | `src/win/net_server.c`、`src/mac/net_client.c` | connection/session coordinator + socket adapters | 6 |
| 8 | `src/win/inject.c`、`src/mac/inject.c`、capture files | input domain + platform adapters | 7 |
| 9 | `src/win/main_ui.c`、`src/mac/main_ui.m`、`src/mac/statusbar.m` | UI pages / presenters / config / update views | 8 |
| 10 | `src/core/hop.h`、测试大文件与脚本 | 按 protocol/config/session/status 领域拆头文件和测试 | 1/9 |

文件行数建议：
- 常规模块优先控制在 200–500 行；超过 700 行必须说明为何仍是单一职责。
- 小型数据结构/codec 可低于 200 行，不为凑行数拆分。
- 行数不是验收指标；依赖、状态所有权、可测试性和职责清晰度才是验收指标。

## 7. 重构不可破坏的系统不变量

1. 控制通道与数据通道隔离，传输不阻塞键鼠输入。
2. 传输帧身份绑定 token/session/batch；认证前不接受数据帧。
3. 文件路径校验、符号链接/reparse point 防护、chunk/file CRC 和声明长度验证不得削弱。
4. 发送 checkpoint 只能由成功 ACK 推进；未确认文件必须重发，已确认文件不得无故重发。
5. 任务身份唯一；取消/重试按 Task ID 隔离，不误伤另一方向或其他任务。
6. Reservation Handle 只有一个 owner；资源账本是唯一事实来源，释放 exactly-once。
7. 任何队列同时受 slot 和 bytes 限制；缓冲池容量有上限，拒绝/释放/高水位可观测。
8. 图片分配前预算预留；owned buffer 转移不重复计费、不重复复制、不重复释放。
9. Clipboard apply 使用 Request ID + 本机 clipboard generation/sequence compare-and-apply；旧任务不得覆盖新复制内容。
10. macOS 图片预算暂时不足必须保留待重试状态，不能吞掉 clipboard change。
11. 鼠标/键盘 down/up 成对；连接断开或角色切换时执行安全 release-all。
12. 所有异步回调校验 Task/Request/Session generation，拒绝迟到的旧事件。
13. Windows 与 macOS 对相同协议输入、任务状态输入遵循相同结果。
14. 不允许仅通过源码提取测试证明真实模块链接和跨线程生命周期正确。

## 8. 测试与 CI 门槛

每个重构切片按影响范围执行：

- Core：core、protocol、task、budget、ACK、rate、tracking、history tests。
- Transfer：sender/receiver/writer 模块测试、网络集成、取消/重连/CRC/路径安全。
- Clipboard：Offer/Request codec、clipboard generation race、图片预算与 retry、双向实际粘贴。
- Connection：双角色认证、session rollover、socket close/reconnect。
- Input：事件顺序、修饰键、release-all、权限和真实设备。
- UI：状态快照并发、配置保存/迁移、六页回归。
- 所有影响平台源文件的改动必须在 Windows 和 macOS 原生构建中验证；无法运行的 runner 明确 BLOCKED。
- 测试结果必须记录命令、OS、提交 SHA、Actions URL 和实际状态。

重构提交不应混入功能改动。若发现真实缺陷，先添加针对性测试，再在同一模块切片中修复，并在提交说明中区分“行为修复”和“结构搬移”。

## 9. Git / PR 工作流

- 严格遵守 `AGENTS.md`：只有一个活动分支和一个开发 PR。
- 本轮使用 `refactor/architecture-modularization`，PR 目标为 `main`。
- 不直接提交到 `main`，不擅自合并 PR；用户明确授权合并后才合并。
- 每个阶段或独立切片形成可审查提交；同一阶段可多个提交。
- 不重写共享历史，不 force-push，不删除用户分支。
- 任何 CI 失败必须在当前活动分支修复，不另开分支。
- 每次结束报告当前分支、PR、基线 SHA、提交 SHA、变更文件、Windows/macOS CI、实机状态和剩余风险。

## 10. 实施记录

| 阶段/切片 | 状态 | 提交 SHA | 变更与证据 |
|---|---|---|---|
| 基线核对 | 完成 | `68818b3f093af20385c3fba5785405eb4ac8d634` | 已核实 main HEAD、PR/分支状态；完成主要模块与函数簇审计附件。动态调用图、全量资源所有权追踪仍作为阶段 0 深度审计待办。 |
| 阶段 0：深度审计 | 进行中 | `eaf8488b21c87201297aaf9edcdad2660bab76f2` | 已建立模块/函数簇和平台差异清单；仍需逐项追踪所有资源 owner、锁顺序、线程退出与回调迟到路径。PR 基线双平台 CI 已通过。 |
| 阶段 1：构建与模块边界 | 进行中 | `6a994e691c844c60b3b15615f299633531283c13` | app 构建递归发现已完成；ACK Window、Transfer Tracking、RX Buffer Pool 已成为独立生产编译单元并接入两端测试。Core 平台 include 检查、显式 source manifest/重复源检查仍待完成。 |
| 阶段 2：接收端/Writer | 进行中 | `54496ab83eb5f3caa61a5b3f5a6b7379a461c56e` | 在既有共享 RX Queue accounting 与平台 Writer IO adapter 之上，将队列容量/预留/忙碌写入/排空测试拆为独立 `tests/xfer_rx_queue_tests.c` 目标，并接入 macOS/Windows 测试脚本。Writer thread、队列同步和 receiver lifecycle 仍未拆完；本切片新增 CI 尚待运行。 |
| 阶段 3：Sender/Service | 待完成 | — | 待实施。 |
| 阶段 4：共享 Transfer Domain | 待完成 | — | 待实施。 |
| 阶段 5：Clipboard | 待完成 | — | 待实施。 |
| 阶段 6：Connection | 待完成 | — | 待实施。 |
| 阶段 7：Input | 待完成 | — | 待实施。 |
| 阶段 8：UI | 待完成 | — | 待实施。 |
| 阶段 9：工程收尾 | 待完成 | — | 待实施。 |

## 11. 当前执行切片

已完成三个 Core 领域模块拆分：ACK Window、Transfer Tracking、RX Buffer Pool。另将 RX Queue accounting 的边界与生命周期用例独立为单独测试目标，避免所有队列回归都依赖大型 `core_tests.c`。最新又将 Windows/macOS 接收端重复的队列容量与 slot/byte accounting 抽取为 `xfer_rx_queue.c/.h`；平台层保留 ring storage、锁/条件变量和线程调度，Core 统一负责 slot/bytes admission、reservation/rollback/commit、dequeue/complete 和 drained 判断。新增 Core 单元测试覆盖满队列、busy writer、预留回滚、提交和排空。最新提交的 Windows/macOS CI 正在执行，尚未以通过状态结案。

下一项执行：已新增 `docs/WRITER_QUEUE_CONTRACT.md`，明确 Writer Queue、Receiver、Task/Budget、Platform Adapter 的责任边界、命令所有权、状态机、FAILED_JOIN 语义和双平台测试门槛。接下来按该契约先提取 Windows writer worker，再按相同接口提取 macOS；平台文件 IO 已先提取。与此同时补齐 Core include 边界检查与构建源清单重复检查。

此顺序是为了先确保新模块会被本地构建和 CI 正确编译，再拆解最有隐式依赖、最容易引入生命周期回归的接收端代码。


### 2026-11-08：RX Queue 测试目标独立化

- 新增 `tests/xfer_rx_queue_tests.c`，直接链接生产 `src/core/xfer_rx_queue.c`，覆盖 slot/byte 上限边界、零 slot 队列、reservation commit/rollback、busy writer 与 queued slot 同时计费、drain 归零。
- macOS `scripts/test-core.sh` 使用 Clang + AddressSanitizer + UndefinedBehaviorSanitizer 编译并运行独立目标。
- Windows `scripts/test-core.ps1` 使用 MSVC（可用时）或 GCC 编译并运行同一测试源。
- 该切片只拆分测试组织，不改变队列生产行为；此前 `core_tests.c` 中的原有 RX Queue 测试暂时保留，作为交叉回归覆盖。
- 提交：`8edde5c`（初始测试文件）、`abf6d7c`（补齐独立测试）、`7806336`（诊断输出修正）、`192b7a1`（macOS 脚本接入）、`54496ab`（Windows 脚本接入）。
- 当前状态：代码已推送到活动分支；本切片双平台 CI 结果待 Actions 实际执行。不得据此声称 Writer Worker 已独立化。


### 2026-11-08：Writer Queue 所有权与生命周期契约

- 新增 `docs/WRITER_QUEUE_CONTRACT.md`。
- 明确 OPEN/WRITE/CLOSE 的 slot/bytes 计费、payload/Task reservation/pool slot 所有权转移和 exactly-once 释放。
- 明确 UNINITIALIZED/RUNNING/STOPPING/STOPPED/FAILED_JOIN 状态，以及线程 join 超时后不得清空句柄、释放同步对象或重新初始化共享状态的约束。
- 明确 Writer 不拥有 Receiver Task 终态，不直接控制 ACK；Receiver 必须在 CRC/删除/复用路径前 drain + CLOSE + drain。
- 当前仍是接口与验收契约，不代表 Writer Worker 已拆成独立生产编译单元；后续按 Windows -> macOS 顺序实施。
- 提交：`6dc51da68276eaa2ea1230e0f94882ab88ff19f7`。


### 2026-11-08：Writer Worker 线程适配层独立化（第一切片）

- 新增 `src/mac/xfer_writer_worker.[ch]` 与 `src/win/xfer_writer_worker.[ch]`，将 native thread 创建、线程优先级/QoS、回调循环、join 和 worker task storage 生命周期从 Receiver include 中抽出。
- Writer Worker 通过 `take / run / complete / on_exit` 回调与接收队列交互；线程模块不再直接访问 `s_wq`、Task Manager、buffer pool 或文件 IO。
- 两个平台仍由 Receiver 侧回调负责队列锁、dequeue/accounting、Task RX reservation release、buffer pool release 和写入耗时统计；这避免将业务依赖反向塞进 worker 模块。
- Windows join 超时不关闭/丢弃线程 HANDLE，也不释放 task storage；Receiver 保留 worker 和 queue 状态，禁止重新初始化仍可能被线程访问的状态。
- macOS 使用原生 `pthread_t`；成功 join 后才释放 task storage。当前 macOS join 仍是阻塞式，与此前行为一致。
- 新增 `tests/writer_worker_tests.c`，覆盖 take/run/complete/on_exit 的调用顺序、成功 join 和 join 后重启；两端 core test 脚本均接入该独立目标。
- 当前切片尚未取得本次新提交的 CI 结果。下一步应补齐 start failure、join timeout 注入、停止期间 producer 唤醒和队列 drain 竞态测试，再继续把 queue command/ownership 从 include 中抽出。
- 提交序列：`ccaef1d`、`1e7649c`、`8d34d94`、`eec2622`、`40c4b7d`、`80d3e2f`、`f6ce65c`、`be4dfd2`、`fa9c1f4`、`baa94ec`、`39d5168`、`ec72cb1`、`e947258`、`359b68f`、`5963e0d`、`05b11b9`、`79204e7`、`1608649`、`18178f9`。


### 2026-11-08：Writer Queue 上下文与初始化失败回滚

- 将平台 Writer Queue 的内部匿名结构命名为 `hop_rx_writer_queue`，Worker 回调全部通过显式 `context` 访问队列对象，不再在回调内隐式捕获全局 `s_wq`。
- 写盘命令执行函数改为接收队列实例；线程回调中的锁、条件变量、ring、accounting、buffer pool 和 IO 均从 context 取得，为后续移入独立 `xfer_writer_queue.c` 做准备。
- macOS 的 mutex / condition 初始化逐项检查，任一失败均按逆序销毁已初始化同步对象并释放 buffer pool；Worker 创建失败同样完整回滚。
- Windows Worker 创建失败时同步释放已初始化的临界区和 buffer pool。
- 此切片仍保留 Receiver 兼容入口 `wq_push / wq_push_data / wq_drain / wq_start / wq_stop`，未改变协议和任务状态机。
- 尚需通过最新 Windows/macOS 原生 CI 与压力测试；在此之前不把本切片视为验收完成。


### 2026-11-08：Windows HA 链接依赖修复与双平台 CI 通过

- 新增 Worker 模块后，Windows 应用本身可以成功构建，但 HA 集成测试仍直接编译 `xfer.c`，未链接独立的 `src/win/xfer_writer_worker.c`，导致 `hop_writer_worker_start/join` 的 LNK2019。
- 已在 `scripts/test-ha.ps1` 将 Worker 生产编译单元加入 HA 测试目标；没有绕过或删除该集成测试。
- 修复提交：`4374782a359ec6ed24a54fa1b0f903afe06a4c3d`。
- GitHub Actions run `42471596814`（run #580）：Windows MSVC build、Core/regression、transfer protocol、TCP loopback、native control/data role + reconnect、updater syntax、package 全部成功；macOS build、Core/regression、transfer protocol、TCP loopback、control/listener loopback、bundle verification、package 全部成功。
- 该结果证明本次 Writer Worker 与 HA 测试链接修复通过 CI；不代表 Writer Queue 已独立模块化，也不替代真实双机/慢盘/长时压力验收。


### 2026-11-08：macOS Writer Worker bounded-exit parity

- macOS Worker adapter now waits for an explicit worker-exit signal with a 3-second deadline before calling `pthread_join`; the worker thread closes its IO before publishing exit.
- A timed-out wait preserves the live worker, task storage, callback context, mutex, and condition variable. A later join can retry; only successful join releases these resources.
- Shared `tests/writer_worker_tests.c` now exercises a callback lasting longer than the join deadline, verifies retained state after timeout, retries join, and verifies restart.
- Added EINTR-safe delay handling in the POSIX timeout test.
- Updated `docs/WRITER_QUEUE_CONTRACT.md` with the platform join contract.
- Latest validation: GitHub Actions run #585, run ID `42476724930`, commit `a46f1584f49b29ffa47dc6766d2d725ae756a4d0`: Windows build/Core/regression/protocol/network/HA/updater/package PASS; macOS build/Core/regression/protocol/network/control-listener/bundle/package PASS. Release publication skipped as expected for a PR.
- This closes Worker join parity only. It does not complete Writer Queue extraction; queue ring, producer reservation/copy/commit, accounting synchronization, and Receiver wrappers remain in `xfer_receive.inc`.

Next execution remains the phase-2 P0 slice: extract the Windows Writer Queue behind an opaque API with explicit Task budget and write-time callbacks, link it as a real production compilation unit, and test it before applying the same semantic contract to macOS. Do not remove Receiver compatibility wrappers until both platform modules pass the same lifecycle and ownership tests.


### 2026-11-08：Windows Writer Queue 独立生产模块（第一平台切片）

- 新增 `src/win/xfer_writer_queue.[ch]`：Writer Queue 现在是单独编译的生产模块，使用 opaque queue handle；Receiver 不再直接持有/操作 ring、queue accounting、buffer pool、锁/条件变量或 worker thread。
- Queue 对外提供 create、enqueue OPEN/WRITE/CLOSE、drain、error reset/read、stop、destroy。Receiver 保留薄兼容包装，负责把 Task reservation、watchdog、时钟/写盘耗时状态、日志通过显式 callbacks 注入。
- WRITE 仍按 queue slot+bytes reserve -> Task RX budget reserve -> buffer-pool acquire -> payload copy -> commit 顺序执行；任何失败路径回滚 queue accounting、Task budget 和 pool slot。Worker completion 恰好释放 Task budget、pool slot 和 queue accounting。
- STOP 先 drain 已接受命令，再拒绝新任务并唤醒线程；join 超时保留 opaque queue 及其全部同步/线程资源，只有成功 join 才允许 destroy。
- 新增 `tests/writer_queue_tests.c`，直接链接生产 Windows Queue、Worker、IO、Core RX Queue 和 Buffer Pool，覆盖 OPEN/WRITE/CLOSE 顺序、Task budget admission rejection/rollback、完成释放、drain/stop 和落盘字节核验。
- Windows HA 测试目标已链接新生产编译单元；Windows Core 脚本增加独立 Queue 测试目标。应用构建递归发现新 `.c` 文件。
- Receiver 源码结构回归测试已调整：Windows 检查 Queue 生产模块拥有 pool/accounting，Receiver include 不再包含 worker/pool 实现；macOS 在 parity 完成前仍检查旧平台适配。
- 本切片还未验收：GitHub Actions Run #594（commit `cc32cd30a050520d8bb10c19ef61c94dca3e0d7b`）正在执行；前一轮因旧源码结构断言失败，已修正断言并触发新一轮。不得将编译/测试状态提前标记通过。
- 当前只完成 Windows 平台提取；macOS Writer Queue 仍留在 `src/mac/xfer_receive.inc`，不能标记阶段 2 完成。


#### Windows Queue extraction validation follow-up

- The first two extraction CI attempts correctly exposed stale regression-fixture assumptions: the fixture expected the Receiver include to export its old global error accessor. The Receiver now keeps a narrow `wq_error_get()` compatibility wrapper; production queue internals remain outside the fixture.
- The next attempt exposed a missing test-link dependency: `xfer_buffer_pool.c` updates the global RX-pool budget through `xfer_budget.c`. The independent Windows writer-queue test target now links that production Core module as well.
- Run #596 for commit `66fb932315208ade62860b9234d578fe0c351cc6` has been queued. No pass is claimed until its Windows and macOS jobs finish.


### 2026-11-08：Windows/macOS Writer Queue 独立生产模块完成（双平台第一切片）

- 新增平台独立的 opaque Writer Queue 生产模块：
  - Windows：`src/win/xfer_writer_queue.[ch]`
  - macOS：`src/mac/xfer_writer_queue.[ch]`
- 两端 Receiver include 现在只保留 OPEN/CLOSE 轻量命令适配、WRITE API 转发和 Task/Watchdog/metrics/log callback 注入；ring、slot/bytes accounting、buffer pool、worker、文件 IO 和 stop/join 生命周期均由 Queue 模块持有。
- 两端公开 API 语义一致：create、enqueue_open、enqueue_write、enqueue_close、drain、error_reset/error、stop、destroy。平台路径类型和原生锁/条件变量仍由各自 adapter 处理。
- 两端 WRITE admission/ownership 顺序保持一致：Queue slot+bytes reservation -> Task RX budget reservation -> buffer pool acquire -> payload copy -> commit。失败时 rollback queue accounting、Task budget、pool slot；worker completion exactly-once release Task budget、pool slot 和 queue accounting。
- Stop 先 drain 已接受命令，再拒绝新命令并唤醒 worker/producers；join 超时保留整个 Queue 对象，成功 join 后才 dispose pool、销毁同步对象并允许 destroy。
- 新增生产模块直连测试：
  - Windows：`tests/writer_queue_tests.c`，由 `scripts/test-core.ps1` 编译执行。
  - macOS：`tests/mac_writer_queue_tests.c`，由 `scripts/test-core.sh` 使用 AddressSanitizer/UndefinedBehaviorSanitizer 编译执行。
  - 覆盖 FIFO OPEN/WRITE/CLOSE、磁盘实际字节、Task budget admission reject/rollback、completion release、drain/stop。
- `scripts/test-ha.ps1` 已链接 Windows Queue 生产编译单元；应用构建递归源发现自动包含两端新模块。回归脚本现在检查两端 Receiver 不再拥有 Queue 内部状态。
- CI 证据：GitHub Actions Run #609，run ID `42478897342`，head `33ec53b9d49c4d2dcb3c0bbe52119c9f3a34e574`：Windows MSVC build、Core/regression（含独立 Writer Queue 测试）、transfer protocol、TCP loopback、native control/data reconnect、updater syntax、package 全部 PASS；macOS build、Core/regression（含 ASan/UBSan Writer Queue 测试）、transfer protocol、TCP loopback、control/listener loopback、bundle verification、package 全部 PASS；Release publication skipped（PR 预期）。
- 中途 CI 揭示并修复了测试集成问题：源码布局断言、fixture error accessor、Writer Queue 测试缺失 RX budget 链接，以及 POSIX feature macro 隐藏 Apple QoS API。最新 Run #609 全部通过。
- 阶段 2 尚未完成：Receiver Protocol / Batch Lifecycle 仍在 `xfer_receive.inc`，后续要从 include 中提取独立模块；还需增加更完整的 stop-vs-producer race、buffer allocation failure、slow-disk/error persistence 和 sanitizer stress tests。不得将整个阶段 2 标记完成。


### 2026-11-08：Receiver DONE 幂等回执提取为 Core 模块（下一阶段切片）

- 新增 `src/core/xfer_rx_receipt.[ch]`，将单批次 DONE 回执的 session 绑定、批次查找、payload 完全匹配和状态记录从平台 Receiver include 中提取为纯 C Core 模块。
- Windows/macOS 的 `rx_begin_batch` 与 `HOP_MSG_XFER_DONE` 处理现在通过同一组 Core API 判断已完成批次、拒绝同批次但不同计数/字节的冲突重放，并对完全相同的重放复用原 DONE_ACK 状态，不重复触发剪贴板副作用。
- 新增 `tests/xfer_rx_receipt_tests.c`，覆盖 session 隔离、batch 隔离、完全相同重放、payload 冲突和 receipt 清除。
- 更新 ABORT 与 session-transport 源码夹具，直接链接该生产 Core 模块；Windows HA 目标也链接该模块。
- CI：Run #621（ID `42479841137`，commit `97e7433fd66d36065bb6f3dbc581ca8edc06098e`）当前排队/等待执行；前序 Run #618 暴露的 fixture 链接遗漏已补齐，Run #621 才是包含全部修正的验证依据。未取得最终结果前不标记通过。
- 下一步继续提取 tombstone / batch lifecycle 状态，确保 session rollover、late BEGIN、duplicate DONE 和 cancel/abort 的状态只由一个 Core owner 管理。


### 2026-11-08：Receiver Session / Tombstone 状态提取为 Core 模块

- 新增 `src/core/xfer_rx_lifecycle.[ch]`，集中拥有 Receiver 当前 Session ID、已作废 Batch ID 和对应 ABORT reason。
- Windows/macOS Receiver 不再在 `s_rx` 结构中复制保存 `dead_batch/dead_reason`，也不再维护独立的 `s_rx_session` 全局变量；统一通过 lifecycle API 读取/绑定 session、标记 tombstone 和查询 tombstone reason。
- Session rollover 时由 Core bind 操作自动清除旧 tombstone；同一 session 重连保留 tombstone，防止旧 BEGIN 重建已取消批次目录。
- 新增 `tests/xfer_rx_lifecycle_tests.c`，覆盖 session bind、同 session tombstone 保留、跨 session tombstone 清除和显式 reset。
- ABORT 与 session-transport 源码回归夹具已改为链接真实 Core lifecycle 模块，不再 mock 这部分状态。
- CI 证据：GitHub Actions Run #632，run ID `42480893287`，head `5b6984299a6b2aae617fd8e727fdaf9e091054b5`：Windows MSVC build、Core/regression（含 DONE receipt 与 RX lifecycle 独立测试）、transfer protocol、TCP loopback、native control/data reconnect、updater syntax、package 全部 PASS；macOS build、Core/regression（含 ASan/UBSan）、transfer protocol、TCP loopback、control/listener loopback、bundle verification、package 全部 PASS；Release publication skipped（PR 预期）。
- 这只抽离了 Receiver 的 Session/Tombstone 数据所有权；`rx_begin_batch`、`rx_terminate`、`rx_finish_batch` 的状态转换编排仍在平台 include。下一切片应将可共享的 batch transition decisions 抽成 Core 规则，并让平台层只执行目录/文件/clipboard/ACK side effects。


### 2026-11-08：Receiver Batch 转换决策抽离为共享 Core 规则

- 新增 `src/core/xfer_rx_batch_rules.[ch]`，将两端重复的批次状态决策收敛到纯 Core 函数，不承载文件系统、剪贴板、网络或 Task manager 副作用。
- BEGIN 决策统一优先级：无效 batch -> 忽略已提交 DONE receipt -> 拒绝 tombstone batch（保留原 ABORT reason）-> 同批次 active resume -> 启动新 batch。
- ABORT/终止路径统一通过 `hop_xfer_rx_batch_is_active` 判断事件是否属于当前活动批次，避免旧 batch 误清理新 batch。
- DONE 决策统一区分 stale、cancel-won、complete、partial、image-failed；Task completion 的原子 claim 仍由 Task manager 执行，Core 规则只根据 claim 结果决定后续分支。
- Windows/macOS 的 `rx_begin_batch`、`rx_terminate`、`rx_finish_batch` 已接入同一套决策 API；平台仍负责 Task 状态落地、目录处理、clipboard commit、status/history 和 wire ACK。
- 新增 `tests/xfer_rx_batch_rules_tests.c`，覆盖 BEGIN 优先级、墓碑原因透传、active matching、DONE stale/cancel/complete/partial/image-failed。
- 真实平台回归夹具和 IMAGE DONE fixture 均直接链接生产 Core 模块；Windows HA target、Windows/macOS Core 测试脚本已纳入新模块。
- CI 证据：Run #638（ID `42481821345`，head `d2800e9bd88d853cc20201d23146f224635a4c0a`）Windows 与 macOS 全部通过：应用构建、Core/regression（含新规则测试及真实 Receiver/IMAGE fixture）、Transfer protocol、TCP loopback、Windows native role/reconnect、macOS control/listener loopback、更新脚本检查和打包均 PASS；PR 发布步骤按预期 skipped。
- 中间 Run #637 暴露 IMAGE DONE 独立 fixture 未包含新 Core header/link；已补齐 fixture include 和生产模块链接，Run #638 验证通过。
- 本切片只抽取了共享 Batch transition policy，不代表 Receiver 协议/批次生命周期整体拆完。下一步仍需提取接收协议消息处理和 batch/file state ownership，并补齐 stop-vs-producer、cancel-vs-finish、late frame、disconnect/reconnect 等并发和故障注入测试。


### 2026-11-08：Receiver Entry Reservation 表抽离为 Core owner

- 新增 `src/core/xfer_rx_entry_reservations.[ch]`，集中拥有每批次按 FILE ID 索引的 entry reservation 标记和已登记 size。
- Windows/macOS Receiver 不再分别持有 `reserved_entries[]` 与 `reserved_entry_sizes[]` 两组数组；统一使用 Core reservation table。
- Core API 对 1-based ID 做边界校验，区分 NEW、ALREADY_HELD、SIZE_CONFLICT、BUDGET_REJECTED、INVALID。
- 新 entry 只有在注入的 Task budget reserve callback 成功后才提交到表；相同 ID/size 重发不重复 reserve；相同 ID 不同 size 继续触发原有 TOO_LARGE batch abort。
- batch clear/reset 统一清空表；Task manager 仍然是实际预算 owner，Core 表只记录该批次已登记的 entry identity/size，不复制或替代预算账本。
- 新增 `tests/xfer_rx_entry_reservations_tests.c`，覆盖新建、重复幂等、size 冲突、预算拒绝后重试、ID 边界和 reset。
- Windows/macOS Core tests、真实 ABORT Receiver fixture、Windows HA target 均直接链接生产 Core 模块。
- CI 证据：Run #644（ID `42482474497`，head `f641997d5d70fb46037bcf8e83934ba43c96228b`）Windows/macOS 完整通过：app build、Core/regression（含新 reservation 测试和 Receiver fixture）、transfer protocol、TCP loopback、Windows native reconnect/control、macOS control/listener、updater syntax、package 全部 PASS。
- 早期 Run #643 发现新 header 引用了错误的公共头；已改为 include 定义 `HOP_XFER_FILE_LIMIT` 的 `xfer_status.h`，Run #644 验证修复通过。
- 下一步继续提取当前文件的 offset/received/CRC/failure 状态，之后再处理消息 dispatch 和更完整的 batch/file 生命周期编排。


### 2026-11-08：Receiver 当前文件 offset/CRC 状态抽离为 Core

- 新增 `src/core/xfer_rx_file_state.[ch]`，集中拥有当前 FILE/IMAGE ID、失败 ACK 状态、声明大小、连续接收字节数和文件 CRC fold。
- Windows/macOS Receiver 状态不再分别维护 `cur_id/cur_fail/cur_size/cur_received/fcrc` 五组重复字段；统一嵌入 `hop_xfer_rx_file_state`。
- Core API 统一 reset/begin、size 设置、failure 标记、ID 匹配、连续 offset/length 检查和 chunk commit。
- Chunk 状态只在 Writer Queue enqueue 成功后推进 received 与 CRC；CRC 错误、队列写入失败和 offset gap 仍保持原有失败语义。图片数据仍由平台层按已预留的 image buffer 独立累积。
- 新增 `tests/xfer_rx_file_state_tests.c`，覆盖 ID、连续偏移、overlap/gap、越界/溢出、CRC fold、failure gate 和新文件 reset。
- Core tests、Receiver ABORT fixture、Windows HA target 均直接链接生产模块；独立测试链接 `xfer_validate.c` 以复用真实 `hop_xfer_file_crc_step`。
- CI 证据：Run #652（ID `42483467036`，head `a6f39aa54ed5d6029658eaa77873fdbbcda91865`）Windows/macOS 全套通过：应用构建、Core/regression（含文件状态、entry reservation、batch rules 测试）、Transfer protocol、TCP loopback、Windows native role/reconnect、macOS control/listener、更新脚本检查和打包全部 PASS。
- 过程中发现并修复两项测试集成遗漏：ABORT fixture 仍断言旧 Receiver 字段；独立 file-state 测试未链接定义 CRC fold 的 `xfer_validate.c`。Run #652 验证最终修复通过。
- 下一步应将 FILE_BEGIN/CHUNK/FILE_END 的 ACK 决策提取成共享 Core policy，再逐步把协议 dispatch 从平台 include 拆出；平台文件系统、剪贴板和 writer IO 仍保持 adapter 边界。


### 2026-11-08：FILE_END ACK 决策归并至 Receiver File State Core

- 扩展 `hop_xfer_rx_file_state_ack`，统一 FILE_END 的 ACK precedence：先返回已记录的 entry failure；再检查 Writer Queue 写盘错误；再检查 peer FILE_END status；再检查实际接收字节数；最后比较整文件 CRC；全部通过才返回 ACK_OK。
- Windows/macOS FILE_END handler 均改为调用同一 Core 决策函数；平台层只负责 drain/close、诊断日志、删除失败文件、剪贴板顶层路径登记和发送 FILE_ACK。
- 保留原有优先级：坏路径/块 CRC/入队失败等已记录失败优先于后续错误；Writer IO error 优先于 peer status；字节数不足优先于整文件 CRC；顶层路径登记失败仍覆盖为 ACK_DISK。
- 扩展 `tests/xfer_rx_file_state_tests.c`，覆盖成功、peer IO error、writer error、整文件 CRC 不符、未收满、已记录 BADPATH 对后续错误的优先级。
- CI 证据：Run #658（ID `42483891419`，head `c7f072d8527461d73a7d4c0907b7d39dce330565`）Windows/macOS 完整通过：应用构建、Core/regression、Transfer protocol、TCP loopback、Windows native reconnect/control、macOS control/listener、updater syntax 和 packaging 全部 PASS。
- FILE_BEGIN/CHUNK/FILE_END 的主要数据校验和 ACK 结果现在已由 Core 持有；平台层仍保留消息 dispatch、文件/目录副作用和 clipboard path commit。下一步应拆分 protocol dispatch 与批次资源生命周期，而不是继续把平台 IO 抽进 Core。
