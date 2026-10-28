# mac —— macOS 端（C 核心 + 少量 ObjC）

客户端角色。

- 键鼠注入：`CGEventCreate*` + `CGEventPost`（纯 C，需 Accessibility 权限）
- 显示唤醒：`IOPMAssertionCreateWithDescription`（纯 C IOKit）
- 切源：`posix_spawn` 调 `m1ddc`
- 剪贴板：`NSPasteboard`（ObjC 薄层）
- 状态栏：`NSStatusItem`（ObjC）
- 网络：POSIX socket 客户端
- 单实例：`flock`

ObjC 仅用于状态栏与剪贴板（约 200 行），其余为纯 C。
构建：`sh scripts/build-mac.sh`（clang，需 Xcode Command Line Tools），
产物 `build/hopdesk`。首次运行需在「系统设置 → 隐私与安全性 → 辅助功能」授权。
