#!/bin/bash
# Bring-up launcher: runs the recompiled game against the user's own disc.
# Usage: tools/run.sh [path/to/disc.iso] [extra args]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PORT="$(cd "$HERE/.." && pwd)"
DISC_DIR="${RTA_DISC_DIR:-$PORT/disc}"
ISO="${1:-$DISC_DIR/rta.iso}"
ELF="$DISC_DIR/extracted/SLES_513.56"
RUNNER="${RTA_RUNNER:-$PORT/PS2Recomp/out/rta/ps2xRuntime/ps2EntryRunner}"
SAVE="${RTA_SAVE_DIR:-$HOME/Library/Application Support/RoadTripAdventure/mc0}"
mkdir -p "$SAVE"
export PS2X_CD_IMAGE="$ISO"
export PS2X_MC_ROOT="$SAVE"
exec "$RUNNER" "$ELF" "${@:2}"
