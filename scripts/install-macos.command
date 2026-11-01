#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/HopDesk.app"
DEST="$HOME/Applications/HopDesk.app"
if [[ ! -d "$APP" ]]; then
  echo "HopDesk.app must be next to this installer. Extract the complete ZIP first."
  read -r -p "Press Return to close..."
  exit 1
fi
mkdir -p "$HOME/Applications"
if [[ -d "$DEST" ]]; then
  # Replace only the application bundle. User configuration in
  # ~/Library/Application Support/HopDesk is intentionally untouched.
  rm -rf "$DEST"
fi
ditto "$APP" "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
echo "Installed HopDesk to $DEST"
echo "Your settings and logs in ~/Library/Application Support/HopDesk were preserved."
open "$DEST"
