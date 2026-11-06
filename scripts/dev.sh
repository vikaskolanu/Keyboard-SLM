#!/usr/bin/env bash
# Build, install and activate the keyboard on the running emulator or USB phone.
# Usage: scripts/dev.sh [--device]   (--device targets a USB phone, default: emulator)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$HOME/Library/Android/sdk"
ADB="$SDK/platform-tools/adb"
PKG=com.antigravity.keyboardslm.app
IME="$PKG/.KeyboardService"

if [[ "${1:-}" == "--device" ]]; then
  TARGET=$($ADB devices | awk 'NR>1 && $2=="device" && $1 !~ /^emulator/ {print $1; exit}')
  [[ -z "$TARGET" ]] && { echo "No USB phone found. Enable USB debugging and accept the prompt."; exit 1; }
else
  TARGET=$($ADB devices | awk '$1 ~ /^emulator/ && $2=="device" {print $1; exit}')
  if [[ -z "$TARGET" ]]; then
    echo "Starting emulator..."
    nohup "$SDK/emulator/emulator" -avd Medium_Phone_API_35 -no-snapshot-save >/tmp/emu.log 2>&1 &
    $ADB wait-for-device
    until [[ "$($ADB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == "1" ]]; do sleep 2; done
    TARGET=$($ADB devices | awk '$1 ~ /^emulator/ {print $1; exit}')
  fi
fi
A="$ADB -s $TARGET"
echo "Target: $TARGET"

(cd "$ROOT/app/android" && ./gradlew -q assembleDebug)
$A install -r "$ROOT/app/build/app/outputs/flutter-apk/app-debug.apk"
sleep 3
$A shell ime enable "$IME"
$A shell ime set "$IME"
$A shell am start -n "$PKG/.MainActivity" >/dev/null
sleep 2
mkdir -p "$ROOT/scripts/out"
$A exec-out screencap -p > "$ROOT/scripts/out/screen.png"
echo "Screenshot: scripts/out/screen.png"
