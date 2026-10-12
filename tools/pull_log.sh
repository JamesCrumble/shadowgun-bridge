#!/bin/sh
# Copies the phone's game log to ~/mass-effect-infiltrator-probe/logs/phone.log
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
"$P/tools/adb" shell cat /sdcard/Android/data/com.eaprules.shadowgun/files/shadowgun.log | tr -d '\r' > "$P/logs/phone.log"
wc -l "$P/logs/phone.log"
