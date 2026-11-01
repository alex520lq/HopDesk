#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
APP="$BUILD/HopDesk.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"
ICONSET="$BUILD/HopDesk.iconset"
PACKAGE="$BUILD/macos-package"

rm -rf "$APP" "$ICONSET" "$PACKAGE" "$BUILD/HopDesk-macOS.zip"
mkdir -p "$MACOS" "$RESOURCES" "$ICONSET" "$PACKAGE"

if [ ! -s "$ROOT/assets/appicon/hopdesk.png" ]; then
    echo "Pre-generated app icon missing: assets/appicon/hopdesk.png" >&2
    exit 1
fi
cp "$ROOT/assets/appicon/hopdesk.png" "$RESOURCES/hopdesk.png"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$ROOT/assets/appicon/hopdesk.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    doubled=$((size * 2))
    sips -z "$doubled" "$doubled" "$ROOT/assets/appicon/hopdesk.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$RESOURCES/HopDesk.icns"

CORE=$(find "$ROOT/src/core" -maxdepth 1 -name '*.c' -print | sort)
MAC_C=$(find "$ROOT/src/mac" -maxdepth 1 -name '*.c' -print | sort)
MAC_M=$(find "$ROOT/src/mac" -maxdepth 1 -name '*.m' -print | sort)
if [ -z "$CORE$MAC_C$MAC_M" ]; then echo "No HopDesk source files found." >&2; exit 1; fi
INC="$ROOT/src/core"
FRAMEWORKS="-framework Cocoa -framework ApplicationServices -framework CoreGraphics -framework IOKit"
# shellcheck disable=SC2086
clang -O2 -Wall -Wextra -fobjc-arc -I "$INC" -o "$MACOS/HopDesk" $CORE $MAC_C $MAC_M $FRAMEWORKS

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>HopDesk</string>
<key>CFBundleIdentifier</key><string>com.hopdesk.app</string>
<key>CFBundleName</key><string>HopDesk</string>
<key>CFBundleDisplayName</key><string>HopDesk</string>
<key>CFBundleIconFile</key><string>HopDesk</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>0.5.0</string>
<key>CFBundleShortVersionString</key><string>0.5.0</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
plutil -lint "$CONTENTS/Info.plist"
test -s "$RESOURCES/HopDesk.icns"
test -s "$RESOURCES/hopdesk.png"
test -x "$MACOS/HopDesk"

cp "$APP/Contents/MacOS/HopDesk" "$BUILD/hopdesk"
cp -R "$APP" "$PACKAGE/HopDesk.app"
cp "$ROOT/scripts/install-macos.command" "$PACKAGE/install-macos.command"
chmod +x "$PACKAGE/install-macos.command"
ditto -c -k --sequesterRsrc --keepParent "$PACKAGE" "$BUILD/HopDesk-macOS.zip"
echo "Built app: $APP"
echo "Built installer archive: $BUILD/HopDesk-macOS.zip"
