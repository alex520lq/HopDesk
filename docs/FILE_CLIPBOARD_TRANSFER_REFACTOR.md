# HopDesk 文件剪贴板与传输重构计划

> 状态：计划已基于代码审查建立；实施按阶段推进，未标记完成的项目均不得视为已实现。
> 基线：`fix/dual-role-server-client` / `59260df3e627aef3b0b2fa63873f391226680fa4`（PR #8，2026-11-05 审查基线）
> 目标：Windows 与 macOS 双向文件/文件夹剪贴板采用按需传输；传输期间键鼠和桌面操作保持响应。

## 0. 产品行为决策（验收时不可改变）

本轮采用**原生剪贴板、延迟传输（Lazy File Paste）**模式：

1. 用户复制本机文件/文件夹时，保留操作系统原生剪贴板内容和本机路径。
2. 本地 Ctrl+V / Cmd+V 始终走系统原生行为，不创建 HopDesk 传输任务、不消耗 Offer。
3. 同一次复制允许先本机粘贴，再跨设备粘贴；本机粘贴不使跨设备 Offer 失效。
4. 复制阶段只发布轻量 File Offer。不得递归扫描目录、读取文件内容、创建发送任务或记录“发送/完成”历史。
5. 只有目标设备真正执行跨设备粘贴时，才发送 Paste Request；源设备验证 Offer 后才异步枚举目录并创建传输队列。
6. 源设备只在本机保存原始路径。绝对路径不发送到另一台设备；对端只持有随机 Offer ID、来源设备、剪贴板代次和显示所需的轻量条目名称。
7. Offer 在同一剪贴板代次内可重复请求；新复制/新剪贴板内容替换旧 Offer。源文件已删除、权限失效、会话断开或 Offer 过期时，明确失败，不使用旧路径。
8. 文件传输完成后，接收端将已落盘的顶层文件路径写入本机原生文件剪贴板，并完成本次待处理粘贴；若目标剪贴板/会话已变化，不覆盖用户的新内容。
9. 文本、图片继续支持双向同步，但必须与文件 Offer 生命周期隔离，不能因文件路径轮询/重试冲掉其他剪贴板内容。

## 1. 当前实现审查记录（问题 → 影响 → 处理要求）

以下记录来自基线提交的 Windows/macOS 代码审查。函数名作为溯源锚点；后续提交需在本文件的“实施记录”中追加提交 SHA、文件和测试证据。

