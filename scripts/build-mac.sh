#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
APP="$BUILD/HopDesk.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"
ICONSET="$BUILD/HopDesk.iconset"
PACKAGE="$BUILD/macos-package"
DEFAULT_VERSION="0.1.0"
if [ -f "$ROOT/VERSION" ]; then
    DEFAULT_VERSION="$(tr -d ' \t\r\n' < "$ROOT/VERSION")"
fi
VERSION="${HOPDESK_VERSION:-$DEFAULT_VERSION}"
if ! printf '%s' "$VERSION" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-dev\.[1-9][0-9]*)?$'; then
    echo "Invalid HOPDESK_VERSION: $VERSION (expected MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-dev.N)" >&2
    exit 1
fi
CHANNEL="stable"
case "$VERSION" in
    *-dev.*) CHANNEL="dev" ;;
esac
VERSION_HEADER="$BUILD/hop_version.h"
printf '#define HOP_VERSION "%s"\n#define HOP_CHANNEL "%s"\n' "$VERSION" "$CHANNEL" > "$VERSION_HEADER"

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

CORE=$(find "$ROOT/src/core" -type f -name '*.c' -print | sort)
MAC_C=$(find "$ROOT/src/mac" -type f -name '*.c' -print | sort)
MAC_M=$(find "$ROOT/src/mac" -type f -name '*.m' -print | sort)
PLATFORM=$(find "$ROOT/src/platform" -type f -name '*.c' -print | sort)
CUI_ROOT="$ROOT/third_party/cui"
if [ ! -f "$CUI_ROOT/CMakeLists.txt" ]; then
    echo "CUI submodule is missing. Run: git submodule update --init --recursive" >&2
    exit 1
fi
CUI_CORE=$(find "$CUI_ROOT/src" -maxdepth 1 -type f -name 'cui_*.c' ! -name 'cui_bidi.c' ! -name 'cui_shape.c' -print | sort)
CUI_RENDER_C="$CUI_ROOT/src/render/cui_render_sw.c $CUI_ROOT/src/render/cui_render_gl.c"
CUI_PLATFORM_C="$CUI_ROOT/src/platform/cui_app.c $CUI_ROOT/src/platform/cui_dialogs.c"
# HopDesk is an event-driven utility, not a continuously rendering game.
# Cap drawable residency at two without changing Retina scale or CUI checkout.
# Set HOPDESK_METAL_DRAWABLE_COUNT=3 for an otherwise identical A/B build.
METAL_DRAWABLE_COUNT="${HOPDESK_METAL_DRAWABLE_COUNT:-2}"
case "$METAL_DRAWABLE_COUNT" in
    2|3) ;;
    *) echo "HOPDESK_METAL_DRAWABLE_COUNT must be 2 or 3." >&2; exit 1 ;;
esac
METAL_UPSTREAM="$CUI_ROOT/src/render/cui_render_metal.m"
if [ "$(grep -Fc 'layer.maximumDrawableCount = 3;' "$METAL_UPSTREAM")" -ne 1 ]; then
    echo "CUI Metal drawable initialization changed; review the HopDesk override." >&2
    exit 1
fi
CUI_RENDER_M="$BUILD/hopdesk_cui_render_metal.m"
sed "s/layer.maximumDrawableCount = 3;/layer.maximumDrawableCount = $METAL_DRAWABLE_COUNT;/" "$METAL_UPSTREAM" > "$CUI_RENDER_M"
echo "HopDesk Metal maximum drawable count: $METAL_DRAWABLE_COUNT"
CUI_PLATFORM_M="$CUI_ROOT/src/platform/cui_app_cocoa.m"
UI_C="$ROOT/src/ui/hopdesk_ui.c"
if [ -z "$CORE$MAC_C$MAC_M" ]; then echo "No HopDesk source files found." >&2; exit 1; fi
INC="$ROOT/src/core"
FRAMEWORKS="-framework Cocoa -framework ApplicationServices -framework CoreGraphics -framework IOKit -framework CoreAudio -framework AudioToolbox -framework QuartzCore -framework OpenGL -framework Metal"
# shellcheck disable=SC2086
clang -O2 -Wall -Wextra -fobjc-arc -DCUI_METAL -include "$VERSION_HEADER" -I "$INC" -I "$CUI_ROOT/include" -I "$CUI_ROOT/src" -o "$MACOS/HopDesk" $CORE $PLATFORM "$UI_C" $MAC_C $MAC_M $CUI_CORE $CUI_RENDER_C $CUI_PLATFORM_C $CUI_RENDER_M $CUI_PLATFORM_M $FRAMEWORKS

cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>HopDesk</string>
<key>CFBundleIdentifier</key><string>com.hopdesk.app</string>
<key>CFBundleName</key><string>HopDesk</string>
<key>CFBundleDisplayName</key><string>HopDesk</string>
<key>CFBundleIconFile</key><string>HopDesk</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$VERSION</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
<key>NSLocalNetworkUsageDescription</key><string>HopDesk 连接同一局域网中的设备，以共享键鼠、剪贴板、文件和系统声音。</string>
<key>NSAppleEventsUsageDescription</key><string>HopDesk 读取访达当前文件夹，以便在按下粘贴键的位置接收文件。</string>
<key>NSAudioCaptureUsageDescription</key><string>启用发送本机系统声音时，HopDesk 将系统声音传到已配对的设备。</string>
</dict></plist>
PLIST

cp "$ROOT/scripts/update-macos.command" "$RESOURCES/update-macos.command"
chmod +x "$RESOURCES/update-macos.command"

SIGN_IDENTITY="${HOPDESK_CODESIGN_IDENTITY:--}"
if [ "$SIGN_IDENTITY" = - ]; then
    echo 'Note: ad-hoc signing ties macOS privacy grants to this build; set HOPDESK_CODESIGN_IDENTITY to a stable signing identity for installed builds.' >&2
fi
codesign --force --deep --sign "$SIGN_IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"
plutil -lint "$CONTENTS/Info.plist"
test -s "$RESOURCES/HopDesk.icns"
test -s "$RESOURCES/hopdesk.png"
test -x "$MACOS/HopDesk"

cp "$APP/Contents/MacOS/HopDesk" "$BUILD/hopdesk"
cp -R "$APP" "$PACKAGE/HopDesk.app"
cp "$ROOT/scripts/install-macos.command" "$PACKAGE/install-macos.command"
cp "$ROOT/scripts/install-launchagent.command" "$PACKAGE/install-launchagent.command"
chmod +x "$PACKAGE/install-macos.command" "$PACKAGE/install-launchagent.command"
ditto -c -k --sequesterRsrc --keepParent "$PACKAGE" "$BUILD/HopDesk-macOS.zip"
echo "Built app: $APP"
echo "Built installer archive: $BUILD/HopDesk-macOS.zip"
