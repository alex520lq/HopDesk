# win —— Windows 端（纯 C + Win32）

服务端角色。

- 托盘图标与极简弹窗（`Shell_NotifyIcon` / Win32 窗口）
- 输入捕获：`WH_MOUSE_LL` / `WH_KEYBOARD_LL` 低级钩子、相对位移、边缘检测
- 剪贴板：`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`
- 显示：`dxva2.dll` DDC（`SetVCPFeature`）、`DisplaySwitch.exe`
- 网络：Winsock2 TCP 服务端
- 单实例：命名 Mutex

构建：MSVC `/MT` 或 MinGW，见 `scripts/build-win.ps1`。