| ID | 现有问题与代码证据 | 用户影响 / 风险 | 必须采取的改进 |
|---|---|---|---|
| CLIP-01 | Windows `src/win/clipboard.c`：`clip_on_update()` 将 `CF_HDROP` 交给 `queue_image()`，后台 `image_worker()` 再调用 `send_drop()` → `xfer_submit_paths()`。macOS `src/mac/clipboard.m`：`poll_pasteboard()` 对 fileURL 调用 `send_paths()` → `xfer_submit_paths()`。 | Ctrl+C 即提交发送作业；本机复制/粘贴也产生发送、完成记录；无跨端粘贴仍会传输。 | 删除“复制即发送”路径。复制事件只更新本机 Offer 并发送轻量元数据；数据通道只接受有效 Paste Request 产生的作业。 |
| CLIP-02 | macOS `clipboard_init()` 在 `dispatch_get_main_queue()` 创建 250ms timer；`poll_pasteboard()` 同步调用文件/图片发送入口。 | 主队列承担剪贴板读取、图片转换及文件提交；复制大图/大量文件可能卡 UI。 | 主队列只读取 changeCount 和安全快照；图片解码/转换、文件枚举、元数据构建、发送全部放后台队列。 |
| CLIP-03 | macOS `xfer_submit_paths_impl()` → `walk_into()` 同步递归目录；Windows 对应实现也在提交路径中递归遍历。 | 复制大目录时扫描成本提前发生；目录扫描可能阻塞调用线程。 | 仅在 Paste Request 验证后扫描；使用独立低优先级工作线程、取消令牌、深度/条目/字节硬上限。 |
| CLIP-04 | Windows `image_worker()` 通过 `CF_HDROP` 和图片共用入口；文件路径失败后会通过 `WM_APP_CLIP` / timer 重试提交。macOS 提交失败时 `s_last_change` 不更新，轮询会再次尝试。 | 文件失败重试可能重复创建作业/重复扫描；图片和文件生命周期混杂。 | 文件 Offer、图片任务、文本同步分离；Offer 更新幂等，重试只重试元数据发布，不启动数据传输。 |
| CLIP-05 | macOS `apply_objects_retry()` 的首次写入不校验用户剪贴板代次；Windows `apply_global()` 的期望序列号在应用时才取得。 | 旧传输完成后可能覆盖用户刚复制的新内容。 | 每个剪贴板写入携带接收端 changeCount/sequence 快照和 Offer/Request ID；只允许仍匹配的任务提交结果。 |
| XFER-01 | Windows/macOS `send_file_entry()` 中读取循环把 `got <= 0` / `ReadFile` 失败与正常 EOF 混在一起。 | 本地读取错误可能被错误地当成文件结束，导致发送端发送不完整文件。 | 区分 EOF、短读和真实 I/O 错误；真实错误必须发送失败状态，不允许成功结束。 |
| XFER-02 | Windows/macOS 接收端 FILE_CHUNK 检查上界，但 FILE_END 成功判定没有完整、独立地验证实际连续接收字节数等于声明 size。 | 截断/缺块文件存在被误判成功的风险。 | 接收端记录已接收偏移/字节数；校验无缺口、无重叠、最终长度严格等于 FILE_BEGIN.size，CRC 正确后才原子提交。 |
| XFER-03 | macOS `walk_into()` 用 `lstat()` 并跳过符号链接；Windows `walk_into()` 尚未对所有 reparse point / Junction 做等价排除。 | Windows 可能越界遍历链接目录或形成循环。 | Windows 检查 `FILE_ATTRIBUTE_REPARSE_POINT` 并默认拒绝/跳过；两端统一不跟随 symlink/reparse point。 |
| XFER-04 | 发送作业队列按链表增长；`XFER_MAX_ENTRIES` 只约束单次枚举，不是全局队列总容量；超限路径需要显式失败。 | 多次提交可累积内存；大量条目可能部分入队、静默截断。 | 发送队列增加 jobs/entries/bytes 三重上限；超限要原子拒绝整个请求并报告原因，不能部分成功。 |
| XFER-05 | 当前每个文件发送后等待 FILE_ACK；大量小文件会产生逐文件 RTT。 | 目录含大量小文件时吞吐明显受 RTT 限制。 | 先保留可靠性，再引入有界 ACK 窗口/小文件批次；窗口大小固定上限，失败只重试对应文件，必须有集成测试。 |
| XFER-06 | 线程降优先级与每块 `Sleep(0)` / `sched_yield()` 不能限制 CPU、磁盘争用或网络占用；现有性能数据主要是同进程回环测试。 | 真实 Wi-Fi/磁盘负载时可能卡顿，峰值吞吐不能代表交互体验。 | 增加发送速率 Token Bucket、有限内存队列、低优先级 I/O、控制 RTT/键鼠延迟观测；真实双机测量后再定默认限速。 |
| XFER-07 | `hop_xfer_tracking_visible()` 只向 UI 暴露一个“可见”方向；`xfer_cancel()` 没有 task ID/方向参数。 | 双向同时传输时状态互相覆盖；取消可能影响另一个方向。 | 改为独立 Transfer Task ID、方向、Offer ID、Request ID、进度、速率、取消与错误状态。 |
| CLIP-06 | 文件接收完成后直接调用 `clipboard_apply_paths()` / `clip_apply_remote_paths()`；写入保护没有覆盖从传输开始到完成的整个用户剪贴板代次。 | 传输期间用户新复制的内容可能被旧任务覆盖。 | 以 request 起始时接收端剪贴板代次为条件；用户发生新复制后取消自动写剪贴板/自动粘贴，只保留已接收文件并提示。 |
| XFER-08 | 接收端有 8-slot 写盘队列，但发送端缺少总量背压；队列容量与配置最大 4MB chunk 共同决定峰值内存。 | 大分块/大量作业时内存峰值不可预测。 | 发送端与接收端都设字节预算；记录高水位；生产者等待/拒绝策略明确；任何队列都不得无界。 |
| XFER-09 | Windows/macOS `xfer_submit_*()` 当前主要按 Batch ID 去重后追加任务；`XFER_MAX_ENTRIES = 8192` 只限制单个任务，不能阻止多个任务累计耗尽队列。 | 多个看似合法的任务叠加后，jobs、文件条目元数据和待传输字节数仍可无限增长；并发 submit 还可能同时通过各自检查。 | 增加跨平台一致的全局资源预算管理器；提交前原子预留 jobs、entries、bytes 和 image-memory 四类资源；覆盖排队、扫描、等待、发送/接收中任务；超限整体背压/拒绝，不得部分入队。 |
| XFER-10 | Windows/macOS 图片提交路径仍会将完整图片数据复制到堆内存后再排队；当前没有跨所有图片任务的累计内存额度。 | 连续复制/排队多张高分辨率图片时，完整 payload 副本叠加，造成不可预测 RSS 峰值，甚至内存耗尽。 | 图片资源独立计入全局 image-memory budget；分配/复制前预留额度；优先使用有界缓冲区、引用计数只读快照或文件 backing/spool；若系统 API 必须物化完整图片，必须先检查单图上限和全局剩余额度，超限明确降级/拒绝。 |

## 2. 目标架构

### 2.1 剪贴板控制面与文件数据面分离

- **控制面（现有控制 TCP）**：同步文本、图片元数据/状态、File Offer、Paste Request、Paste Ready/Failed、Paste Execute/Cancel。
- **数据面（现有独立数据 TCP）**：只承载已经被授权的 FILE_BEGIN / FILE_CHUNK / FILE_END 数据。
- **源端 Offer Registry**：以随机不可预测 Offer ID 为键；本机保存原始路径列表、复制代次、来源设备、创建时间、会话、轻量条目名。不得向对端暴露绝对路径。
- **目标端 Remote Offer Registry**：只保存来源设备、Offer ID、剪贴板代次、名称和有效状态；不创建接收目录、不产生传输历史。
- **Paste Transaction**：每次跨端粘贴创建独立 Request ID；源端验证 Offer、路径和会话后才扫描并入队；目标端记录本地剪贴板版本，避免旧任务覆盖新内容。
- **Transfer Task**：由 Paste Transaction 派生，具有唯一 Task ID、方向、源/目标、文件数/总字节、已完成数/字节、速率、阶段、取消状态和错误原因。

### 2.2 关键协议语义

新增控制消息（最终编号需检查当前消息空间并在 Windows/macOS 两端同时实现）：

