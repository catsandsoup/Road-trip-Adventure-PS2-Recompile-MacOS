#!/bin/bash
# Deterministic capture with the exact golden recipe (HANDOFF §3.2), then the golden comparison and a
# throughput report. Used as the gate for every change that claims to be output-neutral.
#
# Usage: game/tests/det_capture.sh <outdir> <boot|race|newgame> [runner] [seconds]
#   runner   defaults to $RTA_RUNNER or the main tree's ps2EntryRunner
#   seconds  wall-clock run time (default 130; the goldens need ~100 s at 18 ticks/s)
# Env: GOLDEN_DIR overrides the golden root (default <port>/work/golden_det); extra PS2X_* variables
#      (e.g. PS2X_GS_THREAD) pass through to the runner.
# Output: <outdir>/frames (PNG every 50 ticks), <outdir>/log.txt, a compare line and ticks/s per section.
# Throughput is ticks per wall second between dumped frames (deterministic mode is unpaced, so this is
# the compute cost of the route). Only meaningful on an otherwise idle machine.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="$(cd "$HERE/../.." && pwd)"
OUT="$1"; ROUTE="$2"
RUNNER="${3:-${RTA_RUNNER:-$PORT/PS2Recomp/out/rta/ps2xRuntime/ps2EntryRunner}}"
SECS="${4:-130}"
GOLDEN="${GOLDEN_DIR:-$PORT/work/golden_det}"
case "$ROUTE" in
  boot) IN=boot_to_title.txt ;;
  race) IN=quick_race.txt ;;
  newgame) IN=new_game_intro.txt ;;
  *) echo "unknown route $ROUTE" >&2; exit 2 ;;
esac
OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
rm -rf "$OUT/mc" "$OUT/frames" "$OUT/log.txt"
mkdir -p "$OUT/mc" "$OUT/frames"
cd "$PORT/work"
START=$(python3 -c 'import time; print(time.time())')
PS2X_HEADLESS=1 PS2X_DETERMINISTIC=1 PS2X_DUMP_EVERY=50 RTA_SAVE_DIR="$OUT/mc" RTA_RUNNER="$RUNNER" \
  PS2X_INPUT_SCRIPT="$HERE/input/$IN" PS2X_DUMP_FRAMES="$OUT/frames" \
  perl -e "alarm $SECS; exec @ARGV" "$PORT/game/tools/run.sh" > "$OUT/log.txt" 2>&1
END=$(python3 -c 'import time; print(time.time())')
python3 - "$OUT/frames" "$START" "$END" <<'EOF'
import glob, os, re, sys
d, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rows = []
for f in glob.glob(os.path.join(d, "*.png")):
    m = re.match(r"frame_(\d+).*_r(\d+)\.png", os.path.basename(f))
    if m:
        rows.append((int(m.group(1)), int(m.group(2)), os.path.getmtime(f)))
rows.sort()
last = rows[-1] if rows else (0, 0, 0)
print(f"wall={end - start:.1f}s frames={len(rows)} last_tick={last[0]} last_read={last[1]}")
for lo, hi, name in [(0, 700, "boot/menus"), (1400, 3000, "race 3D (race route)"), (3000, 5000, "office 3D (boot route)")]:
    w = [r for r in rows if lo <= r[1] <= hi]
    if len(w) >= 2 and w[-1][2] > w[0][2]:
        print(f"  {name:24s} r{lo}-{hi}: {(w[-1][0] - w[0][0]) / (w[-1][2] - w[0][2]):6.1f} ticks/s")
EOF
python3 "$HERE/compare_frames.py" "$GOLDEN/$ROUTE" "$OUT/frames" | tail -4
