#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-core-tests" \
  "$ROOT/tests/core_tests.c" \
  "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/display_switch.c" \
  "$ROOT/src/core/display_catalog.c" \
  "$ROOT/src/core/crc32.c" \
  "$ROOT/src/core/config.c" \
  "$ROOT/src/core/cli.c" \
  "$ROOT/src/core/runtime_config.c" \
  "$ROOT/src/core/log.c" \
  "$ROOT/src/core/keymap.c" \
  "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/update_manifest.c" \
  "$ROOT/src/core/update_controller.c" \
  "$ROOT/src/core/xfer_budget.c" \
  "$ROOT/src/core/xfer_task.c" \
  "$ROOT/src/core/xfer_task_history.c" \
  "$ROOT/src/core/clock.c" \
  "$ROOT/src/core/xfer_rate.c" \
  "$ROOT/src/core/xfer_ack_window.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_buffer_pool.c" \
  "$ROOT/src/core/xfer_rx_queue.c" \
  "$ROOT/src/core/xfer_rx_core.c" \
  "$ROOT/src/core/session.c"
"$BUILD/hopdesk-core-tests"
python3 "$ROOT/scripts/test-regressions.py"
python3 "$ROOT/scripts/test-macos-input-permissions.py"
python3 "$ROOT/scripts/test-display-switch.py"
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-resilience-tests" "$ROOT/tests/resilience_tests.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/clock.c" "$ROOT/src/core/log.c" "$ROOT/src/core/session.c" "$ROOT/src/core/watchdog.c"
"$BUILD/hopdesk-resilience-tests" "$BUILD/resilience-test.log"

python3 - "$BUILD/hopdesk-resilience-tests" <<'PYTEST'
import subprocess,sys
for mode,expected in [("--watchdog-progress",0),("--watchdog-hang",70)]:
    result=subprocess.run([sys.argv[1],mode],timeout=5)
    assert result.returncode==expected,(mode,result.returncode)
print("PASS: watchdog tolerates progress and exits 70 on an injected hang")
PYTEST

python3 "$ROOT/scripts/test-updates.py"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/mac" -o "$BUILD/hopdesk-writer-io-tests" \
  "$ROOT/tests/writer_io_tests.c" "$ROOT/src/mac/xfer_writer_io.c"
"$BUILD/hopdesk-writer-io-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-queue-tests" \
  "$ROOT/tests/xfer_rx_queue_tests.c" "$ROOT/src/core/xfer_rx_queue.c"
"$BUILD/hopdesk-rx-queue-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -I "$ROOT/src/mac" \
  -o "$BUILD/hopdesk-writer-worker-tests" \
  "$ROOT/tests/writer_worker_tests.c" "$ROOT/src/mac/xfer_writer_worker.c"
"$BUILD/hopdesk-writer-worker-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -I "$ROOT/src/mac" \
  -o "$BUILD/hopdesk-sender-threads-tests" \
  "$ROOT/tests/mac_sender_threads_tests.c" "$ROOT/src/mac/xfer_sender_threads.c"
"$BUILD/hopdesk-sender-threads-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" \
  -include "$ROOT/tests/writer_pool_alloc_override.h" \
  -c "$ROOT/src/core/xfer_buffer_pool.c" -o "$BUILD/writer-buffer-pool-test.o"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/mac" -include "$ROOT/tests/writer_io_fault_override.h" \
  -c "$ROOT/src/mac/xfer_writer_io.c" -o "$BUILD/writer-io-fault-test.o"
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -I "$ROOT/src/mac" -o "$BUILD/hopdesk-writer-queue-tests" \
  "$ROOT/tests/mac_writer_queue_tests.c" "$ROOT/src/mac/xfer_writer_queue.c" \
  "$BUILD/writer-io-fault-test.o" "$ROOT/src/mac/xfer_writer_worker.c" \
  "$BUILD/writer-buffer-pool-test.o" "$ROOT/src/core/xfer_rx_queue.c" \
  "$ROOT/src/core/xfer_budget.c"
"$BUILD/hopdesk-writer-queue-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -I "$ROOT/src/mac" -o "$BUILD/hopdesk-ack-coordinator-tests" \
  "$ROOT/tests/xfer_ack_coordinator_tests.c" "$ROOT/src/mac/xfer_ack_coordinator.c" \
  "$ROOT/src/core/xfer_ack_window.c"
"$BUILD/hopdesk-ack-coordinator-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -I "$ROOT/src/mac" -o "$BUILD/hopdesk-scanner-tests" \
  "$ROOT/tests/xfer_scanner_tests.c" "$ROOT/src/mac/xfer_scanner.c"
