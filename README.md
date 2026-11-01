# HopDesk

**HopDesk 是一个面向 Windows 与 macOS 的轻量级跨设备协作工具。**

它让两台电脑共用键盘、鼠标和剪贴板，并可通过快捷菜单控制显示器输入源。项目使用共享的纯 C 核心，分别调用 Windows Win32 与 macOS 原生 API，不依赖 Qt、Electron 等大型 GUI 框架。

> 当前项目处于开发阶段。仓库包含两端的核心功能实现，但尚不代表已经完成全面的稳定性、安全性和跨设备兼容性验证。

## 功能

| 功能 | 当前实现 |
|---|---|
| 键鼠共享 | Windows 捕获鼠标与键盘事件，macOS 注入事件 |
| 鼠标跨屏 | 支持边缘移交、反向收回、相对位移、点击和滚轮 |
| 键盘映射 | Windows 虚拟键码映射到 macOS 键码；Ctrl 等修饰键按映射规则转发 |
| 剪贴板文本 | 双向文本同步，并抑制同步回环 |
| 文件与文件夹 | 通过独立数据通道传输，支持目录递归和空目录 |
| 图片 | 通过数据通道传输像素数据 |
| 显示器控制 | Windows DDC/CI 输入源控制；macOS 唤醒、切换输入源和显示睡眠 |
| 连接管理 | TCP 连接、心跳与客户端重连 |
| 原生界面 | Windows 系统托盘菜单；macOS 菜单栏状态项 |

### 当前边界

- 目标场景是两台设备之间的局域网连接，不提供多设备组网或公网中继。
- Windows 作为服务端，macOS 作为主动连接的客户端。
- 当前协议使用普通 TCP；没有实现 TLS 加密。
- 连接 token 默认为空，即默认不进行 token 校验。需要在可信网络中使用，或自行配置 token。
- 显示器 DDC 输入源代码值、macOS 切源命令与具体显示器和接线有关，需要按设备调整。
- 图片支持受两端当前剪贴板格式处理能力限制，并非通用图片格式转换器。

## 项目结构

```text
HopDesk/
├── src/
│   ├── core/          # 平台无关的 C 核心
│   │   ├── hop.h      # 协议、配置与公共定义
│   │   ├── protocol.c # TCP 帧编解码
│   │   ├── config.c   # 配置加载
│   │   ├── keymap.c   # Windows 到 macOS 键码映射
│   │   └── log.c      # 日志
│   ├── win/           # Windows：C + Win32
│   └── mac/           # macOS：C + 少量 Objective-C
├── scripts/
│   ├── build-win.ps1  # Windows 构建脚本
│   └── build-mac.sh   # macOS 构建脚本
├── docs/
│   └── architecture.md
└── LICENSE
```

### 平台实现

**Windows**

- Win32 托盘与消息窗口
- `WH_MOUSE_LL` / `WH_KEYBOARD_LL` 输入捕获
- `AddClipboardFormatListener` 剪贴板监听
- Winsock TCP 服务端
- `dxva2.dll` DDC/CI 显示器控制

**macOS**

- CoreGraphics `CGEvent` 键鼠事件注入
- POSIX socket TCP 客户端
- `NSPasteboard` 剪贴板读写
- `NSStatusItem` 菜单栏界面
- IOKit 显示唤醒，以及外部命令切换显示器输入源

## 通信设计

HopDesk 使用两条独立 TCP 连接，避免文件和图片传输阻塞实时键鼠控制。

| 通道 | 默认端口 | 内容 |
|---|---:|---|
| 控制通道 | `24817` | 握手、心跳、键鼠事件、文本剪贴板、显示控制 |
| 数据通道 | `24818` | 文件、文件夹与图片数据 |

数据通道采用分块传输，默认分块大小为 256 KB。文件从磁盘分块读取和写入，不需要把整个文件一次性载入内存。控制通道和数据通道分别使用独立 socket。

