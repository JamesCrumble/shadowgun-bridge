#!/bin/bash
# Asks every thread of the locally running port for its position (DUMP: blocks appear in logs/local.log).
# Usage: tools/thread_dump.sh [only-thread-name-prefix]
P=$(pgrep -f "[q]emu-arm build/shadowgun" | head -1)
[ -z "$P" ] && { echo "not running"; exit 1; }
python3 - "$P" "${1:-}" <<'PY'
import ctypes, os, sys, time
pid = int(sys.argv[1]); want = sys.argv[2]
libc = ctypes.CDLL(None, use_errno=True)
for t in sorted(int(x) for x in os.listdir('/proc/%d/task' % pid)):
    name = open('/proc/%d/task/%d/comm' % (pid, t)).read().strip()
    if name.startswith('llvmpipe') or (want and not name.startswith(want)):
        continue
    libc.syscall(234, pid, t, 12)   # tgkill(pid, tid, SIGUSR2)
    time.sleep(0.15)
PY