"$BUILD/hopdesk-scanner-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-core-tests" \
  "$ROOT/tests/xfer_rx_core_tests.c" "$ROOT/src/core/xfer_rx_core.c" \
  "$ROOT/src/core/xfer_validate.c" "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/crc32.c" "$ROOT/src/core/session.c"
"$BUILD/hopdesk-rx-core-tests"

clang -std=c11 -Wall -Wextra -Werror -D_XOPEN_SOURCE=700 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/mac" -o "$BUILD/hopdesk-rx-fs-tests" \
  "$ROOT/tests/xfer_rx_fs_tests.c" "$ROOT/src/mac/xfer_rx_fs.c"
"$BUILD/hopdesk-rx-fs-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-context-tests" \
  "$ROOT/tests/xfer_sender_context_tests.c"
"$BUILD/hopdesk-sender-context-tests"

SENDER_CTX_DEPS="$ROOT/src/core/xfer_sender_context.c $ROOT/src/core/xfer_rate.c $ROOT/src/core/xfer_budget.c $ROOT/src/core/protocol.c $ROOT/src/core/session.c $ROOT/src/core/clock.c $ROOT/src/core/watchdog.c $ROOT/src/core/crc32.c"
SENDER_WORKER_DEPS="$ROOT/src/core/xfer_sender_worker.c $SENDER_CTX_DEPS $ROOT/src/core/xfer_task.c $ROOT/src/core/log.c"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-ack-wait-tests" \
  "$ROOT/tests/xfer_sender_ack_wait_tests.c" "$ROOT/src/core/xfer_sender_engine_adapter.c" \
  "$ROOT/src/core/xfer_sender_engine.c" "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-ack-wait-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-chunk-tests" \
  "$ROOT/tests/xfer_sender_chunk_tests.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-chunk-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-queue-tests" \
  "$ROOT/tests/xfer_sender_queue_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-queue-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-scheduler-tests" \
  "$ROOT/tests/xfer_sender_scheduler_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-scheduler-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-active-job-tests" \
  "$ROOT/tests/xfer_sender_active_job_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-active-job-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-context-adapter-tests" \
  "$ROOT/tests/xfer_sender_context_adapter_tests.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-context-adapter-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-retry-tests" \
  "$ROOT/tests/xfer_sender_retry_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-retry-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-lifecycle-tests" \
  "$ROOT/tests/xfer_sender_lifecycle_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-lifecycle-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-sender-job-lifecycle-tests" \
  "$ROOT/tests/xfer_sender_job_lifecycle_tests.c" \
  "$ROOT/src/core/xfer_sender_worker.c" "$ROOT/src/core/log.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-job-lifecycle-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-worker-flow-tests" \
  "$ROOT/tests/xfer_sender_worker_flow_tests.c" $SENDER_WORKER_DEPS
"$BUILD/hopdesk-sender-worker-flow-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-worker-tests" \
  "$ROOT/tests/xfer_sender_worker_tests.c" "$ROOT/src/core/xfer_sender_worker.c" \
  "$ROOT/src/core/xfer_task.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-worker-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-engine-tests" \
  "$ROOT/tests/xfer_sender_engine_tests.c" "$ROOT/src/core/xfer_sender_engine.c" \
  "$ROOT/src/mac/xfer_ack_coordinator.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-sender-engine-tests"

# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-engine-adapter-tests" \
  "$ROOT/tests/xfer_sender_engine_adapter_tests.c" "$ROOT/src/core/xfer_sender_engine_adapter.c" \
  "$ROOT/src/core/xfer_sender_engine.c" "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" $SENDER_CTX_DEPS
"$BUILD/hopdesk-sender-engine-adapter-tests"

clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-image-tests" \
  "$ROOT/tests/xfer_sender_image_tests.c" "$ROOT/src/core/xfer_sender_engine.c" \
  "$ROOT/src/mac/xfer_ack_coordinator.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-sender-image-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-file-tests" \
  "$ROOT/tests/xfer_sender_file_tests.c" "$ROOT/src/core/xfer_sender_engine.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-sender-file-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-sender-commit-tests" \
  "$ROOT/tests/xfer_sender_commit_tests.c" "$ROOT/src/core/xfer_sender_engine.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_ack_window.c" "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-sender-commit-tests"


clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-clipboard-txn-tests" \
  "$ROOT/tests/xfer_clipboard_txn_tests.c" "$ROOT/src/core/xfer_clipboard_txn.c"
"$BUILD/hopdesk-clipboard-txn-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" -o "$BUILD/hopdesk-mouse-click-tests" "$ROOT/tests/mouse_click_tests.c" "$ROOT/src/core/mouse_click.c"
"$BUILD/hopdesk-mouse-click-tests"
