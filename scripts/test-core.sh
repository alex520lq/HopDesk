#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-core-tests" \
  "$ROOT/tests/core_tests.c" \
  "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/crc32.c" \
  "$ROOT/src/core/config.c" \
  "$ROOT/src/core/keymap.c" \
  "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/update_manifest.c" \
  "$ROOT/src/core/xfer_budget.c" \
  "$ROOT/src/core/xfer_task.c" \
  "$ROOT/src/core/xfer_task_history.c" \
  "$ROOT/src/core/clock.c" \
  "$ROOT/src/core/xfer_rate.c" \
  "$ROOT/src/core/xfer_ack_window.c" \
  "$ROOT/src/core/xfer_tracking.c" \
  "$ROOT/src/core/xfer_buffer_pool.c" \
  "$ROOT/src/core/xfer_rx_queue.c" \
  "$ROOT/src/core/xfer_rx_batch_rules.c" \
  "$ROOT/src/core/xfer_rx_entry_reservations.c" \
  "$ROOT/src/core/xfer_rx_file_state.c"
"$BUILD/hopdesk-core-tests"
python3 "$ROOT/scripts/test-regressions.py"
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

clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -I "$ROOT/src/mac" -o "$BUILD/hopdesk-writer-queue-tests" \
  "$ROOT/tests/mac_writer_queue_tests.c" "$ROOT/src/mac/xfer_writer_queue.c" \
  "$ROOT/src/mac/xfer_writer_io.c" "$ROOT/src/mac/xfer_writer_worker.c" \
  "$ROOT/src/core/xfer_buffer_pool.c" "$ROOT/src/core/xfer_rx_queue.c" \
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

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-receipt-tests" \
  "$ROOT/tests/xfer_rx_receipt_tests.c" "$ROOT/src/core/xfer_rx_receipt.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-rx-receipt-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-lifecycle-tests" \
  "$ROOT/tests/xfer_rx_lifecycle_tests.c" "$ROOT/src/core/xfer_rx_lifecycle.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-rx-lifecycle-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-batch-rules-tests" \
  "$ROOT/tests/xfer_rx_batch_rules_tests.c" "$ROOT/src/core/xfer_rx_batch_rules.c"
"$BUILD/hopdesk-rx-batch-rules-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-entry-reservations-tests" \
  "$ROOT/tests/xfer_rx_entry_reservations_tests.c" "$ROOT/src/core/xfer_rx_entry_reservations.c"
"$BUILD/hopdesk-rx-entry-reservations-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-file-state-tests" \
  "$ROOT/tests/xfer_rx_file_state_tests.c" "$ROOT/src/core/xfer_rx_file_state.c" "$ROOT/src/core/xfer_validate.c" \
  "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-rx-file-state-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-image-state-tests" \
  "$ROOT/tests/xfer_rx_image_state_tests.c" "$ROOT/src/core/xfer_rx_image_state.c"
"$BUILD/hopdesk-rx-image-state-tests"
clang -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-rx-image-buffer-tests" "$ROOT/tests/xfer_rx_image_buffer_tests.c" "$ROOT/src/core/xfer_rx_image_buffer.c"
"$BUILD/hopdesk-rx-image-buffer-tests"

clang -std=c11 -Wall -Wextra -Werror -D_XOPEN_SOURCE=700 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/mac" -o "$BUILD/hopdesk-rx-fs-tests" \
  "$ROOT/tests/xfer_rx_fs_tests.c" "$ROOT/src/mac/xfer_rx_fs.c"
"$BUILD/hopdesk-rx-fs-tests"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$ROOT/src/core" -o "$BUILD/hopdesk-rx-ack-handler-tests" \
  "$ROOT/tests/xfer_rx_ack_handler_tests.c" "$ROOT/src/core/xfer_rx_ack_handler.c" \
  "$ROOT/src/core/xfer_validate.c" "$ROOT/src/core/protocol.c" "$ROOT/src/core/crc32.c"
"$BUILD/hopdesk-rx-ack-handler-tests"
