# HopDesk 架构审计清单（基线：main @ c00bf9d9）

> 本文是架构重构规划的代码审计附件。文件行号是基线定位信息，后续提交会变化；审计描述职责与依赖，不代表所有列出的实现都是缺陷。

## 1. 传输模块

### Windows `src/win/xfer.c`（约 1,913 行）

职责/函数簇：
- 状态与历史：`history_prepare_path`、`history_load/save`、`status_*`、`xfer_get_status*`、`xfer_get_history`。
- 文件枚举：`elist_push/free`、`guarded_find_next`、`walk_into`。
- 网络帧与发送：`send_session_current`、`send_all`、`send_small`、`send_chunk`、`send_file_entry`、`send_image_job`、`send_done*`。
- 限速/反馈：`rate_init_from_config`、`rate_wait_for_chunk`、`rate_queue_pressure_permille`、`rate_feedback`。
- ACK：`ack_register`、`ack_mark_sent`、`ack_set_deadline`、`ack_deliver`、`ack_wait`。
- 任务编排：`run_job`、`send_thread`、`submit`、`xfer_submit_*`、`xfer_cancel_task`、`xfer_init/shutdown`。
- 接收端：在文件末尾通过 `#include "xfer_receive.inc"` 编译。

重要共享状态包括 status/history locks、send/receive tracking、history records、TX frame lock、发送 epoch，以及传输队列/当前 task/session 状态。具体字段必须在拆分前进一步列成 owner/lock 表。

### macOS `src/mac/xfer.c`（约 1,860 行）

职责簇与 Windows 基本对应，但系统调用分别使用 POSIX/pthread。额外需要核对：
- macOS 的数据连接建立/接受方式与 Windows listener/client 组合不同。
- `pthread` 生命周期、QOS、socket close/shutdown 与 Windows thread/event 语义不能机械替换。
- `s_txlock`、`s_status_lock`、`s_history_io_lock`、`s_send_epoch` 等状态必须明确线程所有权。
- 同样 include `xfer_receive.inc`，所以拆分时要按行为对齐而不是按行号复制。

### Receive includes

`src/win/xfer_receive.inc`（约 1,415 行）和 `src/mac/xfer_receive.inc`（约 1,278 行）可分为：
1. Writer queue / buffer pool：`wq_pool_acquire_locked`、`wq_pool_release_locked`、`wq_task_run`、`writer_thread`（Windows）、`wq_push*`、`wq_drain/start/stop`。
2. Receiver file state：`rx_close_file`、`rx_clear_state`、`rx_reset`、`rx_bind_session`、`rx_expire_stale_session`、目录与顶层条目管理。
3. Batch lifecycle：`rx_terminate`、`rx_begin_batch`、`rx_finish_batch`。
4. Protocol dispatch：`on_data_msg`、`on_data_frame_checked`、`on_data_frame`。
5. Socket lifecycle：Windows `data_listener`、`listen_thread`、`data_client_thread`；macOS `do_connect`、`accept_data`。

Writer queue 当前通过 include 访问 task/budget、status timing、watchdog 和平台文件操作。独立化时必须用明确的配置/回调接口替代隐式静态依赖。Writer 负责缓冲区和写盘任务，不负责协议 ACK 或 Task 终态决策。

## 2. 剪贴板模块

### Windows `src/win/clipboard.c`（886 行）

函数簇：
- Offer 生命周期：`clip_publish_local_offer`、`clip_send_offer*`、`clip_invalidate_remote_offer`、`clip_clear_local_offer`。
- Win32 clipboard 操作：`apply_global_expected`、`clip_retry_pending`、`clip_apply_remote*`、`make_drop_global`。
- 图片后台：`send_dib`、`image_worker`、`queue_image`。
- 文本/文件提交：`send_text`、`send_drop`。
- Paste Transaction：`clip_request_remote_paste`、`clip_on_paste_request`、`clip_complete_remote_paste`、`clip_on_paste_failed`。

模块静态状态同时保存 Win32 HGLOBAL/sequence、worker HANDLE、Offer generation、Request 去重表、pending paste、stopping 状态。系统剪贴板句柄与跨端事务状态应拆开；sequence compare-and-apply 应由 coordinator 驱动，Win32 adapter 只执行受条件保护的系统操作。

### macOS `src/mac/clipboard.m`（766 行）

函数簇：
- Offer/Request 生命周期与 Windows 同域。
- NSPasteboard 轮询：`poll_pasteboard`、`clipboard_init/shutdown`。
- 图片快照/转换：`send_image`、`send_image_data_async`、`image_conversion_fits_budget`。
- Cocoa 应用：`clipboard_apply*`、`apply_objects_retry`。
- Paste Transaction：`clipboard_request_remote_paste`、`clipboard_on_paste_request`、`clipboard_complete_remote_paste`。

静态状态同时持有 changeCount、dispatch timer/group、Offer/Request 状态和 worker 生命周期。主队列只应采集轻量快照/提交 UI 操作；图像转换、文件扫描和大内存操作在有预算的后台工作执行。

## 3. 连接与输入

