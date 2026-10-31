#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -I "$ROOT/src/core" -o "$BUILD/hopdesk-xfer-protocol-tests" \
  "$ROOT/tests/xfer_protocol_tests.c" "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/crc32.c" "$ROOT/src/core/xfer_validate.c"
"$BUILD/hopdesk-xfer-protocol-tests"
