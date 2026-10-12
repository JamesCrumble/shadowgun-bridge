#!/bin/bash
# Runs the port on the PC under the host's qemu-arm with Mesa's software GLES (slow; for bring-up only).
# Usage: tools/run_local.sh [seconds]   Log: logs/local.log
P=$(cd "$(dirname "$0")/.." && pwd)
export SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless
export SHADOWGUN_CONTROL_DIR=${SHADOWGUN_CONTROL_DIR:-ctl} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
export LOADER_TRACE=1 SHADOWGUN_SHOT_EVERY=${SHADOWGUN_SHOT_EVERY:-100} SHADOWGUN_WIDTH=${SHADOWGUN_WIDTH:-854} SHADOWGUN_HEIGHT=${SHADOWGUN_HEIGHT:-480} SHADOWGUN_ALLOW_UNRESOLVED=${SHADOWGUN_ALLOW_UNRESOLVED:-1}
export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf
export LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
cd "$P"
timeout "${1:-60}" qemu-arm build/shadowgun sgdata > logs/local.log 2>&1
echo "exit=$?"
