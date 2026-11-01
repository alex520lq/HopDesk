#!/bin/bash
set -euo pipefail

ARCHIVE="$1"
TARGET_APP="$2"
APP_PID="$3"
TRANSACTION="$4"
NONCE="$5"
VERSION="$6"
TMP_BASE="$(/usr/bin/printenv TMPDIR 2>/dev/null || true)"
if [[ -z "$TMP_BASE" ]]; then TMP_BASE="/tmp"; fi
LOG="$TMP_BASE/HopDesk-update-error.log"
STAGE="$(mktemp -d "$TMP_BASE/hopdesk-update.XXXXXX")"
NEXT_APP="$TARGET_APP.update.$NONCE"
BACKUP_APP="$TARGET_APP.previous.$NONCE"
READY="$TRANSACTION/ready"
CHILD=""
REPLACED=0
COMMITTED=0

stop_child() {
  if [[ -n "$CHILD" ]]; then
    kill "$CHILD" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$CHILD" 2>/dev/null || break
      sleep 0.25
    done
    if kill -0 "$CHILD" 2>/dev/null; then kill -9 "$CHILD" 2>/dev/null || true; fi
    wait "$CHILD" 2>/dev/null || true
    CHILD=""
  fi
}
cleanup() {
  result=$?
  trap - EXIT
  if [[ "$REPLACED" == 1 && "$COMMITTED" == 0 ]]; then
    stop_child
    # Never remove the only recoverable copy of the previous installation.
    rm -rf "$TARGET_APP" || true
    if mv "$BACKUP_APP" "$TARGET_APP"; then
      open "$TARGET_APP" >/dev/null 2>&1 || true
    else
      printf '%s\n' "Rollback failed. Recover the previous app from $BACKUP_APP" >> "$LOG"
    fi
  fi
  rm -rf "$STAGE" "$NEXT_APP"
  rm -f "$ARCHIVE"
  rm -rf "$TRANSACTION"
  exit "$result"
}
fail() {
  printf '%s\n' "$1" > "$LOG"
  # Do not block rollback on a user-facing modal dialog.
  exit 1
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[[ "$NONCE" =~ ^[0-9a-f]{32}$ ]] || fail "Invalid update transaction."
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Invalid expected version."
[[ "$APP_PID" =~ ^[1-9][0-9]*$ ]] || fail "Invalid previous process."
if [[ ! -f "$ARCHIVE" || ! -d "$TARGET_APP" || ! -d "$TRANSACTION" ]]; then
  fail "Archive, application or transaction directory is missing."
fi

# Validate and prepare before signalling the old application to quit.
unzip -Z1 "$ARCHIVE" > "$STAGE/entries" || fail "Invalid ZIP archive."
while IFS= read -r entry; do
  case "$entry" in
    HopDesk.app/*|install-macos.command|install-launchagent.command|macos-package/*|__MACOSX/*) ;;
    *) fail "Unexpected archive path." ;;
  esac
  case "/$entry/" in *"/../"*|*"/./"*) fail "Unsafe relative archive path." ;; esac
done < "$STAGE/entries"
ditto -x -k "$ARCHIVE" "$STAGE" || fail "Cannot extract update."
NEW_APP="$STAGE/HopDesk.app"
if [[ ! -d "$NEW_APP" && -d "$STAGE/macos-package/HopDesk.app" ]]; then
  NEW_APP="$STAGE/macos-package/HopDesk.app"
fi
[[ -d "$NEW_APP" && -x "$NEW_APP/Contents/MacOS/HopDesk" ]] || fail "Missing app executable."
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$NEW_APP/Contents/Info.plist" 2>/dev/null || true)
BUNDLE_VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$NEW_APP/Contents/Info.plist" 2>/dev/null || true)
[[ "$BUNDLE_ID" == "com.hopdesk.app" && "$BUNDLE_VERSION" == "$VERSION" ]] || fail "Bundle identity/version mismatch."
codesign --verify --deep --strict "$NEW_APP" >/dev/null 2>&1 || fail "Invalid code signature structure."
ditto "$NEW_APP" "$NEXT_APP" || fail "Cannot prepare replacement."
printf '%s\n' "$NONCE" > "$TRANSACTION/prepared.tmp"
mv "$TRANSACTION/prepared.tmp" "$TRANSACTION/prepared"

for _ in $(seq 1 240); do
  kill -0 "$APP_PID" 2>/dev/null || break
  sleep 0.25
done
kill -0 "$APP_PID" 2>/dev/null && fail "Previous app did not exit within 60 seconds."
mv "$TARGET_APP" "$BACKUP_APP" || fail "Cannot back up installed app."
REPLACED=1
mv "$NEXT_APP" "$TARGET_APP" || fail "Cannot install replacement."
# Launch directly so the child receives this transaction's environment and PID.
# The AppKit run loop writes ready only after initialization and a UI timer tick.
HOPDESK_UPDATE_READY="$READY" HOPDESK_UPDATE_NONCE="$NONCE" HOPDESK_UPDATE_VERSION="$VERSION" \
  "$TARGET_APP/Contents/MacOS/HopDesk" > "$TRANSACTION/startup.log" 2>&1 &
CHILD=$!
ACKNOWLEDGED=0
for _ in $(seq 1 180); do
  kill -0 "$CHILD" 2>/dev/null || break
  if [[ -f "$READY" && "$(cat "$READY")" == "$(printf '%s\n%s\n%s' "$NONCE" "$VERSION" "$CHILD")" ]]; then
    ACKNOWLEDGED=1
    break
  fi
  sleep 0.25
done
if [[ "$ACKNOWLEDGED" == 1 ]]; then
  for _ in $(seq 1 20); do
    if ! kill -0 "$CHILD" 2>/dev/null; then ACKNOWLEDGED=0; break; fi
    sleep 0.25
  done
fi
if ! kill -0 "$CHILD" 2>/dev/null; then ACKNOWLEDGED=0; fi
[[ "$ACKNOWLEDGED" == 1 ]] || fail "New app did not confirm readiness; restoring previous version."
COMMITTED=1
# The acknowledged child no longer needs the temporary environment/files.
rm -rf "$BACKUP_APP"
