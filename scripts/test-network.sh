#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
clang -std=c11 -Wall -Wextra -I "$ROOT/src/core" -o "$BUILD/hopdesk-network-tests" "$ROOT/tests/network_tests.c" "$ROOT/src/core/protocol.c"
"$BUILD/hopdesk-network-tests"
