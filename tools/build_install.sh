#!/bin/bash
# Builds the port and the APK, installs it on the phone (keeps app data).
# Usage: build_install.sh [noinstall]
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
set -euo pipefail
source "$P/env.sh"
"$P/build.sh" > "$P/logs/build.log" 2>&1 || { tail -40 "$P/logs/build.log"; exit 1; }
grep -E "warning|error" "$P/logs/build.log" | grep -vE "^In file" | tail -15 || true
cd "$P"
Q="$P/qemu/out/qemu-arm-${SG_QEMU_VARIANT:-E}.stripped"
if [ -z "${QEMU_BIN+x}" ] && [ -x "$Q" ]; then export QEMU_BIN="$Q"; fi
echo "qemu: ${QEMU_BIN:-?}"
bash apk/build_apk.sh > "$P/logs/apk.log" 2>&1 || { tail -40 "$P/logs/apk.log"; exit 1; }
tail -2 "$P/logs/apk.log"
[ "${1:-}" = noinstall ] && exit 0
"$P/tools/adb" install -r "$(wslpath -w "$P/Shadowgun-Bridge-debug.apk")"
