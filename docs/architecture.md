# HopDesk 架构设计

## 1. 角色与拓扑

- **服务器（Server）**：可运行在 Windows 或 macOS，监听控制与数据两个 TCP 端口，捕获本机物理键鼠。
- **客户端（Client）**：可运行在 Windows 或 macOS，填写服务器地址，主动连接两个通道并注入输入。
- 两端均支持双向文本、图片和文件剪贴板；文件使用 Offer → Paste Request → 按需传输，不在普通本地复制时创建传输任务。
- `role=server|client` 在启动时读取；设置保存到独立草稿，不热改运行态。旧配置保留原 OS 默认角色。
- 固定双设备局域网，不做多设备发现。两端几何在控制握手交换；当前控制协议为 v7，数据协议为 v8。

## 2. 模块划分

```
src/
├── core/    纯 C 共享业务核心：协议/session、Clipboard transaction、Sender/Receiver
│            Task/Budget/History、ACK/checkpoint、rate/tracking、RX queue/buffer pool
├── win/     Win32/Winsock/文件句柄/线程与 Event/原生剪贴板/键鼠/显示器/UI Adapter
└── mac/     POSIX/Cocoa/CoreGraphics/NSPasteboard/pthread/dispatch/显示器/UI Adapter
```

**共享 Core 的主要生产模块**：
- Clipboard：`xfer_clipboard_txn.c/.h` 统一 Offer、Request、Paste 身份与状态转换；原生 sequence/changeCount、路径快照、剪贴板读写和 Worker 调度留在平台层。
- Sender：`xfer_sender_engine`、`xfer_sender_chunk`、`xfer_sender_ack_wait`、`xfer_sender_queue`、`xfer_sender_scheduler`、`xfer_sender_active_job`、`xfer_sender_job_lifecycle`、`xfer_sender_worker_flow` 等共享发送策略与生命周期。
- Receiver：`xfer_rx_frame_gate`、`xfer_rx_dispatch`、`xfer_rx_message_handlers`、Batch/File/Image state、Lifecycle、Receipt、Entry Reservations 等共享准入、状态与终结规则。
- Transfer infrastructure：`xfer_task`、`xfer_task_history`、`xfer_budget`、`xfer_tracking`、`xfer_rate`、`xfer_ack_window`、`xfer_buffer_pool` 等。

平台层保留 socket/文件句柄、原生锁与等待原语、线程生命周期、文件系统操作、系统剪贴板、键鼠注入/捕获、显示控制和 UI。连接循环仍由两端各自实现；不能将其描述为已统一的跨平台 actor。

## 3. 通信协议

**两条 TCP 长连接，按载荷性质分工：**

| 通道 | 端口 | 承载 | 发送方式 |
|---|---|---|---|
| 控制 | 24817 | 键鼠、显示控制、剪贴板文本 | 入队 + 独立发送线程 |
| 数据 | 24818 | 文件字节、图片像素 | 专用线程，流式读写 |

分两条的理由是**队头阻塞**：单条 TCP 流上的 1 MB 分块会把排在后面的鼠标事件
一起拖住几十毫秒。分开后传大文件不影响手感。

控制通道的发送必须异步。`WH_MOUSE_LL` / `WH_KEYBOARD_LL` 的回调跑在装钩子的线程
（主线程），`WM_CLIPBOARDUPDATE` 也在主线程；如果在那里直接阻塞 `send()`，
一次大载荷就能让本机鼠标键盘整体僵住，超时后 Windows 会静默摘钩，输入捕获永久失效。
所以回调只入队，发送由独立线程做，鼠标移动在队列尾做位移累加合并
（累加而非覆盖 —— 相对位移丢掉就是永久落后，光标会越用越偏）。

### 帧格式

```
+--------+----------+------------------+
| type   | length   | payload          |
| 1 byte | 4 bytes  | length bytes     |
+--------+----------+------------------+
```

- `length`：网络字节序（big-endian）的 uint32。
- 文本 payload：UTF-8。
- 单帧上限 4 MB + 1 KB（对应 `xfer_chunk_kb` 的最大值 4096）。
- 解码器按段 `memcpy`，不逐字节走状态机 —— 后者处理 1 MB 分块要循环一百万次。

### 传输与控制互不干扰

这是硬要求，不是优化项。三层保障：

