#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-core-tests" \
  "$ROOT/tests/core_tests.c" \
  "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/crc32.c" \
  "$ROOT/src/core/config.c" \
  "$ROOT/src/core/keymap.c" \
  "$ROOT/src/core/xfer_validate.c"
"$BUILD/hopdesk-core-tests"
