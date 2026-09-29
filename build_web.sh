#!/bin/sh
# Builds the web runtime (the player as WebAssembly + WebGL2) that
# `oe package --web` ships: build/bin/web/oe_player.js + oe_player.wasm.
# Needs the Emscripten SDK (https://emscripten.org/docs/getting_started/downloads.html):
# run `source <emsdk>/emsdk_env.sh` first, or set EMSDK=<emsdk folder>.
# Usage: ./build_web.sh [Release|Debug] [bin folder of the native build, default build/bin]
set -e
cd "$(dirname "$0")"
CONFIG=${1:-Release}
BIN=${2:-build/bin}
if ! command -v emcmake >/dev/null 2>&1 && [ -n "$EMSDK" ]; then . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1; fi
if ! command -v emcmake >/dev/null 2>&1; then
  echo "Emscripten not found: install emsdk, then run 'source <emsdk>/emsdk_env.sh' (or set EMSDK)." >&2
  exit 1
fi
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" -DOE_BUILD_TESTS=OFF
cmake --build build-web --target oe_player
mkdir -p "$BIN/web"
cp build-web/bin/oe_player.js build-web/bin/oe_player.wasm tools/player/web/index.html "$BIN/web/"
echo "Built: $BIN/web/oe_player.js + oe_player.wasm (used by oe package --web)"