1. **物理分通道**：数据走独立 socket、独立线程，控制帧不必排在 1 MB 分块后面。
2. **线程降级**：传输线程 Windows `THREAD_PRIORITY_BELOW_NORMAL` /
   macOS `QOS_CLASS_UTILITY`，键鼠路径（主线程 / 主队列）永远能直接抢占。
3. **分块让步**：每块之后 `Sleep(0)` / `sched_yield()`，并默认用 256 KB 分块。

实测（同进程双通道，100 MB × 192 批 ≈ 1.6 GB/s 压测，控制通道 200 次/秒 PING）：

| 分块 | 吞吐 | p50（空载 → 传输中） | p99（空载 → 传输中） |
|---|---|---|---|
| 1 MB | 1245 MB/s | 113–122 → 115–138 µs | 172–231 → **427–1430 µs** |
| 256 KB | 1109 MB/s | 110–121 → 109–115 µs | 153–210 → **148–200 µs** |

1 MB 分块下中位数不变但尾延迟劣化约 5 倍；256 KB 下**空载与传输中完全一致**。
代价是 11% 吞吐，而这点吞吐在百兆/千兆链路上都不可见（线路才是瓶颈）。
所以默认 256 KB —— 用纸面吞吐换可感知的顺滑。


### 控制通道消息

| 类别 | 类型 | 方向 | 说明 |
|---|---|---|---|
| 控制 | HELLO / HELLO_ACK | 双向 | 握手 + 共享 token 校验 + 屏幕几何 |
| 控制 | PING / PONG | 双向 | 保活、断线检测 |
| 鼠标 | MOUSE_MOVE | S→C | 相对位移 dx/dy（int16 ×2） |
| 鼠标 | MOUSE_DOWN / MOUSE_UP | S→C | 按键（左/中/右） |
| 鼠标 | MOUSE_WHEEL | S→C | 水平 / 垂直滚动量 |
| 鼠标 | EDGE_LEAVE | C→S | Mac 端鼠标到边缘，请求收回 |
| 键盘 | KEY_DOWN / KEY_UP | S→C | 映射后的 Mac 键码 |
| 键盘 | KEY_MODS | S→C | 修饰键状态位（Ctrl/Alt/Shift/Cmd） |
| 剪贴板 | CLIP_TEXT | 双向 | 文本（含来源标记，避免回环） |
| 显示 | DISP_WAKE | S→C | 唤醒 Mac 显示（保持 N 秒） |
| 显示 | DISP_SET_INPUT | S→C | 调 m1ddc 切面板输入源 |
| 显示 | DISP_SLEEP | S→C | Mac 显示睡眠（断 HDMI 信号） |

### 数据通道消息

一条连接全双工：Windows→Mac 与 Mac→Windows 各自有发送线程，互不干扰。
连接建立后双方都发送 `XFER_HELLO`，载荷为 `version(1) + token_length(1) + token + session(16)`。
收到 HELLO 的一端必须同时验证协议版本、长度、token 与当前控制 session；验证失败立即关闭 socket。
握手成功前收到任何其他数据帧也会关闭连接。两端配置的 token 必须完全一致。

当前数据协议 v8 的每个非 HELLO 数据帧均在下表载荷前加 `session(16) + batch(4)`（共 20 字节）。
CHUNK/END 因而也有显式批次。已有类型体内的 batch 必须与前缀一致；会话错误关闭连接，
旧批次的 CHUNK/END 被忽略。控制协议当前为 v7，控制握手版本与数据协议版本分别校验；数据协议 v7 的 ABORT 增加目标方向，v8 增加 `XFER_DONE_ACK` 批次完成回执。任一协议版本不匹配都拒绝连接，Windows 与 macOS 必须一起升级。DONE_ACK 表示接收端已完成批次收尾并发起剪贴板处理，不保证系统剪贴板更新或目标应用粘贴成功；断线或回执超时且无法确认时，发送端显示结果不确定。

