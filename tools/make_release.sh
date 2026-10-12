#!/bin/bash
# Builds a release bundle in release/<version>/ (git-ignored):
#   Shadowgun-Bridge-<v>.apk      release-signed APK (not debuggable)
#   Shadowgun-Bridge-<v>-src.zip  source of the committed HEAD
#   INSTALL.txt, SHA256SUMS
# The game's own files are the player's and are never part of a release.
# Usage: tools/make_release.sh   (run from WSL; commit first - the source zip is HEAD)
set -euo pipefail
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
source "$P/env.sh"
cd "$P"
Q="$P/qemu/out/qemu-arm-${SG_QEMU_VARIANT:-E}.stripped"
VER=$(grep -o 'versionName="[^"]*"' apk/AndroidManifest.xml | head -1 | cut -d'"' -f2)
OUT="$P/release/$VER"
rm -rf "$OUT"; mkdir -p "$OUT"
"$P/build.sh" > logs/build.log 2>&1
QEMU_BIN="$Q" RELEASE=1 bash apk/build_apk.sh > logs/apk_release.log 2>&1
tail -2 logs/apk_release.log
mv "Shadowgun-Bridge-$VER.apk" "$OUT/"
rm -f "Shadowgun-Bridge-$VER.apk.idsig"
git archive --format=zip --prefix="Shadowgun-Bridge-$VER/" -o "$OUT/Shadowgun-Bridge-$VER-src.zip" HEAD
cp tools/release_install.txt "$OUT/INSTALL.txt"
(cd "$OUT" && sha256sum * > SHA256SUMS)
ls -la "$OUT"
