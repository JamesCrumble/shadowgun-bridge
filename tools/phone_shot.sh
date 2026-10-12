#!/bin/sh
# Screenshot of the phone into logs/phone_<name>.png. Usage: phone_shot.sh [name]
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
"$P/tools/adb" exec-out screencap -p > "$P/logs/phone_${1:-shot}.png"
ls -la "$P/logs/phone_${1:-shot}.png"
