#!/bin/bash
# Builds the offline SNDMOD harness from the PS2Recomp sources.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${PS2X_AUDIO_TREE:-$HERE/../../../PS2Recomp}/ps2xIOP/src"
OUT="${1:-$HERE/../../../work/audio/sndmod_replay}"
c++ -std=c++20 -O2 -Wall -I"$SRC" "$HERE/sndmod_replay.cpp" "$SRC/modules/sndmod/sndmod_driver.cpp" "$SRC/modules/sndmod/spu2.cpp" -o "$OUT"
echo "built $OUT"
