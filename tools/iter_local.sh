#!/bin/bash
# Build, run locally for N seconds, print a digest of the log (logs/local.log has everything).
P=$(cd "$(dirname "$0")/.." && pwd); cd "$P"
./build.sh > logs/build.log 2>&1
grep -E "error:|undefined reference|No such file" logs/build.log | cut -c1-260 | sort -u | head -20
tools/run_local.sh "${1:-90}" 2>&1 | tail -1
grep -E "^JNI unhandled|unresolved symbol|^dlopen|Unknown symbol|what\(\)|LOG\[" logs/local.log | sort | uniq -c | sort -rn | head -${2:-40} | cut -c1-260
echo "--- last lines"
grep -vE "^Missing:|sem_init|\^ pthread_t|^ +(\[|r[0-9]|ip|sp) |^JNI unhandled" logs/local.log | awk '/FATAL/{f=1} {print}' | tail -${3:-12} | cut -c1-200
