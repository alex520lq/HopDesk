$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-core-tests.exe'
$sources = @(
    (Join-Path $root 'tests/core_tests.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c'),
    (Join-Path $root 'src/core/config.c'),
    (Join-Path $root 'src/core/keymap.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/update_manifest.c'),
    (Join-Path $root 'src/core/xfer_budget.c'),
    (Join-Path $root 'src/core/xfer_task.c'),
    (Join-Path $root 'src/core/xfer_task_history.c'),
    (Join-Path $root 'src/core/clock.c'),
    (Join-Path $root 'src/core/xfer_rate.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_buffer_pool.c'),
    (Join-Path $root 'src/core/xfer_rx_queue.c')
)
$inc = Join-Path $root 'src/core'
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$out @sources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources
}
if ($LASTEXITCODE -ne 0) { throw "Core test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Core tests failed ($LASTEXITCODE)" }
& python (Join-Path $root 'scripts/test-regressions.py')
if ($LASTEXITCODE -ne 0) { throw "Targeted regression tests failed ($LASTEXITCODE)" }
$resilience = Join-Path $build 'hopdesk-resilience-tests.exe'
$resilienceSources = @(
    (Join-Path $root 'tests/resilience_tests.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/clock.c'),
    (Join-Path $root 'src/core/log.c'),
    (Join-Path $root 'src/core/session.c'),
    (Join-Path $root 'src/core/watchdog.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$resilience @resilienceSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $resilience @resilienceSources
}
if ($LASTEXITCODE -ne 0) { throw "Resilience test compilation failed ($LASTEXITCODE)" }
& $resilience (Join-Path $build 'resilience-test.log')
if ($LASTEXITCODE -ne 0) { throw "Resilience tests failed ($LASTEXITCODE)" }
& (Join-Path $PSScriptRoot 'test-ha.ps1')

& $resilience --watchdog-progress
if ($LASTEXITCODE -ne 0) { throw "Progressing watchdog operation unexpectedly failed" }
& $resilience --watchdog-hang
if ($LASTEXITCODE -ne 70) { throw "Watchdog did not fail-stop a stalled operation" }

$global:LASTEXITCODE = 0 # Expected watchdog exit 70 is a successful test.

& python (Join-Path $root 'scripts/test-updates.py')
if ($LASTEXITCODE -ne 0) { throw "Update lifecycle regressions failed" }
& (Join-Path $PSScriptRoot 'test-update-windows.ps1')

$writerIo = Join-Path $build 'hopdesk-writer-io-tests.exe'
$writerIoSources = @((Join-Path $root 'tests/writer_io_tests.c'), (Join-Path $root 'src/win/xfer_writer_io.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I (Join-Path $root 'src/win') /Fe:$writerIo @writerIoSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I (Join-Path $root 'src/win') -o $writerIo @writerIoSources
}
if ($LASTEXITCODE -ne 0) { throw "Writer IO adapter compilation failed ($LASTEXITCODE)" }
& $writerIo
if ($LASTEXITCODE -ne 0) { throw "Writer IO adapter tests failed ($LASTEXITCODE)" }

$rxQueueTests = Join-Path $build 'hopdesk-rx-queue-tests.exe'
$rxQueueSources = @((Join-Path $root 'tests/xfer_rx_queue_tests.c'), (Join-Path $root 'src/core/xfer_rx_queue.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxQueueTests @rxQueueSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxQueueTests @rxQueueSources
}
if ($LASTEXITCODE -ne 0) { throw "RX queue test compilation failed ($LASTEXITCODE)" }
& $rxQueueTests
if ($LASTEXITCODE -ne 0) { throw "RX queue tests failed ($LASTEXITCODE)" }

$writerWorker = Join-Path $build 'hopdesk-writer-worker-tests.exe'
$writerWorkerSources = @((Join-Path $root 'tests/writer_worker_tests.c'), (Join-Path $root 'src/win/xfer_writer_worker.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I (Join-Path $root 'src/win') /Fe:$writerWorker @writerWorkerSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I (Join-Path $root 'src/win') -o $writerWorker @writerWorkerSources
}
if ($LASTEXITCODE -ne 0) { throw "Writer worker compilation failed ($LASTEXITCODE)" }
& $writerWorker
if ($LASTEXITCODE -ne 0) { throw "Writer worker lifecycle tests failed ($LASTEXITCODE)" }

$writerQueue = Join-Path $build 'hopdesk-writer-queue-tests.exe'
$writerQueueSources = @((Join-Path $root 'tests/writer_queue_tests.c'), (Join-Path $root 'src/win/xfer_writer_queue.c'), (Join-Path $root 'src/win/xfer_writer_io.c'), (Join-Path $root 'src/win/xfer_writer_worker.c'), (Join-Path $root 'src/core/xfer_buffer_pool.c'), (Join-Path $root 'src/core/xfer_rx_queue.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /I (Join-Path $root 'src/win') /Fe:$writerQueue @writerQueueSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -I (Join-Path $root 'src/win') -o $writerQueue @writerQueueSources
}
if ($LASTEXITCODE -ne 0) { throw "Writer queue compilation failed ($LASTEXITCODE)" }
& $writerQueue
if ($LASTEXITCODE -ne 0) { throw "Writer queue ownership/IO tests failed ($LASTEXITCODE)" }
