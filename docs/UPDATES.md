# HopDesk 自动更新与 Release 发布

## 设计

源码仓库 alex520lq/HopDesk 保持私有。桌面客户端不能安全地把私有仓库访问令牌内置在程序里，因此更新包和公开更新清单放在独立的公开仓库：

- 源码与 CI：alex520lq/HopDesk（Private）
- 更新包与清单：alex520lq/HopDesk-Releases（Public）
- 清单地址：https://raw.githubusercontent.com/alex520lq/HopDesk-Releases/main/latest.txt

更新清单采用严格的 key=value 格式。客户端只接受固定 Release 仓库、固定资产路径、MAJOR.MINOR.PATCH 版本号和 64 位 SHA-256；不会执行清单中任意命令或任意下载地址。

## 一次性 GitHub 配置

自动发布依赖一个公开 Release 仓库和一个最小权限的访问令牌：

1. 在 GitHub 创建公开仓库 alex520lq/HopDesk-Releases，初始化 main 分支（例如勾选 Add a README）。该仓库只放发布包、Release 和 latest.txt，不要复制私有源码。
2. 创建 Fine-grained personal access token，只授权 HopDesk-Releases 这一个仓库的 Contents: Read and write 权限。
3. 打开私有源码仓库 HopDesk → Settings → Secrets and variables → Actions → New repository secret。
4. 将 secret 命名为 HOPDESK_RELEASE_TOKEN，值填写上一步生成的 token。

没有公开 Release 仓库或缺少 secret 时，普通 PR/main 双平台构建仍可运行，但 tag 发布步骤会明确失败；客户端会显示暂时无法读取更新清单。

## 发布新版本

版本必须使用 vMAJOR.MINOR.PATCH 格式，例如 v0.5.1：

1. 将准备发布的代码合并到私有仓库 main。
2. 基于要发布的 main 提交创建并推送 tag：git tag v0.5.1 <commit>、git push origin v0.5.1。
3. GitHub Actions 在 Windows 与 macOS runner 分别构建、运行核心/协议/TCP 测试并打包。
4. 发布 job 计算两个 ZIP 的 SHA-256，生成 latest.txt，把两个平台安装包和清单发布到公开 Release 仓库，并更新其 main/latest.txt。
5. 用户在 Windows 托盘或 macOS 菜单栏点击“检查更新…”，确认后下载、校验、安装并重新启动。

每个 tag 都是不可变的版本资产。发布 job 串行执行，上传资产前先检查已存在的同 tag 清单：只允许两个 ZIP 的哈希等字段完全一致的重跑；已存在但缺失清单、内容变化或查询失败时拒绝覆盖，需要人工核对或发布新版本号。

公开 main/latest.txt 按数字版本比较，只向更高版本推进。旧 tag 晚完成或重跑可以保留历史 Release，但不会把更新入口降级；同版本清单变化及畸形现有清单失败关闭。公开 Release 仓库应只由该串行 job 写入清单，勿并行人工改写。

## 客户端安装行为

### Windows

- 使用 WinHTTP 通过 HTTPS 获取清单和 ZIP。
- 下载时流式计算 SHA-256，限制包大小为 512 MB。
- 只接受 GitHub 固定 Release 仓库的资产地址。
- 独立 PowerShell 更新辅助程序先检查 ZIP 内文件名白名单，拒绝目录穿越和符号链接，并在目标目录准备临时 EXE。辅助程序回执准备成功后，旧进程才被要求退出；辅助程序启动后不再等待用户点击成功提示框。
- 先写入同目录临时 EXE，再备份旧 EXE，替换失败时恢复备份；配置文件和日志不覆盖。
- 启动新 EXE，等待与事务 nonce、预期版本及实际子进程 PID 绑定的 ready 回执，随后观察 5 秒。45 秒内未就绪、启动失败或观察期退出时，停止新进程、恢复并重启旧 EXE；只有确认后才删除备份。

### macOS

- 使用 NSURLSession 异步获取清单和 ZIP，不阻塞菜单栏主线程。
- 流式计算 SHA-256，并限制下载包大小为 512 MB。
- 检查新 App 的 CFBundleIdentifier 必须是 com.hopdesk.app、CFBundleShortVersionString 等于目标版本，并运行 codesign --verify。
- 独立脚本等待当前进程退出，在同一目录准备新 App，备份旧 App 后原子替换；失败时恢复备份。
- 直接启动 bundle 内可执行文件以准确绑定 PID/环境；等待 45 秒内的 nonce/版本/PID 就绪回执及 5 秒存活观察，才删除旧 App。失败时终止新进程、恢复旧 App 并用 open 重启。~/Library/Application Support/HopDesk 中的配置和日志不会被替换。

### 传输互斥与回执边界

下载和用户确认期间不占用安装锁。真正安装前，两端统一关闭文件/图片提交和接收帧的新任务准入，再检查已有扫描预留、队列及发送/接收状态；任一忙碌则取消安装并恢复准入。已取得准入的扫描/提交/接收回调与安装切换互斥，避免“检查时空闲、下载后却截断新传输”。安装维护期间收到新数据帧会断开数据连接，交由原有重连/重试处理，不静默接收后丢弃。

准备失败时旧进程继续运行；准备成功后辅助程序最多等待旧进程退出 60 秒。回执来自初始化完成后的 UI 定时器，证明该版本已进入事件循环，不证明双机连接、剪贴板权限或所有业务路径正常。回滚若因权限/磁盘错误也失败，会保留带事务后缀的 previous 备份，并记录恢复路径到临时目录的 HopDesk-update-error.log。

新事务协议需要当前安装的程序与辅助脚本配套；首次采用本版本应完整安装包更新，不能仅替换 EXE 或混用旧辅助脚本。

## 回归验证

- scripts/test-updates.py：真实共享 C 准入锁并发、回执版本/nonce/PID、清单防回退及同版本不可变测试；POSIX 下运行生产 macOS 脚本，使用 OS 命令替身并执行真实文件移动/子进程，覆盖正常、崩溃、无回执、错误 nonce、回执后早退、替换失败及旧进程不退出。
- scripts/test-update-windows.ps1：在 Windows runner 编译真实 PE 夹具，用生产 powershell.exe 辅助程序验证正常启动、立即失败、错误 nonce、回执后早退的替换/回滚。接入 test-core.ps1；HA 传输测试同时链接新的共享准入模块。
- 这些测试不替代 Windows/macOS 完整原生构建和真机更新验收；发布 job 的实际 GitHub 网络/凭据/重跑测试需单独验收，不因纯策略测试通过即视为发布成功。

## 当前安全边界与发布前要求

当前自动更新具备 HTTPS、固定来源校验、严格清单解析、SHA-256 完整性校验、受限 ZIP 内容、应用标识检查和失败回滚。但 SHA-256 与清单来自同一个公开 Release 服务，它不能抵御 Release 仓库账号或发布凭据被攻陷。

当前 macOS 构建使用 ad-hoc code signing；当前 Windows 构建没有 Authenticode 签名。因此 codesign --verify 仅验证签名结构，不证明发布者身份。在向不受信任的外部用户公开分发前，必须配置 Windows Authenticode 证书、Apple Developer ID 签名与 notarization，并在更新器中固定/验证发布者身份。不得把当前 SHA-256 校验描述为独立的数字签名验证。

当前安装方式是便携目录/用户目录更新。Windows 安装在受管理员保护的目录或 macOS 安装在不可写的 /Applications 时，更新可能因权限不足而失败；旧版本会保留，用户需将应用安装在当前用户可写的位置或手动处理权限。
