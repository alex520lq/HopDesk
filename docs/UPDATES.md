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

每个 tag 都是不可变的版本资产；latest.txt 只指向最近一次成功发布的版本。

## 客户端安装行为

### Windows

- 使用 WinHTTP 通过 HTTPS 获取清单和 ZIP。
- 下载时流式计算 SHA-256，限制包大小为 512 MB。
- 只接受 GitHub 固定 Release 仓库的资产地址。
- 独立 PowerShell 更新辅助程序等待 HopDesk 退出后检查 ZIP 内文件名白名单，拒绝目录穿越和符号链接。
- 先写入同目录临时 EXE，再备份旧 EXE，替换失败时恢复备份；配置文件和日志不覆盖。
- 安装成功后重新启动 HopDesk。

### macOS

- 使用 NSURLSession 异步获取清单和 ZIP，不阻塞菜单栏主线程。
- 流式计算 SHA-256，并限制下载包大小为 512 MB。
- 检查新 App 的 CFBundleIdentifier 必须是 com.hopdesk.app，并运行 codesign --verify。
- 独立脚本等待当前进程退出，在同一目录准备新 App，备份旧 App 后原子替换；失败时恢复备份。
- 使用 open 重新启动应用。~/Library/Application Support/HopDesk 中的配置和日志不会被替换。

## 当前安全边界与发布前要求

当前自动更新具备 HTTPS、固定来源校验、严格清单解析、SHA-256 完整性校验、受限 ZIP 内容、应用标识检查和失败回滚。但 SHA-256 与清单来自同一个公开 Release 服务，它不能抵御 Release 仓库账号或发布凭据被攻陷。

当前 macOS 构建使用 ad-hoc code signing；当前 Windows 构建没有 Authenticode 签名。因此 codesign --verify 仅验证签名结构，不证明发布者身份。在向不受信任的外部用户公开分发前，必须配置 Windows Authenticode 证书、Apple Developer ID 签名与 notarization，并在更新器中固定/验证发布者身份。不得把当前 SHA-256 校验描述为独立的数字签名验证。

当前安装方式是便携目录/用户目录更新。Windows 安装在受管理员保护的目录或 macOS 安装在不可写的 /Applications 时，更新可能因权限不足而失败；旧版本会保留，用户需将应用安装在当前用户可写的位置或手动处理权限。
