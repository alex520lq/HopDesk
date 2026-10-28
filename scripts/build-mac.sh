#!/bin/sh
# =====================================================================
#  HopDesk - macOS 构建脚本
#
#  用法：  sh scripts/build-mac.sh
#
#  阶段 1 源码就位后即可运行；当前为立项骨架。
#  ObjC 仅用于状态栏与剪贴板。
# =====================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"

CORE=$(ls "$ROOT"/src/core/*.c 2>/dev/null || true)
MAC_C=$(ls "$ROOT"/src/mac/*.c 2>/dev/null || true)
MAC_M=$(ls "$ROOT"/src/mac/*.m 2>/dev/null || true)

if [ -z "$CORE$MAC_C$MAC_M" ]; then
    echo "暂无源文件（阶段 1 就位后再构建）。"
    exit 0
fi

INC="$ROOT/src/core"
FRAMEWORKS="-framework Cocoa -framework ApplicationServices -framework CoreGraphics -framework IOKit"

clang -O2 -Wall -Wextra -fobjc-arc \
    -I "$INC" \
    -o "$BUILD/hopdesk" \
    $CORE $MAC_C $MAC_M \
    $FRAMEWORKS

echo "构建成功: $BUILD/hopdesk"
