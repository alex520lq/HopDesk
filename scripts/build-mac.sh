#!/bin/sh
# Build a native macOS application bundle for Finder/Dock launch.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
APP="$BUILD/HopDesk.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP"
mkdir -p "$MACOS" "$RESOURCES"

CORE=$(find "$ROOT/src/core" -maxdepth 1 -name '*.c' -print | sort)
MAC_C=$(find "$ROOT/src/mac" -maxdepth 1 -name '*.c' -print | sort)
MAC_M=$(find "$ROOT/src/mac" -maxdepth 1 -name '*.m' -print | sort)
if [ -z "$CORE$MAC_C$MAC_M" ]; then
    echo "No HopDesk source files found." >&2
    exit 1
fi

INC="$ROOT/src/core"
FRAMEWORKS="-framework Cocoa -framework ApplicationServices -framework CoreGraphics -framework IOKit"
# shellcheck disable=SC2086
clang -O2 -Wall -Wextra -fobjc-arc -I "$INC" \
    -o "$MACOS/HopDesk" \
    $CORE $MAC_C $MAC_M \
    $FRAMEWORKS

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>HopDesk</string>
    <key>CFBundleIdentifier</key><string>com.hopdesk.app</string>
    <key>CFBundleName</key><string>HopDesk</string>
    <key>CFBundleDisplayName</key><string>HopDesk</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleVersion</key><string>0.5.0</string>
    <key>CFBundleShortVersionString</key><string>0.5.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

# Ad-hoc signing is free and gives the bundle a consistent local signature.
# It is not Developer ID signing and does not notarize the app.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
plutil -lint "$CONTENTS/Info.plist"
test -x "$MACOS/HopDesk"

# Keep a standalone binary for developer diagnostics, but publish the .app ZIP.
cp "$MACOS/HopDesk" "$BUILD/hopdesk"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$BUILD/HopDesk-macOS.zip"
echo "Built app: $APP"
echo "Built archive: $BUILD/HopDesk-macOS.zip"