| 类型 | 方向 | 类型体载荷（不含公共前缀） |
|---|---|---|
| XFER_HELLO | 双向 | `ver(1) token_len(1) token[token_len] session(16)`；两端均校验，成功前拒绝其他数据帧 |
| FILE_BEGIN | 双向 | `batch(4) id(4) size(8) mtime(8) flags(1) nlen(2) name plen(2) relpath` |
| FILE_CHUNK | 双向 | `id(4) off(8) len(4) crc(4) data[len]` |
| FILE_END | 双向 | `id(4) status(1) file_crc(4)` |
| FILE_ACK | 双向 | `batch(4) id(4) status(1)` |
| IMAGE_BEGIN | 双向 | `batch(4) id(4) w(4) h(4) stride(4)` |
| XFER_DONE | 双向 | `batch(4) ok(4) total(4) bytes(8)` |
| XFER_ABORT | 双向 | `batch(4) reason(1) target(1)`；target 为接收方的发送或接收任务 |
| XFER_DONE_ACK | 双向 | `batch(4) status(1)`；接收端确认批次终结结果 |

- **目录递归**：每个条目自带相对路径，接收端按路径逐级建目录，
  空目录用 `flags & HOP_FILE_FLAG_DIR` 单独登记。不需要额外的树结构协议。
- **图片不做编码**：直接传裸 BGRA（自上而下）。Windows 从 `CF_DIB` 解出、
  转成 BGRA；Mac 用 `CGBitmapContext` 建 `CGImage`。省掉整个 zlib/PNG 依赖。
- **分块带 64 位偏移**：接收端用 `SetFilePointerEx` / `lseek` 按偏移写。
  当前是单连接顺序发送，这个字段为将来多连接区间并行预留 —— 每个分块写到
  确定的偏移，天然没有乱序重组问题。

### 传输可靠性

| 关注点 | 做法 |
|---|---|
| 单块校验 | 每块带 CRC32（IEEE 802.3，表驱动，~1–2 GB/s）。TCP 的 16 位校验和配合网卡 offload 有漏检可能，GB 级传输漏检一次就是静默损坏 |
| 整文件校验 | 各块 CRC 再折一次得到 file_crc，随 `FILE_END` 传。它同时覆盖数据、**块顺序**和**偏移**，且不需要把整份文件重读一遍 |
| 单文件 ACK | 接收端在 `FILE_END` 之后回 `FILE_ACK`（`OK` / `CRC` / `DISK` / `BADPATH`）。发送端据此决定"重发这一个文件"还是"继续下一个"。只有一个 ACK 槽：发送是串行的，任一时刻只有一个文件在途 |
| 文件级重试 | `HOP_XFER_FILE_RETRY_MAX = 3`，退避 200/400/800 ms。`BADPATH` 属确定性失败，不重试。重试只动失败的那一个文件，`XFER_DONE` 的 `ok/total` 因此是精确计数 |
| 作业级重试 | 链路断了整批退回队尾，仅数据连接重建时保留 `next_i`；控制 session 变更时清零游标并整批重发。退避 0.5–8 s，最多 5 次，总预算 `HOP_XFER_RECONNECT_BUDGET_MS = 30 s` |
| 取消 | 菜单触发后置 `cancel_batch`，发送线程每块之后检查（延迟上限一个分块），`ack_wait` 里也检查，否则会干等满 10 s 的 ACK 超时。接收侧由 conn/listen 线程消费 `cancel_rx`：删半成品、立墓碑，对端再发同批次直接回 `ABORT(CANCEL)` |
| 背压 | 写盘队列有界（`RX_Q_SLOTS = 8`）。队列满 → 接收线程阻塞 → `recv` 停止 → TCP 窗口收缩 → 发送端自动减速。内存上界 = 队列深度 × 块大小，与传输总量无关 |
| 磁盘 I/O 隔离 | 网络线程只做"收包 + 校验 + 入队"，落盘交给独立写线程（同样降级优先级）。文件句柄由写线程独占，避免关文件与写入的竞态 |
| 尺寸边界 | `FILE_CHUNK` 超出 `FILE_BEGIN` 声明的 size 直接丢弃；`IMAGE_BEGIN` 的 w/h/stride 全部校验，上限 `32768` 边长 / `256 MB` |
| 发送互斥 | 接收线程出错时要回 `XFER_ABORT`，和发送线程写同一个 socket。锁粒度是**整帧**，不是单次 `send`/`write`，否则两线程交错会把帧字节搅乱。配套设 `SO_SNDTIMEO = 5 s`，避免发送线程卡死时把接收线程一起拖住 |
| 数据通道身份校验 | v4 HELLO 双向携带协议版本与共享 token；两端校验成功前不接受文件/图片帧、不报告数据通道可用。空 token 被拒绝；token 不加密，TCP 载荷也未启用 TLS |
| 文件 CRC fold | v5 将每块的 64 位 offset 与 chunk CRC 一起折入整文件 CRC；块顺序或位置改变都会改变最终校验值 |

