#!/bin/sh
# Runs a script written from Windows (CRLF) inside WSL: run.sh <script> [args...]
# The script runs from a temporary copy, so the project root is passed as SG_ROOT.
SG_ROOT=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
export SG_ROOT
s="$1"; shift
t=$(mktemp)
tr -d '\r' < "$s" > "$t"
bash "$t" "$@" < /dev/null
rc=$?
rm -f "$t"
exit $rc