- `CLIP_FILE_OFFER`：Offer ID、origin device、clipboard generation、条目数量及显示名；不包含绝对路径、不包含文件字节。
- `CLIP_FILE_OFFER_CLEAR`：新剪贴板内容覆盖文件 Offer 或 Offer 失效。
- `CLIP_PASTE_REQUEST`：Request ID、Offer ID、接收端 clipboard generation、会话标识。
- `CLIP_PASTE_ACCEPTED / CLIP_PASTE_FAILED`：源端验证结果；错误必须可诊断。
- `CLIP_PASTE_READY`：数据已完整落盘并通过验证。
- `CLIP_PASTE_EXECUTE`：目标端确认仍是同一剪贴板代次/目标请求后执行待处理粘贴；如果目标剪贴板已改变则不执行。
- `CLIP_PASTE_CANCEL`：取消对应 Request/Task，不影响其他方向或其他任务。

协议版本：控制面新增消息后递增控制协议版本；如数据帧格式也发生变化，再同步递增数据协议版本。Windows/macOS 版本不匹配时必须在握手阶段明确拒绝，不能静默降级到“复制即传输”。

### 2.3 粘贴触发与系统行为

- 粘贴拦截必须发生在真正接收 Ctrl+V / Cmd+V 的目标设备输入路径；本地 Offer 不拦截，本机原生粘贴完全放行。
- 当目标设备当前剪贴板指向远端 File Offer 时，拦截本次粘贴并发送 Paste Request；源端才开始读取文件。
- 数据完成后先写入接收端原生文件剪贴板，再恢复/执行待处理粘贴。需要验证 Windows SendInput 和 macOS CGEvent 的合成快捷键不会再次触发 HopDesk 拦截。
- 如果目标应用焦点、会话、剪贴板代次或 Request ID 已变化，不得把旧粘贴注入到新窗口；取消自动执行并保留已接收文件。
- 必须覆盖 Ctrl+V、macOS Command+V，以及项目当前键码映射/用户实际 Ctrl 键习惯。修饰键按下/释放必须成对，不允许吞掉 Ctrl/Command 或 V 的释放事件。
- 如果某平台无法安全拦截/重放快捷键，先提供“文件已准备好，请再次粘贴”的安全降级，不得错误地宣称已自动粘贴。

## 3. 三阶段实施计划

### 阶段一：正确性与按需传输（最高优先级）

**目标**：消除复制即传输，建立 Offer → Request → Transfer → Ready 的闭环；保证本机粘贴原生且不消耗 Offer。

任务：

1. 建立跨平台共享的 Offer/Request 数据结构、ID 生成、代次比较、状态迁移和过期规则。
2. Windows/macOS 剪贴板监听只捕获文件路径快照并发布轻量 Offer；不调用 `xfer_submit_paths()`，不扫描目录、不读取文件。
3. 新增并验证控制协议消息；校验来源设备、会话、Offer ID、Request ID、剪贴板代次和消息长度。
4. 在 Windows/macOS 目标输入路径拦截“目标剪贴板是远端文件 Offer”的 Ctrl+V/Command+V；本机 Offer 与普通文本/图片不拦截。
5. Paste Request 到达源端后，后台验证路径仍有效，再递归枚举并创建传输作业；扫描和发送都可取消。
6. 接收端只在任务完成且 request/clipboard generation 仍有效时更新文件剪贴板并执行粘贴；用户期间复制了新内容则不覆盖。
7. 修复短读/读错误、实际接收长度、CRC、原子落盘、符号链接/reparse point、路径碰撞和超限处理。
8. Offer/Request/Task 历史语义分开：没有跨端粘贴不得出现“发送/完成”任务；每次真实粘贴才有传输记录。
9. 增加核心单测、协议测试和 TCP 双端集成测试；Windows/macOS 同步实现，不允许只完成一个平台。

阶段一验收：

- Windows 复制文件夹 → Windows 本机粘贴：不发生数据传输；HopDesk 无发送/完成记录。
- Windows 复制文件夹 → 先本机粘贴多次 → Mac Ctrl+V：只在 Mac 粘贴时产生一次按需任务；文件结构/内容一致。
- Mac 复制文件 → Mac 本机粘贴 → Windows Ctrl+V：反向同样成立。
- 复制后不跨端粘贴：数据通道字节数为 0、传输任务数为 0。
- Offer 仍有效时可重复跨端粘贴；新复制后旧 Offer 不可请求。
- 粘贴期间源文件删除/权限变化/断线/接收端新复制：明确失败或取消，不产生伪成功、不覆盖新剪贴板。
- 10000 条目目录、空目录、Unicode/长路径、符号链接/Junction、超限、读错误、磁盘写满、CRC 损坏均有明确结果。
- 本地键盘/鼠标事件无丢失；修饰键不残留；没有真实桌面证据的测试标记为未验证。

### 阶段二：性能、资源控制与稳定性

**目标**：传输是低优先级后台工作，传输期间用户仍可正常使用两台电脑。

任务：

