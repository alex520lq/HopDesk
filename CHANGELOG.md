# Changelog

HopDesk 采用基于 GitHub Releases 的双通道版本发布体系：

- **稳定版（`stable`）**：正式发布标签 `vMAJOR.MINOR.PATCH`（例如 `v0.1.0`），更新清单位于 `HopDesk-Releases` 的 `stable.txt`（并同步 `latest.txt`）。
- **开发版（`dev`）**：预览/测试标签 `vMAJOR.MINOR.PATCH-dev.N`（例如 `v0.2.0-dev.1`），更新清单位于 `HopDesk-Releases` 的 `dev.txt`，发布为 GitHub Pre-release，与稳定版互不干扰，客户端可自由双向切换。

## [v0.1.0] - 2026-11-28

### 亮点
- 版本号体系重置为 `0.1.0`，建立 `VERSION` 单一事实源与 `CHANGELOG.md` 自动生成 GitHub Release 更新日志流水线。
- 新增 **稳定版 / 开发版双通道更新器**：两端独立发布与校验，支持从开发版回切至稳定版，或从稳定版切入开发版。
- Windows 与 macOS（含双进程 `--ui` 模式）统一升级为 **界面内三段式更新流程**：检查更新 → 后台下载并校验 SHA-256 → 一键重启安装并自动回滚保护。
- macOS 更新脚本自动检测并保留本机固定代码签名证书（如 `HopDesk Local Signing`），避免更新后丢失辅助功能与输入监控权限。
- 界面、CLI 与更新清单外置版本号、发布通道及控制/数据协议版本（`ctrl v8 / xfer v8`），协议变更或不匹配时直观提示。
- 包含已验证的跨端键鼠共享、双向剪贴板（文本/图片/文件按需粘贴）、系统音频转发以及参数化共屏一键切换（DDC/CI + 拓扑切换）。