协议帧格式、消息类型、连接状态和线程模型见 [架构说明](docs/architecture.md)。

## 构建

### Windows

需要 PowerShell 7 或 Windows PowerShell，以及 clang 工具链。推荐
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)：clang + lld + 完整的 mingw-w64
sysroot，自带 CRT，不依赖 VS 或 MSYS2。

```powershell
winget install MartinStorsjo.LLVM-MinGW.UCRT
```

```powershell
pwsh scripts/build-win.ps1
```

脚本输出：

```text
build/hopdesk.exe
```

默认用 clang —— 与 macOS 端同一个编译器前端，`-Wall -Wextra` 的警告语义一致，不会出现
"Mac 上没警告、Windows 上一堆"的情况。脚本按 PATH 上的 `clang.exe` → winget 安装的
llvm-mingw → w64devkit 的顺序探测，都找不到才回落到 gcc。也可以显式指定：

```powershell
pwsh scripts/build-win.ps1 -Compiler clang
pwsh scripts/build-win.ps1 -Compiler gcc
pwsh scripts/build-win.ps1 -Compiler msvc
```

**一个必须显式处理的坑**：clang 的 MinGW driver 接受 `-municode`，但它只定义
`UNICODE` / `_UNICODE`，**不会切换 CRT 入口**（gcc 的 spec 会）。若不额外加
`-Wl,--entry=WinMainCRTStartup`，入口会落到 `__tmainCRTStartup`，`__mingw_app_type`
保持 0（console），与 GUI 语义不符。脚本里已固定该参数。

### macOS

需要 macOS、Xcode Command Line Tools 和 Apple Clang。

```sh
sh scripts/build-mac.sh
```

脚本输出：

```text
build/hopdesk
```

首次使用键鼠注入时，需要在 macOS「系统设置 → 隐私与安全性 → 辅助功能」中为 HopDesk 授权。

## 运行

- **每台设备只允许一个实例**。Windows 端第二次启动会弹提示后退出（互斥量
  `Local\HopDesk.SingleInstance`）；macOS 端静默退出（状态栏里已经有一个了）。
  两个实例同时跑不是"多占点内存"：Windows 端会各自 bind 一次端口，macOS 端会各自
  连同一个数据通道把对方顶掉，键鼠和剪贴板会来回跳，而且两端日志都看不出原因。
- **Windows 端端口是独占的**（`SO_EXCLUSIVEADDRUSE`）。24817 / 24818 被别的程序占着时，
  HopDesk 记录错误并每 2 秒重试；托盘提示端口不可用，不会和别人"共享"端口。这一点和 Unix 相反：
  Windows 的 `SO_REUSEADDR` 会让后来者绑上同一个端口并把连接抢走。

## 配置

两端通过 `hopdesk.conf` 加载 `key=value` 配置。以当前源码默认值为基础，可按实际网络和设备创建配置文件。

macOS 客户端至少需要设置 Windows 服务端地址：

```ini
# macOS 客户端：Windows 服务端地址
host=192.168.1.100

# 两端端口需要保持一致
port=24817
data_port=24818

# 可选：两端配置相同的 token 才能通过握手校验
# 不配置或留空表示不校验
token=

# 边缘移交设置
server_edge=0
client_edge=1
switch_delay_ms=250
switch_cooldown_ms=300
edge_margin_px=40

# 数据通道分块大小，单位 KB
xfer_chunk_kb=256
```

常用配置项：

| 配置项 | 默认值 | 说明 |
|---|---:|---|
| `host` | 空 | macOS 客户端连接的 Windows 服务端地址 |
| `port` | `24817` | 控制通道端口 |
| `data_port` | `24818` | 文件与图片数据通道端口 |
| `token` | 空 | 可选的共享 token；两端需一致 |
| `server_edge` | `0` | Windows 移交边缘：0=左，1=右 |
| `client_edge` | `1` | macOS 收回边缘 |
| `switch_delay_ms` | `250` | 鼠标贴边后持续多久才移交；0 表示立即移交 |
| `switch_cooldown_ms` | `300` | 移交或收回后的边缘冷却时间 |
| `edge_margin_px` | `40` | 边缘内移距离及收回越界阈值 |
| `xfer_chunk_kb` | `256` | 数据通道分块大小，代码范围为 64–4096 KB |