1. macOS：主队列只读取 changeCount 和轻量剪贴板快照；图片解码、行转换、文件枚举、网络发送移到 utility worker。
2. Windows：剪贴板锁只用于快照复制；文件路径采集与图片转换后台执行；禁止在 UI/window procedure 中递归扫描或等待网络。
3. 建立 Windows/macOS 共享语义的 **Global Transfer Resource Budget**（全局传输资源预算管理器），`submit()` 必须在任何大块分配、完整图片复制或正式入队前申请资源；不能只做 Batch ID 重复检查。
   - 全局最大排队/活动任务数（jobs）：限制所有待处理、扫描中、等待发送、发送中及接收中任务的总数。
   - 全局累计文件条目数（entries）：跨所有任务累计；包含扫描中已发现并保留的条目元数据，不把 `XFER_MAX_ENTRIES`（单任务上限）当成全局限制。
   - 全局累计待传输字节数（pending bytes）：跨所有任务累计，使用 64-bit 计数和溢出安全检查；目录扫描过程中增量预留，整个批次超限时整体取消/拒绝，不允许部分成功后留下半个批次。
   - 全局图片内存预算（image bytes）：跨所有排队/处理中图片任务累计，统计实际持有的 payload、转换缓冲区和必要副本；明确区分可共享只读缓冲区与真实重复分配，避免重复计数或漏计。
   - 保留单任务条目数、单文件大小、单图片大小等 per-task/per-item 上限；单任务上限与全局上限同时生效。
   - 资源预留与任务入队必须原子化（锁/原子状态机），并发提交不得超卖；失败的预留必须回滚。
   - 任务从扫描、排队、发送/接收、完成、失败、取消、超时到断线清理，均通过统一生命周期释放预留；释放幂等，禁止重复释放、泄漏和计数下溢。
   - 预算耗尽时采用有界等待/背压或明确拒绝，并向 UI/日志给出可诊断原因；不得忙等、无限排队或静默丢弃。
   - 接收端写盘队列同时限制 slot 和 bytes；发送端、接收端、图片任务使用独立子预算并受总预算约束。
   - 预算配置集中管理；Windows/macOS 使用相同配置项、单位、默认策略和边界语义。具体数值先通过真实设备 RSS/工作集、典型数据集和峰值测试校准，再固化为可调整配置；在数值未验证前不得宣称有硬内存上限。
4. 加入 Token Bucket 限速和可配置上限；默认值通过实际 Wi-Fi/千兆网实测确定，不以本机回环吞吐推定。控制消息和键鼠消息始终优先。
5. 增加自适应节流：根据控制 RTT、发送队列积压、磁盘写入延迟调整数据速率；连续拥塞时降速，空闲时缓慢恢复。
6. 降低不必要的内存复制；发送侧固定大小缓冲池；接收侧保持有界写盘队列。
   - 图片 `submit()` 不得在预算检查前将完整图片复制进新的无界堆缓冲区；先取得 image-memory reservation，再创建/共享 payload。
   - 优先使用有界分块读取/转换、引用计数不可变快照，或临时文件 backing/spool，避免多张大图在队列中各自持有完整堆副本。
   - 对必须由系统 API 一次性物化的图片数据，设置单图最大值和全局累计 image-memory 上限；预算不足时不复制、不排队，明确拒绝或安全降级。
   - 图片解码/格式转换产生的临时缓冲区也纳入预算；转换完成、任务取消或失败后及时释放。
7. 优化小文件目录：在不牺牲 per-file ACK/CRC/失败隔离前提下，引入有限 ACK 窗口或小文件批次；严禁无界并发。
8. 为资源预算建立可观测指标：jobs/entries/pending bytes/image bytes 的当前值、上限、预留失败数、等待时长、峰值高水位、任务释放/回滚次数；日志可按 Task/Request ID 溯源但不记录绝对路径。
9. 对扫描、读取、CRC、发送、接收、写盘、剪贴板应用分别计时；UI 展示当前任务速率和队列状态。
10. 对线程优先级、QoS、磁盘 I/O、网络带宽、取消延迟和关闭生命周期做压力测试。

阶段二验收目标（基于实机基线比较，不得只凭理论声称达标）：

- 传输期间主线程/主队列无目录扫描、无大图像转换、无同步网络等待。
- 普通桌面空闲时 CPU 不因剪贴板轮询/重试持续唤醒；复制文件但不粘贴时 CPU/磁盘扫描增量接近空闲基线。
- 队列内存有硬上限；10000 小文件和多个连续任务不会无界增长。
- 多个 `submit()` 并发执行时，jobs、entries、pending bytes、image bytes、接收写盘队列 slots/bytes 六项全局计数任何时刻都不超过各自预算；边界值、超限值和整数溢出均有测试。
- `XFER_MAX_ENTRIES` 继续作为单任务上限，但不能替代全局 entries 上限；多任务累计条目数达到全局上限时必须整体背压/拒绝。
- 图片在预算预留前不得进行完整堆复制；连续提交多张大图时 RSS 峰值受全局 image-memory budget 约束，释放/取消后计数回落。
- 任务在排队、扫描、传输、完成、失败、取消、超时、断线和应用退出各路径均准确释放预算；重复完成/取消不会重复释放。
- 队列满时不会阻塞 UI/剪贴板回调，不会忙等；拒绝或等待状态可观测，恢复容量后可继续提交。
- 传输期间鼠标/键盘可操作；控制通道 RTT p95 相对空载基线增幅目标不超过 2 倍，p99 不超过 3 倍（实机记录基线和测量方法）。
- CPU 使用率、RSS、磁盘吞吐、网络速率、控制 RTT、键鼠事件延迟、取消响应时间均输出测试报告。
- 1GB 文件、10000 小文件、4K 图片、双向同时传输、Wi-Fi 波动/断线恢复均通过。

### 阶段二执行进度（持续更新）

**第一批已提交：发送端全局资源预算基础。**

- 新增跨平台公共模块 `src/core/xfer_budget.h/.c`，Windows 使用 SRWLOCK，macOS 使用 pthread mutex；两端共享同一套预算语义。
- 初始发送端默认上限：jobs=16、entries=32768、pending bytes=8 GiB、image bytes=256 MiB、单张图片=256 MiB。**这些数值是安全起始值，不是实机调优结论**，后续必须通过 RSS/工作集压测校准。
- Windows/macOS 文件提交先预留 job slot；目录扫描发现每个文件/目录时原子预留 entry 和文件字节；任一资源超限则整批回滚，不进入队列。
- 图片提交在分配/复制 payload 前预留 job、entry、pending bytes 和 image bytes；任务释放时由 job 生命周期统一释放 reservation。
- 第一批只覆盖发送端排队/活动作业及 xfer 层持有的图片 payload；接收端写盘队列与图片转换临时内存尚未纳入。

