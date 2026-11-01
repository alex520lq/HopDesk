# HopDesk 架构设计

## 1. 角色与拓扑

- **Windows 端 = 服务端（Server）**：键鼠的物理主控方，监听 TCP 端口，负责捕获本机输入、边缘检测、剪贴板监听、DDC 显示控制。
- **macOS 端 = 客户端（Client）**：主动连接 Windows，接收事件并用系统 API 注入，提供唤醒 / 切源 / 睡眠等能力。

固定双设备、局域网（实测 RTT ~1ms）。不做多设备发现与多屏布局，配置文件写死两端屏幕几何。

## 2. 模块划分

```
src/
├── core/    纯 C、平台无关：协议帧、配置、日志、连接状态机、键码映射表
├── win/     Windows：Win32 托盘/弹窗、Raw Input、剪贴板、dxva2 DDC、Winsock
└── mac/     macOS：CGEvent 注入（C）、IOKit 唤醒（C）、NSPasteboard 与
             NSStatusItem（少量 ObjC）、POSIX socket
```

平台层只实现 `core` 定义的接口（输入捕获/注入、剪贴板读写、显示控制、UI 回调），
所有跨端逻辑（状态机、协议、时序）收敛在 `core`，保证两端行为一致。

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
连接建立后双方都发送 `XFER_HELLO`，载荷为 `version(1) + token_length(1) + token`。
收到 HELLO 的一端必须同时验证协议版本、长度和 token；验证失败立即关闭 socket。
握手成功前收到任何其他数据帧也会关闭连接。两端配置的 token 必须完全一致。

| 类型 | 方向 | 载荷 |
|---|---|---|
| XFER_HELLO | 双向 | `ver(1) token_len(1) token[token_len]`；两端均校验，成功前拒绝其他数据帧 |
| FILE_BEGIN | 双向 | `batch(4) id(4) size(8) mtime(8) flags(1) nlen(2) name plen(2) relpath` |
| FILE_CHUNK | 双向 | `id(4) off(8) len(4) crc(4) data[len]` |
| FILE_END | 双向 | `id(4) status(1) file_crc(4)` |
| FILE_ACK | 双向 | `batch(4) id(4) status(1)` |
| IMAGE_BEGIN | 双向 | `batch(4) id(4) w(4) h(4) stride(4)` |
| XFER_DONE | 双向 | `batch(4) ok(4) total(4) bytes(8)` |
| XFER_ABORT | 双向 | `batch(4) reason(1)` |

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
| 作业级重试 | 链路断了整批退回队尾，`next_i` 游标保留 —— 重连后从断点条目继续，已确认的不重传。退避 0.5–8 s，最多 5 次，总预算 `HOP_XFER_RECONNECT_BUDGET_MS = 30 s` |
| 取消 | 菜单触发后置 `cancel_batch`，发送线程每块之后检查（延迟上限一个分块），`ack_wait` 里也检查，否则会干等满 10 s 的 ACK 超时。接收侧由 conn/listen 线程消费 `cancel_rx`：删半成品、立墓碑，对端再发同批次直接回 `ABORT(CANCEL)` |
| 背压 | 写盘队列有界（`RX_Q_SLOTS = 8`）。队列满 → 接收线程阻塞 → `recv` 停止 → TCP 窗口收缩 → 发送端自动减速。内存上界 = 队列深度 × 块大小，与传输总量无关 |
| 磁盘 I/O 隔离 | 网络线程只做"收包 + 校验 + 入队"，落盘交给独立写线程（同样降级优先级）。文件句柄由写线程独占，避免关文件与写入的竞态 |
| 尺寸边界 | `FILE_CHUNK` 超出 `FILE_BEGIN` 声明的 size 直接丢弃；`IMAGE_BEGIN` 的 w/h/stride 全部校验，上限 `32768` 边长 / `256 MB` |
| 发送互斥 | 接收线程出错时要回 `XFER_ABORT`，和发送线程写同一个 socket。锁粒度是**整帧**，不是单次 `send`/`write`，否则两线程交错会把帧字节搅乱。配套设 `SO_SNDTIMEO = 5 s`，避免发送线程卡死时把接收线程一起拖住 |
| 数据通道身份校验 | v4 HELLO 双向携带协议版本与共享 token；两端校验成功前不接受文件/图片帧、不报告数据通道可用。空 token 按配置语义跳过身份校验；token 不加密，TCP 载荷也未启用 TLS |
| 文件 CRC fold | v5 将每块的 64 位 offset 与 chunk CRC 一起折入整文件 CRC；块顺序或位置改变都会改变最终校验值 |

**尚未实现**：

- **多文件并发**：单连接内文件是串行的。局域网下单条 TCP 流已是瓶颈（千兆
  11.5 MB/s 的线速远低于磁盘），并发不会提高吞吐；真要提速应该做"并行读盘 +
  单一发送队列"，而不是多连接。
- **断点续传**：连接断开后重传整批，没有 chunk 位图持久化。GB 级数据在稳定
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

接收端落盘到 `%TEMP%\HopDesk\<batch>\` / `$TMPDIR/HopDesk/<batch>/`，
保留相对路径，然后把**顶层条目**挂到剪贴板，用户直接 Ctrl+V / Cmd+V。

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
- 心跳超时（连续 3 次 PING 无 PONG）判定断线，状态回退到"本机控制"。
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
