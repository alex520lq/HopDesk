$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-ha-control-tests.exe'
$inc = Join-Path $root 'src/core'
$sources = @('tests/ha_control_tests.c','src/win/net_server.c','src/win/xfer.c','src/win/xfer_ack_coordinator.c','src/win/xfer_scanner.c','src/win/xfer_rx_protocol_handlers.c','src/win/xfer_rx_fs.c','src/win/xfer_writer_io.c','src/win/xfer_writer_worker.c','src/win/xfer_writer_queue.c','src/core/crc32.c','src/core/xfer_validate.c','src/core/protocol.c','src/core/clock.c','src/core/log.c','src/core/session.c','src/core/watchdog.c','src/core/update_state.c','src/core/xfer_budget.c','src/core/xfer_task.c','src/core/xfer_task_history.c','src/core/xfer_rate.c','src/core/xfer_ack_window.c','src/core/xfer_sender_engine.c','src/core/xfer_sender_queue.c','src/core/xfer_sender_retry.c','src/core/xfer_tracking.c','src/core/xfer_buffer_pool.c','src/core/xfer_rx_queue.c','src/core/xfer_rx_receipt.c','src/core/xfer_rx_lifecycle.c','src/core/xfer_rx_batch_rules.c','src/core/xfer_rx_entry_reservations.c','src/core/xfer_rx_file_state.c','src/core/xfer_rx_dispatch.c','src/core/xfer_rx_message_handlers.c','src/core/xfer_rx_ack_handler.c','src/core/xfer_rx_protocol_adapter.c','src/core/xfer_rx_batch_state.c','src/core/xfer_rx_image_state.c','src/core/xfer_rx_image_buffer.c') | ForEach-Object { Join-Path $root $_ }
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$out @sources ws2_32.lib user32.lib shell32.lib
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources -lws2_32 -luser32 -lshell32
}
if ($LASTEXITCODE -ne 0) { throw "HA test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "HA control tests failed ($LASTEXITCODE)" }
