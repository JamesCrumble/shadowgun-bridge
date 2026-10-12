#!/bin/bash
# build + install + launch + wait N seconds + pull log. Usage: iter.sh [wait_s]
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
"$P/tools/build_install.sh" || exit 1
"$P/tools/launch.sh" >/dev/null 2>&1
sleep "${1:-45}"
"$P/tools/pull_log.sh"
grep -aE "FATAL|unresolved|qemu exited|SIG|Segmentation|abort|phase=" "$P/logs/phone.log" | tail -${2:-12} | cut -c1-220
