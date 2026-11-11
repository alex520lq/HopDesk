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
    (Join-Path $root 'src/core/xfer_rx_queue.c'),
    (Join-Path $root 'src/core/xfer_rx_batch_rules.c'),
    (Join-Path $root 'src/core/xfer_rx_entry_reservations.c'),
    (Join-Path $root 'src/core/xfer_rx_file_state.c')
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
$writerQueueSources = @((Join-Path $root 'tests/writer_queue_tests.c'), (Join-Path $root 'src/win/xfer_writer_queue.c'), (Join-Path $root 'src/win/xfer_writer_io.c'), (Join-Path $root 'src/win/xfer_writer_worker.c'), (Join-Path $root 'src/core/xfer_buffer_pool.c'), (Join-Path $root 'src/core/xfer_rx_queue.c'), (Join-Path $root 'src/core/xfer_budget.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /I (Join-Path $root 'src/win') /Fe:$writerQueue @writerQueueSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -I (Join-Path $root 'src/win') -o $writerQueue @writerQueueSources
}
if ($LASTEXITCODE -ne 0) { throw "Writer queue compilation failed ($LASTEXITCODE)" }
& $writerQueue
if ($LASTEXITCODE -ne 0) { throw "Writer queue ownership/IO tests failed ($LASTEXITCODE)" }

$rxFs = Join-Path $build 'hopdesk-rx-fs-tests.exe'
$rxFsSources = @((Join-Path $root 'tests/xfer_rx_fs_tests.c'), (Join-Path $root 'src/win/xfer_rx_fs.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I (Join-Path $root 'src/win') /Fe:$rxFs @rxFsSources shell32.lib
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I (Join-Path $root 'src/win') -o $rxFs @rxFsSources -lshell32
}
if ($LASTEXITCODE -ne 0) { throw "RX filesystem test compilation failed ($LASTEXITCODE)" }
& $rxFs
if ($LASTEXITCODE -ne 0) { throw "RX filesystem adapter tests failed ($LASTEXITCODE)" }

$ackCoordinator = Join-Path $build 'hopdesk-ack-coordinator-tests.exe'
$ackCoordinatorSources = @((Join-Path $root 'tests/xfer_ack_coordinator_tests.c'), (Join-Path $root 'src/win/xfer_ack_coordinator.c'), (Join-Path $root 'src/core/xfer_ack_window.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /I (Join-Path $root 'src/win') /Fe:$ackCoordinator @ackCoordinatorSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -I (Join-Path $root 'src/win') -o $ackCoordinator @ackCoordinatorSources
}
if ($LASTEXITCODE -ne 0) { throw "ACK coordinator compilation failed ($LASTEXITCODE)" }
& $ackCoordinator
if ($LASTEXITCODE -ne 0) { throw "ACK coordinator tests failed ($LASTEXITCODE)" }

$scannerTests = Join-Path $build 'hopdesk-scanner-tests.exe'
$scannerSources = @((Join-Path $root 'tests/xfer_scanner_tests.c'), (Join-Path $root 'src/win/xfer_scanner.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /I (Join-Path $root 'src/win') /Fe:$scannerTests @scannerSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -I (Join-Path $root 'src/win') -o $scannerTests @scannerSources
}
if ($LASTEXITCODE -ne 0) { throw "Scanner adapter compilation failed ($LASTEXITCODE)" }
& $scannerTests
if ($LASTEXITCODE -ne 0) { throw "Scanner adapter tests failed ($LASTEXITCODE)" }

$rxAckHandler = Join-Path $build 'hopdesk-rx-ack-handler-tests.exe'
$rxAckHandlerSources = @((Join-Path $root 'tests/xfer_rx_ack_handler_tests.c'), (Join-Path $root 'src/core/xfer_rx_ack_handler.c'), (Join-Path $root 'src/core/xfer_validate.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxAckHandler @rxAckHandlerSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxAckHandler @rxAckHandlerSources
}
if ($LASTEXITCODE -ne 0) { throw "RX ACK handler test compilation failed ($LASTEXITCODE)" }
& $rxAckHandler
if ($LASTEXITCODE -ne 0) { throw "RX ACK handler tests failed ($LASTEXITCODE)" }

$rxReceipt = Join-Path $build 'hopdesk-rx-receipt-tests.exe'
$rxReceiptSources = @((Join-Path $root 'tests/xfer_rx_receipt_tests.c'), (Join-Path $root 'src/core/xfer_rx_receipt.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxReceipt @rxReceiptSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxReceipt @rxReceiptSources
}
if ($LASTEXITCODE -ne 0) { throw "RX receipt test compilation failed ($LASTEXITCODE)" }
& $rxReceipt
if ($LASTEXITCODE -ne 0) { throw "RX receipt tests failed ($LASTEXITCODE)" }

$rxLifecycle = Join-Path $build 'hopdesk-rx-lifecycle-tests.exe'
$rxLifecycleSources = @((Join-Path $root 'tests/xfer_rx_lifecycle_tests.c'), (Join-Path $root 'src/core/xfer_rx_lifecycle.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxLifecycle @rxLifecycleSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxLifecycle @rxLifecycleSources
}
if ($LASTEXITCODE -ne 0) { throw "RX lifecycle test compilation failed ($LASTEXITCODE)" }
& $rxLifecycle
if ($LASTEXITCODE -ne 0) { throw "RX lifecycle tests failed ($LASTEXITCODE)" }

$rxBatchRules = Join-Path $build 'hopdesk-rx-batch-rules-tests.exe'
$rxBatchRulesSources = @((Join-Path $root 'tests/xfer_rx_batch_rules_tests.c'), (Join-Path $root 'src/core/xfer_rx_batch_rules.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxBatchRules @rxBatchRulesSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxBatchRules @rxBatchRulesSources
}
if ($LASTEXITCODE -ne 0) { throw "RX batch rules test compilation failed ($LASTEXITCODE)" }
& $rxBatchRules
if ($LASTEXITCODE -ne 0) { throw "RX batch rules tests failed ($LASTEXITCODE)" }

$rxEntryReservations = Join-Path $build 'hopdesk-rx-entry-reservations-tests.exe'
$rxEntryReservationSources = @((Join-Path $root 'tests/xfer_rx_entry_reservations_tests.c'), (Join-Path $root 'src/core/xfer_rx_entry_reservations.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxEntryReservations @rxEntryReservationSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxEntryReservations @rxEntryReservationSources
}
if ($LASTEXITCODE -ne 0) { throw "RX entry reservation test compilation failed ($LASTEXITCODE)" }
& $rxEntryReservations
if ($LASTEXITCODE -ne 0) { throw "RX entry reservation tests failed ($LASTEXITCODE)" }

$rxFileState = Join-Path $build 'hopdesk-rx-file-state-tests.exe'
$rxFileStateSources = @((Join-Path $root 'tests/xfer_rx_file_state_tests.c'), (Join-Path $root 'src/core/xfer_rx_file_state.c'), (Join-Path $root 'src/core/xfer_validate.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxFileState @rxFileStateSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxFileState @rxFileStateSources
}
if ($LASTEXITCODE -ne 0) { throw "RX file state test compilation failed ($LASTEXITCODE)" }
& $rxFileState
if ($LASTEXITCODE -ne 0) { throw "RX file state tests failed ($LASTEXITCODE)" }

$rxImageState = Join-Path $build 'hopdesk-rx-image-state-tests.exe'
$rxImageStateSources = @((Join-Path $root 'tests/xfer_rx_image_state_tests.c'), (Join-Path $root 'src/core/xfer_rx_image_state.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxImageState @rxImageStateSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxImageState @rxImageStateSources
}
if ($LASTEXITCODE -ne 0) { throw "RX image state test compilation failed ($LASTEXITCODE)" }
& $rxImageState
if ($LASTEXITCODE -ne 0) { throw "RX image state tests failed ($LASTEXITCODE)" }

$rxImageBuffer = Join-Path $build 'hopdesk-rx-image-buffer-tests.exe'
$rxImageBufferSources = @((Join-Path $root 'tests/xfer_rx_image_buffer_tests.c'), (Join-Path $root 'src/core/xfer_rx_image_buffer.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$rxImageBuffer @rxImageBufferSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $rxImageBuffer @rxImageBufferSources
}
if ($LASTEXITCODE -ne 0) { throw "RX image buffer test compilation failed ($LASTEXITCODE)" }
& $rxImageBuffer
if ($LASTEXITCODE -ne 0) { throw "RX image buffer tests failed ($LASTEXITCODE)" }

$senderThreads = Join-Path $build 'hopdesk-sender-threads-tests.exe'
$senderThreadsSources = @(
    (Join-Path $root 'tests/win_sender_threads_tests.c'),
    (Join-Path $root 'src/win/xfer_sender_threads.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I (Join-Path $root 'src/win') /Fe:$senderThreads @senderThreadsSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I (Join-Path $root 'src/win') -o $senderThreads @senderThreadsSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender thread adapter test compilation failed ($LASTEXITCODE)" }
& $senderThreads
if ($LASTEXITCODE -ne 0) { throw "Sender thread adapter tests failed ($LASTEXITCODE)" }

$senderContext = Join-Path $build 'hopdesk-sender-context-tests.exe'
$senderContextSources = @((Join-Path $root 'tests/xfer_sender_context_tests.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderContext @senderContextSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderContext @senderContextSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender context contract compilation failed ($LASTEXITCODE)" }
& $senderContext
if ($LASTEXITCODE -ne 0) { throw "Sender context contract tests failed ($LASTEXITCODE)" }

$senderAckWait = Join-Path $build 'hopdesk-sender-ack-wait-tests.exe'
$senderAckWaitSources = @(
    (Join-Path $root 'tests/xfer_sender_ack_wait_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_ack_wait.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderAckWait @senderAckWaitSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderAckWait @senderAckWaitSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender ACK wait test compilation failed ($LASTEXITCODE)" }
& $senderAckWait
if ($LASTEXITCODE -ne 0) { throw "Sender ACK wait tests failed ($LASTEXITCODE)" }

$senderChunk = Join-Path $build 'hopdesk-sender-chunk-tests.exe'
$senderChunkSources = @(
    (Join-Path $root 'tests/xfer_sender_chunk_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_chunk.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c'),
    (Join-Path $root 'src/core/xfer_rate.c'),
    (Join-Path $root 'src/core/xfer_budget.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/session.c'),
    (Join-Path $root 'src/core/clock.c'),
    (Join-Path $root 'src/core/watchdog.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderChunk @senderChunkSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderChunk @senderChunkSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender chunk test compilation failed ($LASTEXITCODE)" }
& $senderChunk
if ($LASTEXITCODE -ne 0) { throw "Sender chunk tests failed ($LASTEXITCODE)" }

$senderQueue = Join-Path $build 'hopdesk-sender-queue-tests.exe'
$senderQueueSources = @(
    (Join-Path $root 'tests/xfer_sender_queue_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_queue.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderQueue @senderQueueSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderQueue @senderQueueSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender queue policy test compilation failed ($LASTEXITCODE)" }
& $senderQueue
if ($LASTEXITCODE -ne 0) { throw "Sender queue policy tests failed ($LASTEXITCODE)" }

$senderScheduler = Join-Path $build 'hopdesk-sender-scheduler-tests.exe'
$senderSchedulerSources = @(
    (Join-Path $root 'tests/xfer_sender_scheduler_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_scheduler.c'),
    (Join-Path $root 'src/core/xfer_sender_queue.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderScheduler @senderSchedulerSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderScheduler @senderSchedulerSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender scheduler test compilation failed ($LASTEXITCODE)" }
& $senderScheduler
if ($LASTEXITCODE -ne 0) { throw "Sender scheduler tests failed ($LASTEXITCODE)" }

$senderActiveJob = Join-Path $build 'hopdesk-sender-active-job-tests.exe'
$senderActiveJobSources = @(
    (Join-Path $root 'tests/xfer_sender_active_job_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_active_job.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderActiveJob @senderActiveJobSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderActiveJob @senderActiveJobSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender active-job test compilation failed ($LASTEXITCODE)" }
& $senderActiveJob
if ($LASTEXITCODE -ne 0) { throw "Sender active-job tests failed ($LASTEXITCODE)" }

$senderContext = Join-Path $build 'hopdesk-sender-context-adapter-tests.exe'
$senderContextSources = @(
    (Join-Path $root 'tests/xfer_sender_context_adapter_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderContext @senderContextSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderContext @senderContextSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender context adapter test compilation failed ($LASTEXITCODE)" }
& $senderContext
if ($LASTEXITCODE -ne 0) { throw "Sender context adapter tests failed ($LASTEXITCODE)" }

$senderRetry = Join-Path $build 'hopdesk-sender-retry-tests.exe'
$senderRetrySources = @(
    (Join-Path $root 'tests/xfer_sender_retry_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_retry.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderRetry @senderRetrySources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderRetry @senderRetrySources
}
if ($LASTEXITCODE -ne 0) { throw "Sender retry policy test compilation failed ($LASTEXITCODE)" }
& $senderRetry
if ($LASTEXITCODE -ne 0) { throw "Sender retry policy tests failed ($LASTEXITCODE)" }

$senderLifecycle = Join-Path $build 'hopdesk-sender-lifecycle-tests.exe'
$senderLifecycleSources = @(
    (Join-Path $root 'tests/xfer_sender_lifecycle_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_lifecycle.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderLifecycle @senderLifecycleSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderLifecycle @senderLifecycleSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender lifecycle policy test compilation failed ($LASTEXITCODE)" }
& $senderLifecycle
if ($LASTEXITCODE -ne 0) { throw "Sender lifecycle policy tests failed ($LASTEXITCODE)" }

$senderWorkerFlow = Join-Path $build 'hopdesk-sender-worker-flow-tests.exe'
$senderWorkerFlowSources = @(
    (Join-Path $root 'tests/xfer_sender_worker_flow_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_worker_flow.c'),
    (Join-Path $root 'src/core/xfer_sender_retry.c'),
    (Join-Path $root 'src/core/xfer_sender_lifecycle.c'),
    (Join-Path $root 'src/core/xfer_task.c'),
    (Join-Path $root 'src/core/xfer_budget.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderWorkerFlow @senderWorkerFlowSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderWorkerFlow @senderWorkerFlowSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender worker flow test compilation failed ($LASTEXITCODE)" }
& $senderWorkerFlow
if ($LASTEXITCODE -ne 0) { throw "Sender worker flow tests failed ($LASTEXITCODE)" }

$senderWorker = Join-Path $build 'hopdesk-sender-worker-tests.exe'
$senderWorkerSources = @(
    (Join-Path $root 'tests/xfer_sender_worker_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_worker.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c'),
    (Join-Path $root 'src/core/xfer_sender_scheduler.c'),
    (Join-Path $root 'src/core/xfer_sender_queue.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$senderWorker @senderWorkerSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderWorker @senderWorkerSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender worker orchestrator compilation failed ($LASTEXITCODE)" }
& $senderWorker
if ($LASTEXITCODE -ne 0) { throw "Sender worker orchestrator tests failed ($LASTEXITCODE)" }

$senderEngine = Join-Path $build 'hopdesk-sender-engine-tests.exe'
$senderEngineSources = @(
    (Join-Path $root 'tests/xfer_sender_engine_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_engine.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderEngine @senderEngineSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderEngine @senderEngineSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender engine test compilation failed ($LASTEXITCODE)" }
& $senderEngine
if ($LASTEXITCODE -ne 0) { throw "Sender engine tests failed ($LASTEXITCODE)" }

$senderEngineAdapter = Join-Path $build 'hopdesk-sender-engine-adapter-tests.exe'
$senderEngineAdapterSources = @(
    (Join-Path $root 'tests/xfer_sender_engine_adapter_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_engine_adapter.c'),
    (Join-Path $root 'src/core/xfer_sender_ack_wait.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c'),
    (Join-Path $root 'src/core/xfer_sender_engine.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderEngineAdapter @senderEngineAdapterSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderEngineAdapter @senderEngineAdapterSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender engine adapter test compilation failed ($LASTEXITCODE)" }
& $senderEngineAdapter
if ($LASTEXITCODE -ne 0) { throw "Sender engine adapter tests failed ($LASTEXITCODE)" }

$senderImage = Join-Path $build 'hopdesk-sender-image-tests.exe'
$senderImageSources = @(
    (Join-Path $root 'tests/xfer_sender_image_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_engine.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderImage @senderImageSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderImage @senderImageSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender image test compilation failed ($LASTEXITCODE)" }
& $senderImage
if ($LASTEXITCODE -ne 0) { throw "Sender image tests failed ($LASTEXITCODE)" }

$senderFile = Join-Path $build 'hopdesk-sender-file-tests.exe'
$senderFileSources = @(
    (Join-Path $root 'tests/xfer_sender_file_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_engine.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderFile @senderFileSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderFile @senderFileSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender file test compilation failed ($LASTEXITCODE)" }
& $senderFile
if ($LASTEXITCODE -ne 0) { throw "Sender file tests failed ($LASTEXITCODE)" }

$senderCommit = Join-Path $build 'hopdesk-sender-commit-tests.exe'
$senderCommitSources = @(
    (Join-Path $root 'tests/xfer_sender_commit_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_engine.c'),
    (Join-Path $root 'src/core/xfer_tracking.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderCommit @senderCommitSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderCommit @senderCommitSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender commit test compilation failed ($LASTEXITCODE)" }
& $senderCommit
if ($LASTEXITCODE -ne 0) { throw "Sender commit tests failed ($LASTEXITCODE)" }


$senderAckAdapter = Join-Path $build 'hopdesk-sender-ack-adapter-tests.exe'
$senderAckAdapterSources = @(
    (Join-Path $root 'tests/xfer_sender_ack_adapter_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_ack_adapter.c'),
    (Join-Path $root 'src/win/xfer_ack_coordinator.c'),
    (Join-Path $root 'src/core/xfer_ack_window.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$senderAckAdapter @senderAckAdapterSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $senderAckAdapter @senderAckAdapterSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender ACK adapter test compilation failed ($LASTEXITCODE)" }
& $senderAckAdapter
if ($LASTEXITCODE -ne 0) { throw "Sender ACK adapter tests failed ($LASTEXITCODE)" }

$clipboardTxn = Join-Path $build 'hopdesk-clipboard-txn-tests.exe'
$clipboardTxnSources = @(
    (Join-Path $root 'tests/xfer_clipboard_txn_tests.c'),
    (Join-Path $root 'src/core/xfer_clipboard_txn.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /I $inc /Fe:$clipboardTxn @clipboardTxnSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $clipboardTxn @clipboardTxnSources
}
if ($LASTEXITCODE -ne 0) { throw "Clipboard transaction test compilation failed ($LASTEXITCODE)" }
& $clipboardTxn
if ($LASTEXITCODE -ne 0) { throw "Clipboard transaction tests failed ($LASTEXITCODE)" }

# Shared Sender Job release transaction (owner/resource/Task/Shutdown ordering).
$jobLifecycle = Join-Path $build 'hopdesk-sender-job-lifecycle-tests.exe'
$jobLifecycleSources = @(
    (Join-Path $root 'tests/xfer_sender_job_lifecycle_tests.c'),
    (Join-Path $root 'src/core/xfer_sender_job_lifecycle.c'),
    (Join-Path $root 'src/core/xfer_sender_context_adapter.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$jobLifecycle @jobLifecycleSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $jobLifecycle @jobLifecycleSources
}
if ($LASTEXITCODE -ne 0) { throw "Sender Job Lifecycle test compilation failed ($LASTEXITCODE)" }
& $jobLifecycle
if ($LASTEXITCODE -ne 0) { throw "Sender Job Lifecycle tests failed ($LASTEXITCODE)" }
