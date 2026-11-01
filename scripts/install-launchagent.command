#!/bin/sh
# Opt-in per-user LaunchAgent. Invoke after installing ~/Applications/HopDesk.app and quitting the app.
set -eu
LABEL=com.hopdesk.agent
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"
if [ "${1:-}" = "--uninstall" ]; then
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    rm -f "$PLIST"
    echo "HopDesk LaunchAgent removed."
    exit 0
fi
APP="${1:-$HOME/Applications/HopDesk.app}"
[ -d "$APP" ] || { echo "Not an installed HopDesk app: $APP" >&2; exit 1; }
APP=$(cd "$APP" && pwd -P)
if /usr/bin/pgrep -x HopDesk >/dev/null 2>&1; then
    echo "Quit the running HopDesk from its menu, then run this installer again." >&2
    exit 1
fi
BIN="$APP/Contents/MacOS/HopDesk"
[ -x "$BIN" ] || { echo "Not an installed HopDesk app: $APP" >&2; exit 1; }
mkdir -p "$HOME/Library/LaunchAgents"
# plutil writes the executable as a string, including spaces / XML metacharacters.
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
TARGET="$TEMP_DIR/agent.plist"
/usr/bin/plutil -create xml1 "$TARGET"
/usr/bin/plutil -insert Label -string "$LABEL" "$TARGET"
/usr/bin/plutil -insert ProgramArguments -xml '<array/>' "$TARGET"
/usr/bin/plutil -insert ProgramArguments.0 -string "$BIN" "$TARGET"
/usr/bin/plutil -insert RunAtLoad -bool true "$TARGET"
/usr/bin/plutil -insert KeepAlive -xml '<dict><key>SuccessfulExit</key><false/></dict>' "$TARGET"
/usr/bin/plutil -insert ThrottleInterval -integer 5 "$TARGET"
chmod 600 "$TARGET"
mv -f "$TARGET" "$PLIST"
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
launchctl bootstrap "$DOMAIN" "$PLIST"
echo "Installed and started. Abnormal exits restart; normal menu exit stops until next sign-in."
