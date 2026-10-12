#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-$ROOT/android-sdk}"
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
OUT="$ROOT/apk/build/apk"
# QEMU_BIN: the static aarch64 qemu-arm to ship. Default: our patched build (../qemu/out, tools/qemu_build.sh),
# else Debian's qemu-user-static.
QEMU="${QEMU_BIN:-$ROOT/qemu/out/qemu-arm-D.stripped}"

if [ ! -x "$BT/aapt2" ] || [ ! -f "$PLATFORM" ]; then
    echo "Android SDK build-tools 34.0.0 and platform 34 are required" >&2
    exit 1
fi
if [ -n "${JAVA_HOME:-}" ]; then
    PATH="$JAVA_HOME/bin:$PATH"
fi
if ! command -v javac >/dev/null; then
    echo "javac not found; install a JDK or set JAVA_HOME" >&2
    exit 1
fi
if [ ! -x "$QEMU" ]; then
    echo "Missing $QEMU" >&2
    exit 1
fi

cd "$ROOT"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/gen" "$OUT/assets" "$OUT/res"

if [ ! -f build/shadowgun ]; then
    echo "build/shadowgun is missing; build the port first" >&2
    exit 1
fi

if [ ! -d apk/build/sysroot ]; then
    bash apk/make_sysroot.sh
fi

cp apk/AndroidManifest.xml "$OUT/AndroidManifest.xml"
cp -a apk/res/. "$OUT/res/"
# The launcher icon is the game's own, taken from the player's APK at build time (never checked in).
GAME_APK="$(ls gamebin/*Anniversary*.apk gamebin/*.apk 2>/dev/null | head -1 || true)"
if [ -n "$GAME_APK" ] && unzip -o -q -j "$GAME_APK" 'res/drawable-xxxhdpi-v4/app_icon.png' -d "$OUT/icon" 2>/dev/null; then
    cp "$OUT/icon/app_icon.png" "$OUT/res/drawable/icon.png"
    cp "$OUT/icon/app_icon.png" "$OUT/res/drawable-xxhdpi/icon.png"
fi
cp build/shadowgun "$OUT/assets/shadowgun"
mkdir -p "$OUT/lib/arm64-v8a"
python3 apk/patch_syscalls.py "$QEMU" "$OUT/lib/arm64-v8a/libqemu.so"
rm -f apk/build/sysroot.zip
python3 apk/ziptool.py dir apk/build/sysroot apk/build/sysroot.zip
cp apk/build/sysroot.zip "$OUT/assets/sysroot.zip"

"$BT/aapt2" compile --dir "$OUT/res" -o "$OUT/resources.zip"
"$BT/aapt2" link \
    --manifest "$OUT/AndroidManifest.xml" \
    -I "$PLATFORM" \
    --java "$OUT/gen" \
    --min-sdk-version 26 \
    --target-sdk-version 34 \
    -o "$OUT/base.ap_" "$OUT/resources.zip"

find "$OUT/gen" apk/src -name '*.java' -print0 | xargs -0 javac \
    -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
    -classpath "$PLATFORM" -d "$OUT/classes"
"$BT/d8" --lib "$PLATFORM" --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

python3 apk/ziptool.py append "$OUT/base.ap_" "$OUT/unsigned.apk" \
    "S:lib/arm64-v8a/libqemu.so=$OUT/lib/arm64-v8a/libqemu.so" \
    "S:assets/shadowgun=$OUT/assets/shadowgun" \
    "S:assets/sysroot.zip=$OUT/assets/sysroot.zip" \
    "D:classes.dex=$OUT/dex/classes.dex"

# Kept outside $OUT so rebuilt APKs keep the same signature and update in place.
# RELEASE=1 signs with the release key (apk/build/release.keystore, created on first use; keep a copy of it:
# a different key means the app has to be uninstalled before an update) and names the file Shadowgun-Bridge-<version>.apk.
if [ "${RELEASE:-0}" = 1 ]; then
    KEY="$ROOT/apk/build/release.keystore"
    PASSFILE="$ROOT/apk/build/release.pass"
    if [ ! -f "$KEY" ]; then
        head -c 18 /dev/urandom | base64 | tr -d '/+=' > "$PASSFILE"
        keytool -genkeypair -keystore "$KEY" -storepass "$(cat "$PASSFILE")" -keypass "$(cat "$PASSFILE")"             -alias sgbridge -keyalg RSA -keysize 4096 -validity 36500             -dname "CN=ME Infiltrator Bridge,O=JamesCrumble,C=RU" >/dev/null 2>&1
    fi
    VER=$(grep -o 'versionName="[^"]*"' "$ROOT/apk/AndroidManifest.xml" | head -1 | cut -d'"' -f2)
    APK_OUT="$ROOT/Shadowgun-Bridge-$VER.apk"
    PASS=$(cat "$PASSFILE")
    "$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
    "$BT/apksigner" sign --ks "$KEY" --ks-pass "pass:$PASS" --key-pass "pass:$PASS" --ks-key-alias sgbridge         --out "$APK_OUT" "$OUT/aligned.apk"
else
    KEY="$ROOT/apk/build/debug.keystore"
    if [ ! -f "$KEY" ]; then
        keytool -genkeypair -keystore "$KEY" -storepass android -keypass android             -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000             -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
    fi
    APK_OUT="$ROOT/Shadowgun-Bridge-debug.apk"
    "$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
    "$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android --out "$APK_OUT" "$OUT/aligned.apk"
fi
"$BT/apksigner" verify --verbose "$APK_OUT"
echo "APK: $APK_OUT"
