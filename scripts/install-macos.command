#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/HopDesk.app"
if [[ ! -d "$APP" || ! -x "$APP/Contents/MacOS/HopDesk" ]]; then
  echo "HopDesk.app must be next to this installer. Extract the complete ZIP first." >&2
  if [[ -t 0 ]]; then read -r -p "Press Return to close..."; fi
  exit 1
fi

if [[ -n "${HOPDESK_INSTALL_DIR:-}" ]]; then
  INSTALL_DIR="$HOPDESK_INSTALL_DIR"
elif [[ -d /Applications && -w /Applications ]]; then
  INSTALL_DIR="/Applications"
else
  INSTALL_DIR="$HOME/Applications"
fi
mkdir -p "$INSTALL_DIR"
DEST="$INSTALL_DIR/HopDesk.app"
LEGACY_DEST="$HOME/Applications/HopDesk.app"
STAGE="$INSTALL_DIR/.HopDesk.app.staging.$$"
DOMAIN="gui/$(id -u)"
AGENT_PLIST="$HOME/Library/LaunchAgents/com.hopdesk.agent.plist"
AGENT_WAS_LOADED=0

cleanup() {
  rm -rf "$STAGE"
}
trap cleanup EXIT

detect_signing_identity() {
  local id=""
  if [[ -n "${HOPDESK_CODESIGN_IDENTITY:-}" ]]; then
    printf '%s\n' "$HOPDESK_CODESIGN_IDENTITY"
    return 0
  fi
  if [[ -d "$DEST" ]]; then
    id=$(/usr/bin/codesign -d -r- "$DEST" 2>&1 \
      | /usr/bin/sed -n 's/.*certificate leaf = H"\([0-9A-Fa-f]\{40\}\)".*/\1/p' \
      | /usr/bin/head -n 1 || true)
    if [[ -n "$id" ]]; then
      printf '%s\n' "$id"
      return 0
    fi
  fi
  # Fall back to an existing local HopDesk signing certificate in the keychain,
  # preferring valid identities first and skipping certs with invalid key usage.
  id=$(/usr/bin/security find-identity -v -p codesigning 2>/dev/null \
    | /usr/bin/awk '/"HopDesk Local Signing"/ && $0 !~ /Invalid Key Usage/ { print $2; exit }' || true)
  if [[ -z "$id" ]]; then
    id=$(/usr/bin/security find-identity -p codesigning 2>/dev/null \
      | /usr/bin/awk '/"HopDesk Local Signing"/ && $0 !~ /Invalid Key Usage/ { print $2; exit }' || true)
  fi
  if [[ -n "$id" ]]; then
    printf '%s\n' "$id"
  fi
}

SIGN_ID="$(detect_signing_identity)"

# Prepare and verify the staged bundle before touching the live installation.
rm -rf "$STAGE"
/usr/bin/ditto "$APP" "$STAGE"
/usr/bin/xattr -dr com.apple.quarantine "$STAGE" 2>/dev/null || true

if [[ -n "$SIGN_ID" && "$SIGN_ID" != "-" ]]; then
  if /usr/bin/codesign --force --deep --sign "$SIGN_ID" "$STAGE" >/dev/null 2>&1 && \
     /usr/bin/codesign --verify --deep --strict "$STAGE" >/dev/null 2>&1; then
    echo "Signed HopDesk with stable identity $SIGN_ID (macOS permissions preserved)."
  else
    echo "Warning: unable to sign with identity $SIGN_ID; keeping package signature." >&2
    SIGN_ID=""
  fi
fi

if [[ "${HOPDESK_SKIP_STOP:-0}" != "1" ]]; then
  if /bin/launchctl print "$DOMAIN/com.hopdesk.agent" >/dev/null 2>&1; then
    AGENT_WAS_LOADED=1
    /bin/launchctl bootout "$DOMAIN/com.hopdesk.agent" 2>/dev/null || true
  fi
  /bin/launchctl bootout "$DOMAIN/com.hopdesk.split-agent" 2>/dev/null || true
  if /usr/bin/pgrep -x HopDesk >/dev/null 2>&1; then
    echo "Stopping running HopDesk..."
    /usr/bin/pkill -x HopDesk 2>/dev/null || true
    for _ in $(seq 1 20); do
      /usr/bin/pgrep -x HopDesk >/dev/null 2>&1 || break
      sleep 0.25
    done
    if /usr/bin/pgrep -x HopDesk >/dev/null 2>&1; then
      /usr/bin/pkill -9 -x HopDesk 2>/dev/null || true
      sleep 0.5
    fi
  fi
fi

# Replace only the application bundle. User configuration in
# ~/Library/Application Support/HopDesk is intentionally untouched.
if [[ -d "$DEST" ]]; then
  rm -rf "$DEST"
fi
mv "$STAGE" "$DEST"

if [[ "$DEST" != "$LEGACY_DEST" && -d "$LEGACY_DEST" ]]; then
  rm -rf "$LEGACY_DEST"
  echo "Removed duplicate legacy bundle: $LEGACY_DEST"
fi

# Keep an existing per-user LaunchAgent pointed at the canonical app path.
if [[ -f "$AGENT_PLIST" && "${HOPDESK_SKIP_STOP:-0}" != "1" ]]; then
  CURRENT_BIN=$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' "$AGENT_PLIST" 2>/dev/null || true)
  if [[ "$CURRENT_BIN" == "$LEGACY_DEST/Contents/MacOS/HopDesk" ]]; then
    /usr/libexec/PlistBuddy -c "Set :ProgramArguments:0 $DEST/Contents/MacOS/HopDesk" "$AGENT_PLIST" 2>/dev/null || true
  fi
  if [[ "$AGENT_WAS_LOADED" == "1" ]]; then
    /bin/launchctl bootstrap "$DOMAIN" "$AGENT_PLIST" 2>/dev/null || true
  fi
fi

echo "Installed HopDesk to $DEST"
echo "Your settings and logs in ~/Library/Application Support/HopDesk were preserved."
if [[ -z "$SIGN_ID" ]]; then
  echo "Note: ad-hoc signature used; if Accessibility or Input Monitoring stops working, remove old HopDesk entries and re-add $DEST."
fi

if [[ "${HOPDESK_NO_LAUNCH:-0}" != "1" && "$AGENT_WAS_LOADED" != "1" ]]; then
  /usr/bin/open "$DEST"
fi