**尚未实现**：

- **多文件并发**：单连接内文件是串行的。局域网下单条 TCP 流已是瓶颈（千兆网络
  理论约 125 MB/s，实际瓶颈取决于网络、磁盘与实现），并发不保证提高吞吐；真要提速应该做"并行读盘 +
  单一发送队列"，而不是多连接。
- **断点续传**：同会话重连可保留已确认条目，但没有 chunk 位图或跨重启持久化。GB 级数据在稳定
  局域网下不需要；跨不稳定链路才值得做，且需要 `RESUME_QUERY` / `RESUME_STATE`
  一对消息 + 接收端按 batch 持久化已完成偏移。


### 回环抑制

剪贴板消息带 `origin`（设备 id）。收到对端同步后写入本机剪贴板时，
该次写入不再触发广播，避免 A→B→A 抖动。

## 4. 键鼠共享


### Windows（捕获）

- 全局低级钩子 `WH_MOUSE_LL` / `WH_KEYBOARD_LL`（在自身进程内，无需独立 DLL）。
- 处于"控制 Mac"状态时：钩子返回非零吞掉事件，同时转发；鼠标钳制在移交边缘。
- 边缘检测：鼠标贴到配置的左/右边界并**停留 `switch_delay_ms`** 才移交
  （对齐 Deskflow 的 `switchDelay`）。移交后 `switch_cooldown_ms` 内不再判定，
  避免在接缝处来回弹。停留判定必须用定时器：光标顶到边界后位置不再变化，
  系统不再投递移动事件，事件驱动的计时永远等不到下一次事件。
- 鼠标移动用相对位移（对标 Deskflow 的 `relativeMouseMoves`）。

### macOS（注入）

- `CGEventCreateMouseEvent` / `CGEventCreateKeyboardEvent` + `CGEventPost(kCGHIDEventTap, ...)`，纯 C。
- 维护 Mac 端虚拟坐标，按收到的相对位移累加。
- 需要 Accessibility 权限。
- 鼠标越过 Mac 屏幕边缘、且越界量累计达到 `edge_margin_px` → 发 `EDGE_LEAVE`，
  服务端收回并恢复本机输入。用"越界距离"而不是"停留时长"，因为本端坐标由
  自己维护，距离是确定量，不需要引入定时器；效果与 `switchDelay` 等价。

### 键码映射

- `core` 内置 Windows 虚拟键码（VK）→ macOS `kVK_*` 映射表。
- 修饰键：Windows **Ctrl → macOS Command**（沿用现有 Deskflow 配置 `modifierCtrl=meta`），
  Win 键 → Command/Option 按配置；Alt ↔ Option。

## 5. 剪贴板同步

- Windows：`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`。
- macOS：`NSPasteboard`，主队列 250 ms 定时器轮询 `changeCount`。

按载荷大小分流：

| 格式 | 通道 | 说明 |
|---|---|---|
| `CF_HDROP` / file URL | 数据通道 | 多选、目录递归（含空目录） |
| `CF_DIB` / TIFF-PNG | 数据通道 | 转裸 BGRA，不做图片编码 |
| `CF_UNICODETEXT` / String | 控制通道 | 文本都很小 |

**文件优先于文本**：资源管理器里 Ctrl+C 一个文件，剪贴板里是 `CF_HDROP`，
只读文本的话会"什么都不发生"。同理，从浏览器复制图片常常同时给
`CF_HDROP`（临时文件）和 `CF_DIB`，发文件更忠实原样。

共享 Core 的 `xfer_clipboard_txn.c/.h` 持有 Offer generation、remote Offer identity、Paste Request 去重/准入、完成版本守卫和 Worker admission 等跨平台事务状态；Windows/macOS 生产路径调用同一组事务转换。原生剪贴板 sequence/changeCount、路径快照、图片转换、系统写入、线程与主队列调度仍由平台 Adapter 持有。

