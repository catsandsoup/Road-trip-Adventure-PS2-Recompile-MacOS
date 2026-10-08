#!/bin/bash
# Builds the offline VU1 equivalence test (vu1_replay) from the PS2Recomp
# interpreter/recompiler runtime sources and the locally generated microprogram
# C++ (work/vu1gen). Output goes to work/vu1test (gitignored scratch).
# Env: PS2X_VU1_RUNTIME=<PS2Recomp tree> (default PS2Recomp), PS2X_VU1GEN_DIR=<generated dir>
#      (default work/vu1gen), PS2X_VU1TEST_OUT=<output dir> (default work/vu1test).
# Usage: game/tests/vu1/build.sh && work/vu1test/vu1_replay work/vu1cat work/vu1snap,work/explore/cap [--fuzz 20] [--poison 2]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
RT="${PS2X_VU1_RUNTIME:-$ROOT/PS2Recomp}/ps2xRuntime"
GEN="${PS2X_VU1GEN_DIR:-$ROOT/work/vu1gen}"
OUT="${PS2X_VU1TEST_OUT:-$ROOT/work/vu1test}"
mkdir -p "$OUT"
SSE2NEON="$(ls -d "$RT"/../out/rta/_deps/sse2neon-src "$ROOT"/PS2Recomp/out/rta/_deps/sse2neon-src 2>/dev/null | head -1 || true)"
INC=(-I"$RT/include" -I"$RT/src/lib/Kernel")
[ -n "$SSE2NEON" ] && INC+=(-I"$SSE2NEON")
DEFS=(-DUSE_SSE2NEON -DNDEBUG)
CXX="${CXX:-clang++}"
objs=()
for src in "$RT/src/lib/vu/ps2_vu1_core.cpp" "$RT/src/lib/vu/ps2_vu1_upper.cpp" "$RT/src/lib/vu/ps2_vu1_lower.cpp" \
           "$RT/src/lib/vu/ps2_vu1_recomp.cpp" "$HERE/vu1_replay.cpp"; do
  o="$OUT/$(basename "$src" .cpp).o"
  # same flags as the runtime library build (oracle must be compiled identically)
  "$CXX" -std=gnu++20 -O3 "${DEFS[@]}" "${INC[@]}" -c "$src" -o "$o"
  objs+=("$o")
done
for src in "$GEN"/vu1rec_*.cpp; do
  o="$OUT/$(basename "$src" .cpp).o"
  "$CXX" -std=gnu++20 -O2 -ffp-contract=off -frounding-math "${DEFS[@]}" "${INC[@]}" -c "$src" -o "$o"
  objs+=("$o")
done
"$CXX" -std=gnu++20 -O2 "${DEFS[@]}" "${INC[@]}" -c "$HERE/vu1_stubs.cpp" -o "$OUT/stubs.o"
"$CXX" "${objs[@]}" "$OUT/stubs.o" -o "$OUT/vu1_replay"
echo "built $OUT/vu1_replay"
