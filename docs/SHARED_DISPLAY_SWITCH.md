# 共用显示器切换

适用接线：Windows 有一块保留的主屏，另一块扩展屏同时接到 Windows 和 Mac，靠输入源切换归属。普通双机各用独立屏幕时不要开启共屏模式。

## 设置与参数化配置

1. 首次设置时，在 Windows 中启用共用屏的“扩展”模式。HopDesk 需要取得原来的位置、分辨率和刷新率。
2. Windows HopDesk → 设置 → 输入与显示 → 显示器切换，开启“Windows 与 Mac 共用一块显示器”。根据界面列出的设备名、主屏标记和坐标，填写共用扩展屏的设备名，例如 `\\.\DISPLAY2`。设备名是 Windows 的名称，不等同于显示设置中的数字标签。主屏不能作为释放目标。
3. 确认 Windows/Mac 输入源值。旧脚本中的 BenQ PD2700U 接线是 Windows DP=`0x0F`（15）、Mac HDMI=`0x11`（17），其他型号/接线必须实测确认，不能只按示例填写。
4. Mac 设置中的切源命令必须指向同一块物理显示器，例如 `m1ddc display 1 set input`。后台执行命令时会补入 `/opt/homebrew/bin` 和 `/usr/local/bin` 到 PATH。HopDesk 不安装 m1ddc。
5. 所有切换参数都可在界面“设置 → 输入与显示 → 显示器切换（DDC/CI）→ 高级切换参数”或 CLI 中热生效调整（无需重启）：
   - `disp_shared`（默认 `false`）：是否启用共屏拓扑切换。
   - `disp_device`（默认空）：Windows 共用扩展屏设备名（例如 `\\.\DISPLAY2`）。
   - `disp_win_input` / `disp_mac_input`（默认 `0x000F` / `0x0011`）：VCP 0x60 输入源值。
   - `disp_wake_sec`（默认 `60`，范围 `1..3600` 秒）：交给 Mac 后保持 Mac 显示唤醒的时长（可按需调为 `1800`；切回 Windows 后会自动释放）。
   - `disp_ddc_retries`（默认 `3`，范围 `1..10`）：Windows DDC 写输入源重试次数。
   - `disp_verify_timeout_ms`（默认 `8000`，范围 `1000..30000` ms）：DDC 0x60 回读与 Windows 拓扑生效确认超时。
   - `disp_poll_ms`（默认 `200`，范围 `50..2000` ms）：DDC 回读与拓扑轮询间隔。
   - `disp_wake_settle_ms`（默认 `300`，范围 `0..10000` ms）：共屏模式收到 Mac 唤醒确认后的短等待。
   - `disp_wake_timeout_ms`（默认 `10000`，范围 `1000..30000` ms）：等待 Mac 唤醒或切源命令应答的超时。
   - `disp_wake_delay_ms`（默认 `1200`，范围 `0..10000` ms）：普通模式发送唤醒后延迟切源的时间。
   - `disp_sleep_fallback`（默认 `false`）：切回 Windows 时，若 Windows DDC 与 Mac 切源命令都失败，是否允许向 Mac 发送 `mac_sleep_cmd` 断信号兜底。
6. 保存后可使用概览页的 **“一键切换显示器”**（按现实拓扑与当前 VCP 0x60 自动判定交给 Mac / 收回 Windows / 矛盾态拉回）、**“显示器交给 Mac”**、**“显示器交给 Windows”**，或在 Windows 托盘菜单点击“一键切换显示器”，或使用 CLI `hopdesk action display-toggle`。无需 SSH 或另外保存 Mac 地址，直接使用已认证的 HopDesk 控制连接。

CLI 配置等价示例（Windows PowerShell）：

```powershell
.\HopDesk.exe config set disp_shared=true 'disp_device=\\.\DISPLAY2' disp_win_input=0x0F disp_mac_input=0x11 disp_wake_sec=1800 disp_ddc_retries=3 disp_verify_timeout_ms=8000 disp_poll_ms=200 disp_wake_settle_ms=300 --json
.\HopDesk.exe action display-toggle --json
```

