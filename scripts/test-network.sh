#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" -o "$BUILD/hopdesk-network-tests" "$ROOT/tests/network_tests.c" "$ROOT/src/core/protocol.c"
"$BUILD/hopdesk-network-tests"

clang -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$ROOT/src/core" \
  -o "$BUILD/hopdesk-audio-link-tests" "$ROOT/tests/audio_link_tests.c" "$ROOT/src/platform/audio_link.c" \
  "$ROOT/src/core/audio_protocol.c" "$ROOT/src/core/protocol.c" "$ROOT/src/core/session.c" "$ROOT/src/core/clock.c" "$ROOT/src/core/log.c"
"$BUILD/hopdesk-audio-link-tests"