- `src/win/net_server.c`（约 424 行）：控制消息 dispatch、发送队列、reader/sender/server/client 线程、监听与 socket 生命周期混合。需区分 connection coordinator、bounded control queue、WinSock adapter。
- `src/mac/net_client.c`（约 438 行）：控制消息 dispatch、socket connect/accept、发送队列、generation、reconnect loop。与 Windows 的角色/拓扑并非同构，不能只按文件名合并；先统一 session contract，再拆平台实现。
- 输入捕获/注入文件：`src/win/mouse_capture.c`、`kbd_capture.c`、`inject.c`；macOS `capture.c`、`inject.c`。必须保留按下/释放配对、modifier 映射、断线 release-all、权限提示和角色切换语义。

## 4. UI / 更新

- Windows `main_ui.c` 同时维护 HWND、页面构建、配置草稿、任务 ID 列表、历史筛选、日志 worker、刷新和窗口消息。
- macOS `main_ui.m` 同时维护 Cocoa 控件、配置草稿、任务/历史刷新、日志和页面构建。
- macOS `statusbar.m` 同时承担状态栏、传输窗口和更新下载/校验流程；更新功能不应属于 UI presenter。
- 首步不拆 UI：先让 UI 消费一致的只读快照；控件构建/布局拆分放在传输、剪贴板、连接接口稳定之后。

## 5. Core 与头文件

- `src/core/hop.h` 约 474 行，集中帧/配置/消息/传输/剪贴板 codec 声明及常量；适合后续按 protocol、config、session、clipboard、transfer types 拆头文件，但必须避免破坏现有 include contract。
- 现有 Core 已有独立的 `xfer_budget`、`xfer_task`、`xfer_task_history`、`xfer_rate` 等模块；重构应复用并完善它们，不能重新造 Task Manager 或第二份预算账本。
- `xfer_ack_window.h`、`xfer_tracking.h` 仍需检查纯策略/数据结构与同步机制的边界，不能仅因头文件较大而拆分。

## 6. 构建与测试现状

- Windows app build 从 `src/core/*.c` 和 `src/win/*.c` 顶层枚举；macOS build 使用 `find -maxdepth 1`。若加入嵌套目录，新模块会漏编译。阶段 1 已先将 app build source discovery 改为递归。
- Core test scripts 使用显式 source list；这是测试目标的依赖清单，不应简单替换为链接所有 Core 文件。新增模块测试时必须显式更新对应测试目标。
- CI workflow 为 Windows MSVC + macOS Apple Clang，分别运行 core、transfer protocol、TCP loopback 和角色/连接测试，并打包两端应用。
- `tests/core_tests.c`（约 1,497 行）包含 protocol/config/keymap/transfer validation/budget/rate/task/tracking/history 等测试簇，后续按真实生产模块拆成独立 test targets。
- `scripts/test-regressions.py`（约 863 行）包含多个源码提取/合成夹具。保留能验证平台边界回归的用例，但优先将 writer/receiver/clipboard 生产逻辑改为可直接链接的模块测试。

## 7. Windows/macOS 对照原则

| 领域 | 必须一致 | 允许不同 |
|---|---|---|
| Protocol / validation | 字段、长度、错误语义、session/batch 规则 | socket API |
| Task / budget | 状态转换、Reservation owner、exactly-once release | mutex/atomic 原语 |
| ACK / checkpoint | ACK 后推进；未确认文件恢复 | event/condvar 实现 |
| Writer | slot/bytes/pool 约束、写入结果和错误语义 | HANDLE vs fd、线程 API |
| Clipboard | Offer/Request/generation 规则、compare-and-apply | HGLOBAL vs NSPasteboard/changeCount |
| Input | 按键语义、modifier、release-all | hook vs CGEvent |
| UI | 状态快照字段、命令语义 | Win32 vs Cocoa 控件 |

## 8. 本审计暂未证明的事项

- 当前主干的完整 CI 通过情况：需以本轮 PR 最新 Actions run 为准。
- 真机双向文件粘贴、长时间传输、弱网、慢盘、8 小时 idle、真实输入延迟：本次代码静态审计未执行。
- 全部静态变量和所有动态分配的完整所有权闭环：需要阶段 0 后续对 writer/receiver/sender 按线程和资源逐项审计，当前只完成了主要模块和状态入口的识别。
- 行数与函数簇来自代码快照，不能替代动态调用图或 sanitizer/线程压力验证。

## 9. 首个模块拆分记录

`src/core/xfer_ack_window.h` 原先包含 11 个 `static inline` 函数，Windows/macOS 发送端都直接包含该头文件。已拆为：
- `src/core/xfer_ack_window.h`：ACK window 数据结构、常量和公开函数声明。
- `src/core/xfer_ack_window.c`：reset/count/register/mark-sent/deadline/deliver/take/unregister/checkpoint 规则实现。
- `scripts/test-core.ps1` 与 `scripts/test-core.sh`：将新生产编译单元链接进两端 Core 测试。

该拆分不改变 ACK window 的同步约定（调用方仍持有平台 ACK lock），也不把平台锁引入 Core。原有 `test_xfer_ack_window` 覆盖窗口容量、重复 ACK、deadline race、RTT 和连续 checkpoint，保留原测试向量。

## 10. 审计结论

第一项真正的生产代码拆分应从 writer queue 开始：其职责相对独立、slot/bytes/pool 所有权可明确建模，而且能通过慢盘、满队列、取消、shutdown drain 等测试验证。不要先拆 UI，也不要直接把整个 `.inc` 改名为 `.c`；后者只会把隐式依赖变成编译/链接错误。
