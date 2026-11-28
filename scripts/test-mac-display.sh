#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build"
clang -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  "$ROOT/tests/mac_display_tests.c" "$ROOT/src/core/protocol.c" \
  "$ROOT/src/core/display_switch.c" "$ROOT/src/core/clock.c" "$ROOT/src/core/log.c" "$ROOT/src/core/session.c" \
  -framework ApplicationServices -framework IOKit -framework CoreFoundation \
  -o "$ROOT/build/hopdesk-mac-display-tests"
"$ROOT/build/hopdesk-mac-display-tests"