共屏设置默认关闭。两端无法凭连接成功、分辨率、显示器数量或相同型号确认是同一块物理屏幕，因此必须明确选择。它不会自动更改用户正在使用的显示配置。

## 切换行为与所有权

- **一键自动判定（`display-toggle`）**：不依赖状态文件，按 Windows 当前现实决定方向：共用扩展屏不在 Windows 拓扑中 → 收回 Windows；扩展屏在 Windows 拓扑中但面板 VCP 0x60 停在 `disp_mac_input`（矛盾态）→ 直接拉回 Windows；正常双屏 → 交给 Mac。
- **交给 Mac**：收到 Mac 唤醒确认（至少一块外接屏已激活且未睡眠）→ 等待 `disp_wake_settle_ms` → 确保选定 Windows 扩展屏可访问 → 重试写入输入源并从 Windows 回读 VCP 0x60 → 保存原模式 → 从 Windows 桌面释放这一块扩展屏 → 重新枚举确认。Mac 唤醒确认不能证明选中的 m1ddc 屏幕与 Windows 目标是同一物理屏，接线/目标仍须人工核对。
- **交给 Windows**：恢复选定扩展屏及其原模式（优先使用 `hopdesk.conf.display`，不存在时尝试 Windows 注册表上次模式）→ 重新枚举确认 → 切回 Windows 输入源并回读（Windows DDC 失败时请求 Mac 执行配置的切源命令；若仍失败且用户开启 `disp_sleep_fallback`，再发送 Mac 睡眠命令兜底）→ 验收通过后通知 Mac 立即释放唤醒保活断言。若显示器切到 Mac 后因 DP HPD 导致初次恢复失败，会先请求 Mac 切回 Windows 输入源再重试恢复拓扑。
- 切源失败不释放扩展屏；释放失败尝试恢复 Windows 输出和输入源。回滚失败明确报错，需检查 Windows 显示设置及显示器 OSD。默认不强制 Mac 睡眠，仅在 `disp_sleep_fallback=true` 时启用兜底。
- 只调整指定的非主屏，不执行影响整台机器的 `DisplaySwitch /external`、`/extend`。其他物理屏和虚拟屏不作为判定条件。
- Windows 原模式保存到配置文件旁的 `hopdesk.conf.display`，先原子写入并刷新磁盘，再修改显示拓扑；进程重启后可用同一配置恢复。不要在屏幕交给 Mac 时删除它。设备改名、换接显卡接口、替换显示器后，应先在 Windows 恢复扩展，再重新设置目标。
- 单个后台 Worker 串行切换；重复操作被拒绝并立即返回失败结果。耗时 DDC、拓扑变更和 Mac 命令不在低级钩子或控制接收线程执行。所有发送与回复都在网络锁内绑定连接 epoch / session 与事务 ID，防止重连后把旧指令发给新会话。退出时先回收 Worker 再关闭网络对象。
- 普通模式保持原来的 DDC 输入源操作，不改变 Windows 拓扑；Mac 端在 Windows 不可达/断线时，明确目标按钮（交给 Mac / 交给 Windows）自动回落为本机切源并更新界面状态，不阻断离线恢复。
- 屏幕归属与键鼠提供方独立。切屏时先释放远端按键/按钮，切完仍需将鼠标推到配置边缘进入共享，不强制夺取键鼠提供权。

控制协议升级到 **v8**，数据协议仍为 v8。必须同时更新 Windows 和 Mac，并重启后台；旧控制协议会被拒绝，不支持混用。

## 测试与限制

共享生产流程测试覆盖双向顺序、自动方向判定、唤醒失败、输入源失败、释放失败、回滚失败、Mac 切源与可选睡屏兜底、收回后释放唤醒断言、帧字段校验及新参数配置校验。平台控制循环测试使用本机 TCP 和 API doubles，不代表显示器实测。

必须在真实 Windows + Mac + 共用显示器上验收：两端按钮与一键切换发起、原位置/分辨率/刷新率恢复、切源后键鼠跨屏、DDC 关闭、Mac 睡眠/断线、重复点击、切换中重连/退出、进程重启恢复，以及附带虚拟屏的场景。未执行前均为待验证。主屏共享暂不支持；应保留一块 Windows 主屏作为本地控制入口。
