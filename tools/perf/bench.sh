#!/usr/bin/env bash
# Frame time of a project in the GPU editor, played headless (see docs/RENDER_PERF.md).
# Renders the Game view while playing, reads every frame back (so the GPU work is
# included) and prints the mean wall time per frame of `frames` frames, without
# the editor start-up: the difference between a short and a long run.
# Usage: tools/perf/bench.sh <project> [frames, default 360]
set -euo pipefail
project="${1:?usage: tools/perf/bench.sh <project> [frames]}"
frames="${2:-360}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
oe="$root/build/bin/oe"
[ -x "$oe" ] || oe="$root/build/bin/oe.exe"
shot="$(mktemp -d)/bench.png"
run() {
    local start end
    start=$(date +%s%N)
    "$oe" editor "$project" --screenshot "$shot" --play --frames "$1" >/dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000 ))
}
short=$(run 60)
long=$(run $((60 + frames)))
us=$(( (long - short) / frames ))
echo "$project: $((us / 1000)).$(printf '%03d' $((us % 1000))) ms/frame over $frames frames"