显示器相关配置包括 `disp_win_input`、`disp_mac_input`、`disp_wake_sec`、`mac_input_cmd` 和 `mac_sleep_cmd`。其中 DDC/CI 输入源值依赖显示器型号；macOS 默认切源命令使用 `m1ddc`，请确认命令已安装且适用于你的设备。

## 运行前检查

1. 确保 Windows 与 macOS 位于可互通的网络中。
2. 在 macOS 配置文件中填写 Windows 的局域网 IP。
3. 确保两端控制端口和数据端口一致，并允许对应 TCP 端口通信。
4. 首次运行 macOS 端前授予辅助功能权限。
5. 如果启用了 token，两端必须配置相同值。
6. 显示器切源功能需要按实际显示器输入源和接线配置。

**可靠性**（详细设计见 `docs/architecture.md`）：

- **控制通道认证后才上线**；5 秒内未完成握手即关闭。Windows 独立接受新连接，
  新连接认证通过后接替旧连接，未认证连接不能挤掉当前会话。
- **双端应用心跳**：每 2 秒发送 PING，超过 6 秒未收到完整控制帧就断开，
  回收远端按键/鼠标状态并重连；数据连接随控制断开主动重建。TCP keepalive 仅作兜底。
- **有界控制队列**：2048 条软上限后丢弃移动/滚轮等可丢帧；关键事件继续排队，
  达到 4096 条或 16 MB 上限就断开复位，不能静默丢掉 KEY_UP/按钮释放。
- **UI 线程减负**：Windows 剪贴板图片转换、文件目录枚举和 DDC 切源放到后台；
  剪贴板忙时用消息/计时器重试，不在低级钩子的主线程 Sleep。钩子恢复为尽力检测，
  不是能发现所有单独鼠标/键盘钩子失效的保证。
- **日志轮转**：单文件达到约 5 MB 后轮转为 `.1`，只保留当前与上一份，多线程写入串行化。

- **每块 CRC32 + 整文件 CRC**。整文件 CRC 是各块 CRC 再折一次，同时覆盖数据、
  块顺序和偏移，且不需要把 GB 级文件重读一遍。
- **单文件 ACK 闭环（数据协议 v3）**。每个文件发完由接收端回 `FILE_ACK`
  （`OK` / `CRC` / `DISK` / `BADPATH`）。失败只重发这一个文件，最多 3 次
  （200 / 400 / 800 ms 退避）；`BADPATH` 是确定性失败，直接放弃不重试。
  一个坏文件不再判死整批，`XFER_DONE` 里的 `ok` / `total` 就是精确的"N/M 成功"。
- **断线按会话重连重试**。仅数据连接重建时，从断点条目继续（已确认的不重传）；
  控制会话变化时重置确认游标、整批重发，不能沿用旧接收端的确认状态。
  退避 0.5–8 s，最多 5 次，总预算 30 s。超预算才放弃 —— 否则对端再也不回来时
  作业会永久挂在队列里。
- **可取消**。托盘 / 菜单栏「取消当前传输」双向生效：发送侧每块之后检查一次
  （响应延迟上限一个分块），接收侧删掉半成品并立墓碑，对端再发同批次直接回 `ABORT`。
- **有界写盘队列（8 槽）做背压**。磁盘跟不上时接收线程阻塞，TCP 窗口随之收缩，
  发送端自动减速 —— 内存上界是"队列深度 × 块大小"，不随传输总量增长。
  实测传 200 MB 峰值工作集 10 MB（1 MB 分块时 19 MB）。
