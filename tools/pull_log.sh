#!/bin/sh
# Copies the phone's game log to ~/mass-effect-infiltrator-probe/logs/phone.log
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A="$P/tools/adb"
D=/sdcard/BridgeGames/com.jamescrumble.shadowgun
[ -n "$($A shell ls $D/shadowgun.log 2>/dev/null)" ] || D=/sdcard/Android/data/com.jamescrumble.shadowgun/files
$A shell cat $D/shadowgun.log | tr -d '\r' > "$P/logs/phone.log"
wc -l "$P/logs/phone.log"
