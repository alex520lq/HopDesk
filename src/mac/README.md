# mac —— macOS 端（C 核心 + 少量 ObjC）

客户端角色。

- 键鼠注入：`CGEventCreate*` + `CGEventPost`（纯 C，需 Accessibility/PostEvent 权限）；本地跨端文件粘贴快捷键还需 Input Monitoring。应用首次启动会请求权限并打开对应设置页。

本地 `build-mac.sh` 默认使用 ad-hoc 签名，重建后 macOS 可能要求重新授权。安装版应通过 `HOPDESK_CODESIGN_IDENTITY` 指定稳定的代码签名身份；构建脚本不会绕过系统权限。
- 显示唤醒：`IOPMAssertionCreateWithDescription`（纯 C IOKit）
- 切源：`posix_spawn` 调 `m1ddc`
- 剪贴板：`NSPasteboard`（ObjC 薄层）
- 状态栏：`NSStatusItem`（ObjC）
- 网络：POSIX socket 客户端
- 单实例：`flock`

ObjC 仅用于状态栏与剪贴板（约 200 行），其余为纯 C。
构建：`sh scripts/build-mac.sh`（clang，需 Xcode Command Line Tools），
产物 `build/hopdesk`。首次运行需在「系统设置 → 隐私与安全性 → 辅助功能」授权。