- **落盘在独立线程**，文件句柄由写线程独占。剪贴板只在单文件 `FILE_ACK` 成功之后
  才登记，不会出现"粘贴出一个写了一半的文件"。

数据协议版本在握手时校验（当前控制/数据协议均为 v6）。数据通道 HELLO
携带协议版本、共享 token 和控制握手生成的 128 位 session ID；
所有非 HELLO 数据帧都带 `session(16) + batch(4)` 前缀，CHUNK/END 不再仅靠隐式活动文件状态。
旧会话或不匹配批次不能写入当前传输；临时文件按 session/batch 分目录，避免重启后批次号复用覆盖旧结果。
v6 与旧版不兼容：**Windows 与 macOS 必须一起升级**。版本、token 或 session 不匹配即拒绝连接。

注意：token 只用于身份校验，当前 TCP 数据仍未加密；空 token 代表显式关闭 token
校验。自己管理的可信局域网可暂缓 TLS/复杂配对，但 session ID 不是加密或防窃听机制。
不要暴露到公网/不可信 Wi-Fi；路径、长度、版本和 CRC 校验仍保留，不能因“自用”删掉内存与文件安全边界。

**还没做的**：多文件并发、断点续传。局域网单条 TCP 流已是瓶颈，多文件并发
不提高吞吐；断点续传要按 batch 持久化已完成偏移，只在跨不稳定链路时才值得做。

## 可选：登录自启与崩溃重启

这些脚本只随安装包提供，**不会自动修改系统**。先把程序安装到固定目录，再主动运行：

- Windows：在解压后的安装目录执行 `./install-autostart-windows.ps1`。
  源码目录运行时需显式传 `-Executable '完整路径\HopDesk.exe'`。
  当前用户登录后由计划任务启动 supervisor，异常退出后按 2–30 秒退避重启；
  托盘正常退出（退出码 0）不重启，直到下次登录。卸载用 `-Uninstall`。
- macOS：先运行 `install-macos.command` 安装 App，再运行安装包中的
  `install-launchagent.command`；默认启动 `~/Applications/HopDesk.app`（与现有安装器一致），也可传自定义 App 路径。
  创建当前用户的 `com.hopdesk.agent` LaunchAgent，异常退出由 launchd 重启，正常退出不重启。
  卸载用 `sh install-launchagent.command --uninstall`。

不要同时启用多套自启动方式；若已有实例，先正常退出再启动守护。安装脚本修改需在原生系统验证。
这是单机故障自恢复，不是多节点高可用。进程内看门狗监控主循环和关键阻塞操作：
DNS 5 秒、整帧发送 10 秒、常规文件/DDC/剪贴板操作 15 秒、写盘背压 20 秒、
主循环/启动 30 秒、单个接收帧处理 60 秒。超期直接以 70 退出，不在线程持锁时强行取消。
系统整体休眠/暂停会延长期限；不保证覆盖未纳入监控的调用、内核或整机故障。
**只有主动安装上述守护脚本后，异常退出才会自动重启**；否则需手动启动。
进程退出会丢失内存传输队列，重启后需重新复制；不会宣称跨重启续传。
异常退出可能留下暂存目录；成功剪贴板文件也在该目录中，因此不会冒险自动清空。

## 资源目标

项目以低资源占用为设计目标：

| 指标 | 设计目标 |
|---|---:|
| 常驻内存 | 每端低于 5 MB |
| 可执行文件 | 每端低于 1 MB |
| 运行时依赖 | 不依赖大型 GUI 框架或独立运行时 |

以上是项目目标，不是对所有构建版本和运行场景的实测保证。

## 文档

- [架构设计与通信协议](docs/architecture.md)
- [Windows 端说明](src/win/README.md)
- [macOS 端说明](src/mac/README.md)

## 许可证

[MIT](LICENSE)
