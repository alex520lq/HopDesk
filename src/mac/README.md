# mac —— macOS 端（C 核心 + 少量 ObjC）

支持服务端与客户端角色。

- 键鼠注入：`CGEventCreate*` + `CGEventPost`（需要辅助功能及事件投递权限）
- 本地跨端文件粘贴：`CGEventTap` 监听 Command+V（需要辅助功能及输入监控权限）；首次启动会请求权限并打开对应设置页。访达作为当前 App 时，HopDesk 在按键时记录目标文件夹，接收端直接在该文件夹创建源文件/目录，传输中可见；读取普通访达窗口目标需要一次性“自动化：控制访达”授权。访达桌面直接使用桌面目录。无法取得目标位置时沿用暂存后再次粘贴的流程。
- 显示唤醒：`IOPMAssertionCreateWithDescription`（纯 C IOKit）
- 切源：`posix_spawn` 调 `m1ddc`
- 剪贴板：`NSPasteboard`（ObjC 薄层）
- 状态栏：`NSStatusItem`（ObjC）
- 网络：POSIX socket 客户端
- 单实例：`flock`

构建：`sh scripts/build-mac.sh`（clang，需 Xcode Command Line Tools），产物为 `build/HopDesk.app` 和 `build/HopDesk-macOS.zip`。
本地构建默认使用 ad-hoc 签名，重建后 macOS 可能要求重新授权。安装版可通过 `HOPDESK_CODESIGN_IDENTITY` 指定稳定的代码签名身份；构建脚本不会绕过系统权限。
