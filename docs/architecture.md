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

单条 TCP 长连接，复用键鼠、剪贴板、显示控制三类消息。

### 帧格式

```
+--------+----------+------------------+
| type   | length   | payload          |
| 1 byte | 4 bytes  | length bytes     |
+--------+----------+------------------+
```

- `length`：网络字节序（big-endian）的 uint32。
- 文本 payload：UTF-8。

### 消息类型

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

- Windows：`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`，读 `CF_UNICODETEXT`。
- macOS：`NSPasteboard`（ObjC 薄层），轮询 `changeCount` 或用变更通知。
- 阶段 3 只做文本；图片（CF_DIB / NSPasteboard PNG）、文件（CF_HDROP / file URL）后续扩展。

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
- 单实例：Windows 命名 Mutex / macOS `flock`，防止两个实例冲突。

## 8. 资源与构建

- 目标：每端常驻内存 < 5 MB、可执行文件 < 1 MB、零运行时依赖。
- Windows：MSVC `/MT` 静态链接或 MinGW；macOS：clang 单命令。
- 不引入 CMake 与第三方 GUI 框架，保持体积与可审计性。
