#!/bin/bash
set -euo pipefail

ARCHIVE="$1"
TARGET_APP="$2"
APP_PID="$3"
TMP_BASE="$(/usr/bin/printenv TMPDIR 2>/dev/null || true)"
if [[ -z "$TMP_BASE" ]]; then TMP_BASE="/tmp"; fi
LOG="$TMP_BASE/HopDesk-update-error.log"
STAGE="$(mktemp -d "$TMP_BASE/hopdesk-update.XXXXXX")"
NEXT_APP="$TARGET_APP.update.$$"
BACKUP_APP="$TARGET_APP.previous"

cleanup() {
  rm -rf "$STAGE"
  rm -f "$0"
}
fail() {
  printf '%s\n' "$1" > "$LOG"
  osascript -e "display dialog \"HopDesk 更新失败：$1\n\n旧版本未被主动删除。日志：$LOG\" buttons {\"好\"} default button 1 with icon stop" >/dev/null 2>&1 || true
  exit 1
}
trap cleanup EXIT

if [[ -z "$ARCHIVE" || -z "$TARGET_APP" || -z "$APP_PID" ||
      ! -f "$ARCHIVE" || ! -d "$TARGET_APP" ]]; then
  fail "更新包或当前应用不存在。"
fi

# The updater is copied to /tmp before HopDesk exits, so replacing the .app
# bundle cannot remove the script that is currently performing the update.
for _ in $(seq 1 240); do
  if ! kill -0 "$APP_PID" 2>/dev/null; then
    break
  fi
  sleep 0.25
done
if kill -0 "$APP_PID" 2>/dev/null; then
  fail "HopDesk 未能在 60 秒内退出，已取消替换。"
fi

# Reject unexpected archive paths before extraction; the package is expected to
# contain only the app bundle, its two installer helpers, and ditto resource-fork data.
if ! unzip -Z1 "$ARCHIVE" >/dev/null 2>&1; then
  fail "更新包不是有效的 ZIP 文件。"
fi
while IFS= read -r entry; do
  case "$entry" in
    HopDesk.app/*|install-macos.command|install-launchagent.command|macos-package/*|__MACOSX/*) ;;
    *) fail "更新包包含不允许的文件路径。" ;;
  esac
  case "/$entry/" in
    *"/../"*|*"/./"*) fail "更新包包含不安全的相对路径。" ;;
  esac
done < <(unzip -Z1 "$ARCHIVE")

ditto -x -k "$ARCHIVE" "$STAGE" || fail "无法解压更新包。"
NEW_APP="$STAGE/HopDesk.app"
if [[ ! -d "$NEW_APP" && -d "$STAGE/macos-package/HopDesk.app" ]]; then
  NEW_APP="$STAGE/macos-package/HopDesk.app"
fi
if [[ ! -d "$NEW_APP" || ! -x "$NEW_APP/Contents/MacOS/HopDesk" ]]; then
  fail "更新包中缺少有效的 HopDesk.app。"
fi

BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$NEW_APP/Contents/Info.plist" 2>/dev/null || true)
if [[ "$BUNDLE_ID" != "com.hopdesk.app" ]]; then
  fail "应用标识不匹配，拒绝替换。"
fi
codesign --verify --deep --strict "$NEW_APP" >/dev/null 2>&1 || fail "新应用的代码签名结构校验失败。"

TARGET_DIR="$(dirname "$TARGET_APP")"
ditto "$NEW_APP" "$NEXT_APP" || fail "无法准备新应用。"
if [[ -e "$BACKUP_APP" ]]; then
  rm -rf "$BACKUP_APP" || fail "无法清理上一次更新备份。"
fi
mv "$TARGET_APP" "$BACKUP_APP" || fail "无法备份当前应用。"
if ! mv "$NEXT_APP" "$TARGET_APP"; then
  mv "$BACKUP_APP" "$TARGET_APP" || true
  fail "替换失败，已尝试恢复旧版本。"
fi

rm -f "$ARCHIVE"
open "$TARGET_APP" || fail "更新已安装，但无法自动启动。请手动打开 HopDesk。"
rm -rf "$BACKUP_APP"
