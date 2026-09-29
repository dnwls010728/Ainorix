#!/bin/sh
# Regenerates engine/render/shaders/Shaders.glsl.h from Shaders.glsl.
# Needs sokol-shdc (https://github.com/floooh/sokol-tools-bin, bin/<os>/sokol-shdc):
# put it on PATH or set SOKOL_SHDC=/path/to/sokol-shdc.
set -e
cd "$(dirname "$0")/../../engine/render/shaders"
SHDC="${SOKOL_SHDC:-sokol-shdc}"
"$SHDC" --input Shaders.glsl --output Shaders.glsl.h --slang hlsl5:glsl300es:glsl430 --no-log-cmdline
echo "wrote engine/render/shaders/Shaders.glsl.h"
