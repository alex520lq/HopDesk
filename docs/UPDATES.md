# HopDesk 自动更新与双通道 Release 发布

## 设计

HopDesk 主仓库 `alex520lq/HopDesk` 为公开仓库，直接复用单仓库 GitHub Releases 与内置 `GITHUB_TOKEN` 发布安装包、更新日志与双通道更新清单（无需额外创建第二个仓库或手动申请 PAT）：

- 源码、CI 与 GitHub Releases：`alex520lq/HopDesk`（Public）
- 稳定版清单：`https://raw.githubusercontent.com/alex520lq/HopDesk/updates/stable.txt`（`format=2`，同时向 `updates` 分支 `latest.txt` 写入兼容旧客户端的 `format=1` 清单）
- 开发版清单：`https://raw.githubusercontent.com/alex520lq/HopDesk/updates/dev.txt`

清单指针存放在同仓库独立的 orphan `updates` 分支，不污染 `main` 分支的提交历史，也不会触发多余 CI 构建。

### 双通道与跨通道切换规则

- **稳定版（`stable`）**：标签格式 `vMAJOR.MINOR.PATCH`（例如 `v0.1.0`、`v1.4.0`），发布为正式 GitHub Release，写入 `stable.txt` 与 `latest.txt`。
- **开发版（`dev`）**：标签格式 `vMAJOR.MINOR.PATCH-dev.N`（例如 `v0.2.0-dev.1`、`v1.4.1-dev.1`），发布为 GitHub Pre-release，仅写入 `dev.txt`，绝不污染稳定版指针。
- **同通道防回退**：在同一通道内只允许向更高版本升级（严格 SemVer 比较，`0.2.0-dev.1 < 0.2.0-dev.2 < 0.2.0`）。
- **跨通道自由切换**：用户在客户端界面“设置 → 更新与日志”中选择另一个通道并点击“检查更新”时，允许双向切换——例如当前运行 `dev v1.4.1-dev.1` 可以直接切回 `stable v1.4.0`，或从 `stable v0.1.0` 切入 `dev v0.2.0-dev.1`。

### 严格清单格式（`format=2`）

```ini
format=2
channel=stable
version=0.1.0
tag=v0.1.0
ctrl_proto=8
xfer_proto=8
windows_url=https://github.com/alex520lq/HopDesk/releases/download/v0.1.0/HopDesk-Windows.zip
windows_sha256=<64-hex>
macos_url=https://github.com/alex520lq/HopDesk/releases/download/v0.1.0/HopDesk-macOS.zip
macos_sha256=<64-hex>
notes_url=https://github.com/alex520lq/HopDesk/releases/tag/v0.1.0
summary=<单行中文摘要>
```

客户端兼容旧版 `format=1` 清单（视作 `stable`），并严格校验通道与版本号匹配、固定 GitHub Release 路径、64 位 SHA-256 及协议版本号；任何未知键、重复键或篡改 URL 均拒绝。

## 发布新版本

版本号以根目录 `VERSION` 为单一事实源，更新日志以 `CHANGELOG.md` 的 `## [v<version>]` 章节为准。发布 job 使用 GitHub Actions 内置 `GITHUB_TOKEN`（仅 `release` job 授予 `contents: write`），无需额外配置仓库或 Secret：

1. 更新 `VERSION` 与 `CHANGELOG.md`，并将代码合并到 `main`。
2. 基于目标 `main` 提交创建并推送 tag：
   - 稳定版：`git tag v0.1.0 <commit> && git push origin v0.1.0`
   - 开发版：`git tag v0.2.0-dev.1 <commit> && git push origin v0.2.0-dev.1`
3. GitHub Actions 在 Windows（MSVC）与 macOS runner 分别构建、运行全部核心/协议/TCP/更新测试并打包。
4. 发布 job 使用 `scripts/release_notes.py` 从 `CHANGELOG.md` 和 `src/core/hop.h` 提取更新日志、协议版本与摘要，生成 `format=2` 清单，直接发布到 `alex520lq/HopDesk` 的 GitHub Releases（`dev` 自动标记为 Pre-release），并按单调策略更新同仓库 `updates` 分支的对应通道指针（`stable.txt` + `latest.txt` 或 `dev.txt`）。

每个 tag 都是不可变资产。重跑仅允许哈希等字段完全一致；通道指针按各自通道单调推进，且拒绝跨通道覆盖。

## 界面内三段式更新流程（Windows & macOS 一致）

两端均由后台引擎统一持有更新状态机（共享 C Core `src/core/update_controller.c`），主窗口展示当前版本、通道和协议版本（`vX.Y.Z · 控制 vN / 数据 vN`）并与目标版本直观对比：

1. **检查更新**：按所选通道异步拉取清单，展示目标版本、协议是否变更、是否为跨通道切换、更新摘要以及发布页链接。
2. **下载更新**：后台流式下载安装包并显示实时进度，下载完成后自动校验 SHA-256。下载期间完全不中断正常键鼠/剪贴板/音频工作。
3. **重启并安装更新**：用户主动点击后，关闭新传输准入并确认当前无活跃传输，再移交独立更新脚本完成替换、就绪回执验证（nonce + version + PID）及 5 秒存活观察；失败自动回滚并重启旧版本。

### macOS 固定签名保留与权限

`scripts/update-macos.command` 在替换已安装的 `HopDesk.app` 前，会自动检测目标 App 的代码签名 designated requirement（`certificate leaf = H"<40位哈希>"`）。若已安装副本使用本机固定证书（例如 `HopDesk Local Signing`）或 Apple Developer ID 签名，更新脚本会自动使用同一证书对新 App 重签并执行 `codesign --verify --deep --strict`，从而在下载更新并自动重启后保留 macOS TCC 的辅助功能与输入监控权限。若旧副本仅为 ad-hoc 签名，则保持默认行为。

双进程模式（`--agent` + `--ui`）同样完整支持界面内三段式更新：更新动作通过本地 IPC 交由后台 Agent 执行，确认准备就绪后 Agent 安全退出交棒给更新脚本重启新版本，旧 UI 窗口同步自动关闭。

### 传输互斥与回执边界

下载期间不占用安装锁。真正点击“重启并安装更新”时，两端统一调用 `hop_update_install_begin(xfer_busy)` 关闭新任务准入并检查活跃传输；若传输忙碌则保持已下载包就绪，用户可在传输结束后直接点击重启安装而无需重新下载。

## 回归验证

- `tests/core_tests.c`：`format=1`/`format=2` 清单解析、`-dev.N` SemVer 比较、同通道防回退、跨通道双向切换（含 `dev 1.4.1-dev.1 -> stable 1.4.0`）、`hop_update_snapshot` 状态机及 `update_channel` 配置项测试。
- `scripts/test-updates.py`：共享准入锁、就绪回执、双通道发布策略防回退/防跨通道污染、`release_notes.py` 日志提取，以及 POSIX 下生产 `update-macos.command`（含 `-dev.1` 版本与固定证书重签保留）全路径回归。
- `scripts/test-update-windows.ps1`：Windows runner 上编译真实 PE 夹具验证生产 `update-windows.ps1` 替换与回滚。