文件复制先发送 Offer，接收端在粘贴请求后才开始传输。Windows 接收端和 macOS 的非访达目标使用
`%TEMP%\HopDesk\<batch>\` / `$TMPDIR/HopDesk/<batch>/` 暂存并保留相对路径，再把顶层条目挂到剪贴板。
macOS 在访达中按 Command+V 时从键盘焦点确认粘贴区域。桌面焦点使用本机桌面路径，普通访达窗口通过访达的插入位置确定目录，需要“自动化：控制访达”权限。
接收端直接在该目录创建源文件/目录并逐步写入，无需完成后重放按键。无法确认焦点或目标目录时使用暂存和第二次粘贴流程。
直接写入时若目标中已有同名顶层条目，
本次接收失败，不覆盖已有内容。

安全边界：对端是可信的，但越界写入不该由"信任"兜底 —— 相对路径里的
`..` 与绝对路径必须拒绝，符号链接不跟随（macOS 用 `lstat`）。

## 6. 显示切换（替代 SSH）

复用现有 `display-switch.ps1` 的成熟时序，仅把控制通道从 SSH 换成 HopDesk 连接：

- Windows：`dxva2.dll` 的 `SetVCPFeature(0x60, ...)` 写 DDC、`DisplaySwitch.exe /external|/extend`。
- macOS：
  - 唤醒：`IOPMAssertionCreateWithDescription`（IOKit，纯 C，等价 caffeinate）；
  - 切源：`posix_spawn` 调 `m1ddc`（不重写 DDC over IOKit）；
  - 睡眠：IOKit 或 `pmset displaysleepnow`。

切换瞬间屏幕断连、Mac 可能短暂掉线：连接层需重连，并把键鼠强制收回 Windows。

## 7. 连接与容错

- 客户端指数退避重连（1s、2s、4s… 上限 10s）。
- 双端每 2 秒发 PING，PING 收到后回 PONG；超过 6 秒未收到任何完整控制帧就断开，
  状态回退到本机控制。使用单调时钟，不受 NTP / 手动改时间影响；此阈值不包括调度延迟。
- 控制与数据握手均有 5 秒期限。Windows 控制 accept 与每个 peer 的读循环分离，
  新连接通过 token 校验后才能接替旧连接、回 ACK / 通知 UI。macOS 收到有效 ACK 后才显示已连接。
- 数据监听将候选连接握手与当前解码流分开，候选认证成功才替换旧连接；监听失败每 2 秒重试。
  控制通道断开时主动 shutdown 数据连接，避免失效数据会话长期占槽。
- 每个本地数据连接有 epoch，发送线程快照与每帧写入前比对，避免旧作业写进已复用的 socket。
  当前数据协议 v8 以控制握手生成的 128 位 session 绑定数据通道，并随每个非 HELLO 数据帧校验。
  控制替换时 compare-and-clear 防止旧读线程清掉新会话；临时目录按 session/batch 隔离。
- 控制队列软上限 2048、硬上限 4096、载荷字节上限 16 MB；软限后只丢移动/滚轮/心跳。
  关键事件无法入队或分配失败就断开复位，不静默丢弃释放事件。控制 socket 单次发送超时 3 秒。
- 解码器遇到过大载荷或分配失败进入粘滞失败，调用方断开连接；不尝试在未知边界上重新同步。
- Windows DDC 与剪贴板图片/文件处理移至后台；远端剪贴板写入通过窗口消息回主线程。
  基于最近系统输入与钩子回调时间做尽力重装，空闲时不反复装钩；单独某个钩子丢失仍可能漏检。
- 正常交还时 Windows 补发仍按着的按键与鼠标松开；macOS 断线时为普通键、修饰键和按钮发释放事件。
- 日志由锁串行写入，约 5 MB 轮转，保留当前文件与 `.1`。可选计划任务 / LaunchAgent 负责登录启动与
  异常退出重启，正常菜单退出不拉起。进程内看门狗将关键阻塞超期转为退出码 70，
  无清理、无致命路径日志、无线程强制取消；主动安装守护后才能自动拉起。
- **单实例**：Windows 用命名互斥量 `Local\HopDesk.SingleInstance`，第二个实例弹提示后退出；
  macOS 用 `flock` 锁 `$TMPDIR/hopdesk.lock`。锁由内核在进程退出时释放，
  **崩溃也不会留下僵尸锁**（pidfile 方案就得判断"那个 pid 还活着吗、是不是被复用了"）。
  两个实例的后果不是"多占点内存"：Windows 端会各自 bind 一次端口，
  macOS 端会各自连同一个数据通道、把对方的客户端槽位顶掉，键鼠和剪贴板来回跳。
- **端口独占**：Windows 的监听 socket 用 `SO_EXCLUSIVEADDRUSE`，**不是** `SO_REUSEADDR`。
  两者在 Windows 上语义相反 —— `SO_REUSEADDR` 允许**别的进程**绑同一端口并把连接抢走
  （Unix 上它只是允许绕过 TIME_WAIT，没有这个坑）。抢连接的症状是
  "对端连上了，但键鼠时灵时不灵"，日志里完全看不出来。独占之后后来者 bind 直接
  失败（`WSAEACCES`），错误明确。实测杀掉进程后立即重启仍能正常绑定，不受 TIME_WAIT 影响。

## 8. 资源与构建

- 目标：每端常驻内存 < 5 MB、可执行文件 < 1 MB、零运行时依赖。
- Windows：clang（llvm-mingw UCRT）单命令，注意 `-Wl,--entry=WinMainCRTStartup`
  （clang 的 `-municode` 只定义宏、不切 CRT 入口）；macOS：clang 单命令。
- 不引入 CMake 与第三方 GUI 框架，保持体积与可审计性。


## 9. 状态所有权与挂起隔离

| 状态 | 所有者 / 同步边界 |
|---|---|
| 控制 session | 共享 Core session registry；平台连接层在锁内快照并 compare-and-clear 自己的 epoch/session |
| Clipboard Offer / Request / Paste 事务 | `xfer_clipboard_txn`；平台锁负责串行调用，平台 Adapter 执行原生剪贴板读写与异步调度 |
| Sender Job / Queue / Active marker | 平台 Job 持有实际资源；共享 Queue/Scheduler/Active Job/Job Lifecycle 操作借用视图与转移规则；平台锁保护所有权转移 |
| Sender ACK / checkpoint | 平台 ACK Coordinator 持有并加锁；共享 ACK Window/ACK Wait/Engine 决定等待与推进规则 |
| Receiver Batch/File/Image 状态 | Receiver 线程持有 `xfer_rx_context`；共享 Frame Gate/Dispatch/Rules/State 模块执行平台无关决策 |
| Receiver session/tombstone/DONE receipt | Receiver lifecycle / receipt；新 session 使旧 tombstone/receipt 失效，旧帧不得污染新 Batch |
| Writer Queue / Buffer Pool | 平台 Writer 持有锁、线程和文件句柄；共享 RX Queue/Buffer Pool 计算有界 slot/bytes 与 buffer 额度 |
| Task / Reservation / 全局预算 | Task 持有 Reservation handle，Budget 是账本；调用方负责同步，handle 不得复制或重复释放 |
| Windows UI/输入状态 | 窗口线程；后台剪贴板结果通过窗口消息提交 |
| macOS socket/UI 状态 | 网络模块与 Cocoa 主队列分别持有；通过快照/主队列消息交接 |

平台连接循环仍分开实现，不宣称已实现单一跨平台 actor 状态机。数据连接还有本地 epoch，与线上 session 一起防止旧 socket / 旧帧污染。接收目录使用 session/batch 命名；异常退出残留目录不自动删除，因为成功剪贴板路径也可能仍被使用。

## 10. 保留的产品与验证边界

- **可信 LAN 安全取舍**：暂缓 TLS、复杂配对和密钥轮换。非空 token 仍以明文传输；
  空 token 不通过认证。session 负责关联与过期隔离，不提供抗窃听/冒充保证；
  CRC 也不是防篡改机制。保留版本/长度/路径校验，不暴露公网或不可信 Wi-Fi。
- **产品范围**：固定 Windows 主控 / Mac 被控、静态地址、有限屏幕布局，适合双设备自用；
  自动发现、多设备漫游、持久化队列和跨重启续传仍未实现。
- **验证范围**：协议单测、生产函数 fixture、故障注入与交叉编译不能替代原生 Windows/macOS
  实机测试。仍需确认辅助功能权限、睡眠唤醒、断网/换 IP、DDC、剪贴板竞争、大文件和守护安装/重启。