- 新增跨平台公共模块 `src/core/xfer_budget.h/.c`，Windows 使用 SRWLOCK，macOS 使用 pthread mutex；两端共享同一套预算语义。
- 初始默认上限：jobs=16、entries=32768、pending bytes=8 GiB、image bytes=256 MiB、单张图片=256 MiB。**这些数值是安全起始值，不是实机调优结论**，后续必须通过 RSS/工作集压测校准。
- Windows/macOS 文件提交先预留 job slot；目录扫描发现每个文件/目录时原子预留 entry 和文件字节；任一资源超限则整批回滚，不进入队列。
- 图片提交在分配/复制 payload 前预留 job、entry、pending bytes 和 image bytes；任务释放时由 job 生命周期统一释放 reservation。
- 第一批覆盖发送端排队/活动作业及 xfer 层持有的图片 payload；第二批已将接收端写盘队列纳入 Task reservation 和全局账本；第三批进一步覆盖 macOS 图片快照/解码/转换临时内存，以及 Windows DIB 快照/转换内存。macOS `dataForType`、图片解码和 BGRA 转换均在 utility worker 执行，Windows 剪贴板锁只覆盖系统数据快照复制。
- 图片临时缓冲区与最终队列 payload 共用全局 image-memory ceiling；新增 `hop_xfer_budget_transfer_image_temp()`，在不重复计数同一物理分配的前提下，将已分配的 BGRA 缓冲区预算原子转移到正式传输 Task。临时 Task 和传输 Task 分别持有各自的 Task ID / Reservation Handle。
- 新增 Token Bucket + AIMD 发送限速：按 socket send duration（按 chunk 大小归一化）、FILE_ACK 等待时间和本机接收写盘队列压力反馈；每 8 个 chunk 进行一次反馈，拥塞时乘性降速，健康时按配置上限的 1% 加性恢复。Windows 使用高精度 waitable timer（不可用时回退），macOS 使用可中断的短间隔 nanosleep。
- 新增配置项 `xfer_rate_limit_mib`（16–4096 MiB/s），默认 128 MiB/s、初始 64 MiB/s、最低 4 MiB/s；默认值是暂定起始值，未经过真实 Wi-Fi/千兆网校准。限速值不能被当作实机性能结论。
- 图片 owned-buffer 提交现在在预算检查后直接接管现有像素缓冲区；发送图片 chunk 不再复制到通用 tx buffer。预算快照现在同时返回当前 usage、高水位、拒绝/释放/转移计数和配置上限。
- 传输状态新增基于单调时钟的 speed / ETA：按实际进度采样并使用 EWMA 平滑；连续 2 秒没有进度时，UI 不再显示陈旧速率。Windows/macOS 传输管理页显示 jobs/entries/pending/image/RX 当前值与限额、高水位、拒绝数、reservation/RX/temp 释放数、图片缓冲转移量及账本异常。
- 新增共享固定容量 4 的 ACK window：发送端在 FILE_BEGIN 前登记 batch/id，在 FILE_END 前标记发送；ACK 按 batch/id 精确归位，支持乱序到达；窗口填满后再统一收取 ACK。每个文件仍保留独立 CRC、ACK、失败隔离与重试，TCP 数据帧仍串行发送，没有引入无界并发或额外数据缓冲。
- ACK 超时按每个 FILE_END 的单调时钟 RTT/截止时间计算；取消、对端 ABORT、断线和任务结束都会清理该 batch 的所有 ACK 槽，过期 ACK 不会覆盖其他文件或重试状态。
- **仍未完成**：真实双机 RSS/吞吐/键鼠延迟压测；剪贴板异步应用完成时刻（当前只记录 API dispatch 耗时，不冒充系统剪贴板实际完成时间）。阶段计时已在第五批补齐。发送端目前只有一个 sender thread，因此使用单个固定大小、线程生命周期内复用的 TX buffer 已足够；图片 payload 直接 owned-buffer 分段发送，不需要再复制进 TX buffer。只有未来引入并行 sender 时才需要扩展成多槽固定 buffer pool。ACK window 目前是有界文件级在途回执，不代表接收端并行写多个文件。阶段二仍不能在实机验收前标记整体完成。
- `pending bytes` 当前按任务完整保留量计数，直到任务终态统一释放；不是实时递减的剩余字节显示值。
- 第一批预算边界、超限、幂等释放及 32 线程并发抢占测试通过；Windows HA 测试夹具已链接新模块。第一批 CI：[Actions #42021950477](https://github.com/alex520lq/HopDesk/actions/runs/42021950477)，提交 `8ba90741ba8bb170a795c895f093cc78e7ad3aa1`。

**第二批已提交：接收端写盘队列预算。**

- 新增接收队列全局限制：8 slots / 8 MiB；slot 统计包含排队、正在写盘和正在分配的缓冲区，不再漏算 writer 当前持有的 chunk。
- 接收 chunk 在 `malloc`/复制前预留 Task 级 rx buffer slot 与 bytes；写线程释放实际 buffer 后归还 reservation。接收任务在所有 writer buffer 释放前不能进入终态或销毁 Task。
- 资源快照新增 rx queue 当前值/高水位、拒绝次数、释放次数和账本异常计数；测试覆盖 slot/bytes 边界、错误释放和任务终态保护。
- Windows/macOS 原生构建、核心测试、回归测试、协议、TCP、角色重连及打包 CI 全部通过：[Actions #42042092381](https://github.com/alex520lq/HopDesk/actions/runs/42042092381)，代码提交 `e8c7d300910a859ca11842321dce254ec005170e`，测试夹具提交 `1da692c071b7dcf7c4627fc1aa2cc55a44075409`。
- 真实双机性能、RSS、磁盘压力及键鼠延迟尚未执行；CI 通过不代表实机验收通过。

**第三批已提交：剪贴板图片临时内存、owned-buffer 预算转移与自适应限速。**

- macOS 图片 `dataForType`、NSImage 解码、CGContext bitmap 和行翻转均在 utility worker；预留临时内存额度后才物化剪贴板数据，按真实像素尺寸收窄额度。
- Windows DIB 快照在复制前预留临时内存，剪贴板锁只覆盖数据快照；BGRA 转换和传输提交在后台 worker。
- 新增原子 image-temp → queued-image 预算转移，避免同一 BGRA allocation 在转换 Task 和传输 Task 交接时被重复计数；失败路径释放源 reservation，成功路径由目标 Task 持有最终 payload reservation。
- 图片提交使用 owned-buffer 接口，避免再复制整张像素图；文件/图片 chunk 发送直接引用原有数据缓冲，图片 chunk 采用 header + payload 分段发送。
- 新增共享 Token Bucket/AIMD 限速模块；按 chunk 大小归一化 socket send time，结合文件 ACK 延迟与本机 RX 写盘队列压力进行调节；每 8 个 chunk 反馈一次。
- 新增 `xfer_rate_limit_mib` 配置（16–4096 MiB/s，默认128）；预算快照同时暴露当前限额。预算转移、限速、配置边界均增加核心测试。
- Windows/macOS 原生构建、核心测试、文件传输协议测试、TCP loopback、角色/重连集成测试和打包 CI 全部通过：[Actions #42051872404](https://github.com/alex520lq/HopDesk/actions/runs/42051872404)，代码提交 `f3190e77764f171d4c23d3c4cbefebfd2e6e5701`。
- **实机验收仍未执行**；默认速率尚未实测校准。第四批已补齐有界 ACK window 和预算 UI；剩余完整阶段计时、发送侧固定大小可复用缓冲池和压力报告继续列为待办。

**第四批已提交：传输可观测性与有界 ACK window。**

- `hop_xfer_tracking` 以单调时间计算传输速率和 ETA，进度回退/文件重试会重置采样；UI 在进度停滞超过 2 秒后隐藏过期速率。
- Windows/macOS 传输页展示当前资源 usage/limits、RX 高水位、预算拒绝、reservation/RX/temp 释放、图片临时缓冲转移及账本异常计数；同时修复 macOS 当前任务卡片标题与任务标题重叠。
- 新增 `src/core/xfer_ack_window.h`：固定 4 槽、batch/id 关联、发送/收到/取出状态、RTT 记录、单项/整批清理。
- 两端 sender 以 4 个文件为上限填充窗口，再按文件收取 ACK；CRC、逐文件 ACK、失败隔离和重试仍保留。接收端仍按 TCP 顺序逐个完成文件写盘，不会同时打开多个当前文件。
- 每个 ACK deadline 从 FILE_END 完整写入后计算，并统一使用单调纳秒时钟。ACK 槽在发送 FILE_END 前先进入 sent 状态，避免快速 ACK 竞态；写入完成后再发布独立 deadline。若 ACK 在 deadline 发布前到达，先保存 received timestamp，随后按实际接收时间与 deadline 比较，不再把 FILE_END 写入耗时错误计入 ACK 等待期限。
- 新增核心测试覆盖 ACK 先于 deadline 发布的竞态、deadline 前到达和恰好到达 deadline 的边界（等于 deadline 按超时处理）。
- ACK window 使用局部 send cursor 填充；重连 checkpoint `j->next_i` 只在对应文件获得最终 ACK 或明确终态结果后推进。断线时从最早未确认文件恢复；若在窗口填充阶段断线，当前窗口可能安全重传，但不会跳过未确认条目。认证 session 变化时仍从头开始，避免把旧接收会话的进度误当成新会话状态。
- 恢复 ACK window 重构时意外丢失的 Windows `send_image_job()` / `send_done()`，避免图片发送和批次结束路径链接失败。
- 最新 CI [Actions #42059765078](https://github.com/alex520lq/HopDesk/actions/runs/42059765078)：Windows MSVC、核心、协议、TCP、角色/重连集成、更新脚本、打包与 Artifact 上传全部通过；macOS job 未分配 runner（runner_id=0、无执行步骤），因此 macOS 本轮结果为基础设施未执行，不能标记通过。实机小文件吞吐、磁盘压力和键鼠延迟仍待测试。

**第五批已提交：按批次记录分阶段耗时。**

- `hop_xfer_tracking` 为每个可见批次累计单调纳秒耗时：scan、read、CRC、send、receive、write、clipboard_dispatch；计数采用饱和累加，终态后不再接受新耗时。
- Windows/macOS 在批次结束时输出一行结构化 timing 日志，包含 batch、方向及各阶段毫秒数；不记录绝对路径。扫描时间随发送作业保存，并只在新的状态快照建立时计入，重连同一 session 不重复累计；session 变化重建快照时重新纳入原始扫描耗时。
- `receive` 表示接收解析/校验/入队阶段，包含 CRC 与队列背压，因此与独立 `crc` 统计存在重叠；`write` 是后台 writer 实际执行时间；`clipboard_dispatch` 只表示调用剪贴板 API/提交主线程工作的耗时，不代表异步 pasteboard 最终完成。
- 增加核心测试覆盖累计、溢出饱和、无效阶段和终态后拒绝计时。
- 发送侧缓冲复用复核：每个平台只有一个 sender thread，在该线程启动时分配一次 `CHUNK_HDR + chunk` TX buffer，线程退出时释放，文件分块在整个生命周期复用；图片使用已预算的 owned pixel buffer + header/payload 分段发送，不产生第二份整图 payload。当前单发送线程架构无需再建立多槽 buffer pool。
- 当前 CI [Actions #42060905387](https://github.com/alex520lq/HopDesk/actions/runs/42060905387) 对应较早的部分提交：Windows 原生编译通过，但 core test 因测试 fixture 在栈上分配大型 `hop_xfer_tracking` 而 stack overflow；已改为 static fixture（`78f2cf87`）。该 run 的 macOS job 未分配 runner（runner_id=0、无执行步骤）。当前完整计时实现尚无对应 CI run；需由有 Actions 权限的用户手动运行 Build HopDesk / workflow_dispatch，不能把旧 run 当作最新代码验证。

### 阶段三：任务模型、双向一致性与产品验收

**目标**：传输管理器能准确表示多任务和双向状态，不互相覆盖、不误取消。

任务：

1. 将当前 send/receive 单一可见状态改为 Task ID 索引的任务集合。
2. 每个任务分别保存 Offer ID、Request ID、session、direction、phase、files/bytes、speed、ETA、retry、cancel token、error。
3. 取消/重试 API 接受 Task ID；取消发送不影响接收，取消一个 Request 不影响其他 Request。
4. UI 展示发送与接收任务列表，支持同时显示双向进度、排队、扫描、等待、传输、校验、完成、部分失败、取消。
5. 历史记录按真实 Paste Transaction 记录；Offer 广播、普通本地复制、未粘贴不记为传输。
6. 统一 Windows/macOS 的错误码和用户可读原因；路径隐私：日志不泄露完整本机绝对路径。
7. 处理重复粘贴、双击粘贴、连续粘贴、任务去重、请求超时、迟到 ACK、旧 session、重连和应用退出。
8. 补齐可自动化协议状态机测试、文件系统故障注入和两平台 UI 状态测试。
9. 将全局资源预算作为任务状态机不变量：每个 Task ID 持有明确 reservation；所有状态转换只允许一次性提交/释放资源；UI 能展示资源不足/等待原因，取消/重试不会串改其他任务的 reservation。
10. 增加资源账本一致性检查：运行时预算计数与任务集合汇总可对账；测试覆盖重复消息、迟到 ACK、重连、并发取消、进程退出和任务恢复，发现计数漂移立即报错并保留诊断信息。
9. 完成两种角色组合的真实双机验收：Windows server → macOS client、macOS server → Windows client。

阶段三验收：

- 双向同时传输时 UI 同时显示两个方向的独立任务。
- 取消/重试只影响指定 Task ID。
- Offer 复制/本机粘贴不会显示成传输任务。
- 同一 Request 的重复控制消息幂等；旧 session/旧 Offer/旧 Request 不会启动或覆盖新任务。
- 任务完成、部分失败、失败、取消、重连恢复的状态和历史一致。
- 所有 Task ID 的资源 reservation 与全局账本一致；任务终态后 reservation 恰好释放一次。
- 双向同时传输、并发图片提交、连续取消和断线重连下，全局资源预算不超限且无资源计数漂移。
- Windows/macOS 原生构建与自动化 CI 全部通过；实机未测项目单独标注 BLOCKED，不得用 mock 代替。

## 4. 性能压测矩阵

| 场景 | 数据集 | 重点指标 |
|---|---|---|
| 本地复制不粘贴 | 单文件、10000 文件目录 | 传输任务数=0；扫描字节=0；CPU/RSS/磁盘变化 |
| 本机反复粘贴 | 1GB 文件、目录 | 100% 原生行为；HopDesk 网络数据=0 |
| 跨端大文件 | 1GB、4GB（可用时） | 吞吐、CPU、RSS、温度/风扇、控制 RTT |
| 跨端大量小文件 | 10000 × 4KB；1000 × 1MB | 文件枚举耗时、ACK RTT、队列峰值、完成率 |
| 深层目录 | 20+ 层、空目录、Unicode 名称 | 路径正确性、深度上限、取消响应 |
| 图片 | 1080p、4K、8K（上限内） | 转换耗时、额外内存、UI 卡顿 |
| 图片资源预算 | 多张 4K/8K 图片连续提交；多任务并发；预算边界/超限 | image bytes 当前值/高水位、RSS 峰值、预留失败、取消释放、是否发生完整堆复制 |
| 全局队列预算 | 多任务并发提交；每任务小于单任务上限但总量超过全局上限 | jobs/entries/pending bytes 峰值、原子拒绝/背压、计数一致性、UI 响应 |
| 双向传输 | 两端同时各传 1GB | 任务隔离、总吞吐、键鼠响应 |
| 弱网 | Wi-Fi、限速、丢包/断连 | 自适应降速、重试、无伪成功 |
| 磁盘压力 | 慢盘、磁盘满、权限拒绝 | 背压、错误传播、临时文件清理 |
| 长稳 | 2 小时传输、8 小时空闲 | CPU/RSS/句柄/线程泄漏、轮询空转 |

所有报告必须记录 OS 版本、硬件、网络类型、提交 SHA、测试命令、数据集、测量工具和结果。对同一设备分别记录空载基线与传输期间结果。

## 5. 交付与 Git 工作流

- 本计划与实施继续沿用仓库当前唯一活动分支 `fix/dual-role-server-client` 及其 PR #8；不得另开并行开发 PR。
- 不直接提交到 `main`，不擅自合并 PR。
- 本文件是问题溯源和验收清单；每个阶段开始/完成都追加提交 SHA、改动文件、自动化测试命令/结果、CI 链接、实机验证状态。
- 协议变化必须同步 Windows/macOS；如果新版本不兼容，PR 和 README 必须明确要求两端一起升级。
- 每个阶段单独形成可审查提交；不要把未经测试的大量改动压成一个提交。
- 所有“通过”必须有证据；未运行的原生构建、真实桌面/双机测试必须标注“未执行/待实机”。

## 6. 实施记录

| 阶段/记录 | 提交 SHA | 变更 | 自动化测试 | 原生 CI / 实机 |
|---|---|---|---|---|
| 计划建立/基线校准 | `614959cbc74c1c29540a7a7d31f4bd1447d2dd31` | 已存在本计划文档；校准为 PR #8 当前 HEAD，保留问题 ID、产品语义、架构、三阶段任务和验收矩阵 | 文档与代码锚点复核 | 待 CI |
| 阶段一代码与 CI | `59260df3e627aef3b0b2fa63873f391226680fa4` | Offer/Request、按需扫描/传输、协议与测试夹具修复；Windows/macOS 主程序及打包 | 核心、协议、TCP、HA/角色重连测试通过 | CI 全部成功；真实双机/桌面粘贴验收仍待执行 |
| 阶段二第一批：发送端预算基础 | `e47a3b1ac5802b0dcb23f7137734bea254ca6470`（代码）；`7185ecd10279dcfd06574b422a438706af65f00f`（Windows 测试夹具）；`8ba90741ba8bb170a795c895f093cc78e7ad3aa1`（并发测试） | 新增跨平台预算账本；发送端 jobs/entries/pending bytes/image bytes 预留；图片复制前预算检查；32 线程并发预算测试 | Windows/macOS CI 全部成功；[Actions #42021950477](https://github.com/alex520lq/HopDesk/actions/runs/42021950477) | 已完成第一批自动化验证；临时图像内存、限速/节流、实机压测仍待实施；文件枚举已由 Paste Worker 在后台执行 |
| 阶段二第二批：接收端写盘队列预算 | `e8c7d300910a859ca11842321dce254ec005170e`（代码）；`1da692c071b7dcf7c4627fc1aa2cc55a44075409`（回归夹具） | 接收写盘队列 slot + bytes 纳入 Task reservation 与全局账本；writer 活动 buffer 计入 slot；分配前预留、释放后归还；账本高水位/拒绝/释放/异常计数 | Windows/macOS CI 全部成功；[Actions #42042092381](https://github.com/alex520lq/HopDesk/actions/runs/42042092381) | 自动化验证通过；实机压测未执行 |
| 阶段二第三批：图片临时预算转移与自适应限速 | `f3190e77764f171d4c23d3c4cbefebfd2e6e5701` | 图片快照/转换临时预算、原子 owned-buffer 预算转移、分块直发、Token Bucket/AIMD、可配置速率上限、预算限额快照 | Windows/macOS 核心、协议、TCP、角色/重连集成与打包全部通过；[Actions #42051872404](https://github.com/alex520lq/HopDesk/actions/runs/42051872404) | 自动化验证通过；真实双机/弱网/RSS/键鼠延迟压测待执行 |
| 阶段二第四批：speed/ETA、预算 UI 与 ACK window | `e99ef04`, `7f84820`, `02659ea`, `2992810`, `98398b5`, `587484e`, `3b99894`, `7072d1c`, `db37431`, `2738dab`, `48abfec`, `846da91`, `89ec493`, `86dd3f0`, `38c474b`, `3685f3e`, `7b50d28`, `7adfc7b`, `625dcd5`, `f032c7c`, `feb95f4`, `f4f78e6`, `f4f966d` | 单调时钟 speed/ETA、双端资源预算可观测 UI、固定 4 槽 ACK window、逐文件重试、单调 deadline、ACK 后推进重连 checkpoint、恢复 Windows 图片/批次发送 helper | Windows 完整 CI 通过 [Actions #42059765078](https://github.com/alex520lq/HopDesk/actions/runs/42059765078)；macOS runner 未分配，待重跑 | 实机 10,000 小文件、ACK 丢失/超时、断线恢复、双向并发和键鼠延迟待测 |
| 阶段二第五批：分阶段耗时 | `b8b81e9`, `8447a12`, `5f9925d`, `721ac41`, `6f83067`, `1bab2c5`, `be7f563`, `067c3a8`, `78f2cf8`, `9b8018b` | 批次级 scan/read/CRC/send/receive/write/clipboard dispatch 纳秒累计；终态饱和统计；完成时输出结构化日志；补充计时边界测试；复核单 sender 固定 TX buffer 复用；清除 Windows cursor resource ANSI/Unicode 编译警告 | 完整实现待 Windows/macOS 新 CI；旧 run 的 stack overflow 已由 static fixture 修复 | clipboard 实际异步完成、实机性能/RSS/键鼠延迟待测 |
| 阶段三 | 待实施 | Task ID 状态模型、reservation 账本、独立取消/重试、UI/历史与双向验收 | 待执行 | 待执行 |

## 7. 本轮明确不做的事

- 不以“复制时立即传输，传完再粘贴”作为兼容方案。
- 不把绝对路径发送到对端。
- 不以增大 chunk、提高线程优先级或开启无界并发作为性能优化。
- 不为了让 CRC/长度测试通过而放宽接收校验。
- 不在未确认键盘事件目标、修饰键状态和焦点变化时盲目自动重放 Ctrl+V。
- 不把本机回环压测当作真实双机性能证据。
- 不因传输重构顺手改动无关的双角色键鼠/UI 功能。
