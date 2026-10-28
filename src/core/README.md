# core —— 平台无关核心（纯 C）

两端共享，禁止包含任何平台头文件（windows.h / CoreGraphics / Cocoa 等）。

职责：

- 协议帧的打包 / 解包（`type + length + payload`）
- 连接状态机（握手、心跳、重连退避、键鼠归属）
- 配置加载与屏幕几何
- 日志
- Windows VK → macOS 键码映射表、修饰键位定义

平台层通过这里定义的接口注册回调（输入捕获/注入、剪贴板读写、显示控制、UI 提示）。
