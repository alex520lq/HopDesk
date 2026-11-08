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
  "$ROOT/src/core/xfer_rx_queue.c"
"$BUILD/hopdesk-core-tests"
python3 "$ROOT/scripts/test-regressions.py"
clang -std=c11 -Wall -Wextra -Werror -pthread -I "$ROOT/src/core" \
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
