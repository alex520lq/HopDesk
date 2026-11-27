# HopDesk 配置体系与无头 CLI 指南

HopDesk 将 **统一配置文件（`hopdesk.conf`）** 作为唯一持久化真相源，并在同一个 `HopDesk` 可执行文件中内置了 **无 GUI CLI**（支持 `--json`），供 AI 编程助手、脚本和运维工具在不打开主界面的情况下自发现、校验、编辑并热应用所有参数。

---

## 1. 配置文件路径

| 平台 | 默认配置路径 | 默认日志路径 |
|---|---|---|
| Windows | `<HopDesk.exe 同目录>\hopdesk.conf` | `<HopDesk.exe 同目录>\hopdesk.log` |
| macOS | `~/Library/Application Support/HopDesk/hopdesk.conf` | `~/Library/Application Support/HopDesk/hopdesk.log` |

启动后台或执行 CLI 时均可通过 `--config <path>` 指定独立配置文件。

---

## 2. CLI 命令参考

所有命令均不打开 GUI、不抢占单实例锁，可直接配合 `--json` 输出严格 JSON：

```bash
# 查看版本与协议号
hopdesk version --json

# 查看当前平台解析出的配置路径与日志路径
hopdesk config path --json

# 导出全量参数 Schema（键名、分组、类型、范围/枚举、默认值、生效级别、说明）
hopdesk config schema --json
hopdesk config schema --group transfer --json
hopdesk config schema --key audio_port --json

# 读取配置（若后台 Agent 运行且未传 --no-live / --config，优先读取 Agent 实时配置）
# token 默认脱敏为 "***"，传 --show-secret 可读取原文
hopdesk config get --json
hopdesk config get port data_port audio_port mouse_scale_pct --json

# 修改一个或多个参数（全量强校验 -> 原子写盘 -> 若 Agent 运行则通过本机 IPC 热应用）
hopdesk config set switch_delay_ms=150 mouse_scale_pct=120 wheel_invert=true --json
hopdesk config set role=client host=192.168.1.10 token=my-shared-secret --strict --json

# 重置单个或全部参数为平台默认值
hopdesk config reset mouse_scale_pct wheel_invert --json
hopdesk config reset --json

# 校验配置文件语法、范围及跨字段约束（--strict 额外检查 name/token/host 完整性）
hopdesk config validate --strict --json

# 查询运行中的后台 Agent 状态
hopdesk status --json

# 向运行中的后台 Agent 发送控制指令
hopdesk action reconnect --json
hopdesk action disconnect --json
hopdesk action reload --json
hopdesk action display-mac --json
hopdesk action display-win --json
hopdesk action display-wake --json
hopdesk action display-sleep --json
```

### `config set` JSON 返回示例

```json
{
  "ok": true,
  "config_path": "D:\\HopDesk\\hopdesk.conf",
  "agent_notified": true,
  "changed": ["switch_delay_ms", "audio_port"],
  "applied_hot": ["switch_delay_ms"],
  "requires_reconnect": [],
  "requires_restart": ["audio_port"],
  "warnings": []
}
```

---

## 3. 参数分层与生效语义

所有参数元数据由 [`src/core/config.c`](../src/core/config.c) 与 [`src/core/config_schema.h`](../src/core/config_schema.h) 统一定义，GUI、配置文件、IPC 与 CLI 共用同一份校验规则：

- **`hot`（热生效）**：保存后立即由运行中 Agent 应用，无需重启（如 `input_mode`, `audio_mode`, `ui_theme`, `server_edge`, `client_edge`, `switch_delay_ms`, `switch_cooldown_ms`, `edge_margin_px`, `mouse_scale_pct`, `wheel_scale_pct`, `wheel_invert`, `mod_ctrl`, `mod_alt`, `mod_win`, `clip_text`, `clip_image`, `clip_file`, `clip_text_max_kb`, `disp_*`, `mac_*_cmd`, `xfer_chunk_kb`, `xfer_rate_limit_mib`, `xfer_*_retry_max`, `xfer_ack_timeout_ms`, `xfer_max_*`, `xfer_rx_dir`, `net_heartbeat_*`, `net_ctrl_queue_*`, `log_level`, `log_rotate_mb`, `io_deadline_ms`, `update_check_enabled`）。
- **`reconnect`（重连生效）**：在下次连接或调用 `hopdesk action reconnect` 后生效（如 `name`, `host`, `token`, `net_handshake_timeout_ms`, `net_connect_timeout_ms`, `net_send_timeout_ms`, `audio_keepalive_ms`, `audio_peer_timeout_ms`, `audio_send_limit_ms`）。
- **`restart`（重启生效）**：绑定监听端口或固定环形队列结构，需重启后台进程后生效（如 `role`, `port`, `data_port`, `audio_port`, `xfer_rx_queue_slots`, `xfer_rx_queue_mb`, `clip_poll_ms`）。

完整 65 项参数的精确类型、范围、默认值与说明，随时执行 `hopdesk config schema --json` 即可获取最新权威定义。

---

## 4. 安全与本机 IPC 边界

- **Windows**：独立控制管道 `\\.\pipe\HopDesk.Control.v1`，启用当前用户 SID DACL 与 `PIPE_REJECT_REMOTE_CLIENTS`，仅允许本机同一用户会话访问；所有状态变更转发至 Agent 主线程串行执行。
- **macOS**：复用每用户私有 `$TMPDIR/hopdesk-agent.sock`（权限 `0600` + `getpeereid` 校验 `uid == geteuid()`）。
- **不可削弱的安全红线**：协议版本号、非空 Token 认证、CRC32 校验、相对路径穿越校验与更新包 SHA-256 校验均为硬不变量，不可通过配置关闭。
