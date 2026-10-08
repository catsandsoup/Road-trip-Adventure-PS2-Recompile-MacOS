#!/bin/bash
# Mirror the project-root HANDOFF/CHANGELOG/GOALS into game/docs for publishing, replacing the local
# absolute project path and home directory with placeholders. The root copies stay the masters.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
for f in HANDOFF.md CHANGELOG.md GOALS.md; do
  sed -e "s|$ROOT|<project root>|g" -e "s|$HOME|~|g" "$ROOT/$f" > "$HERE/docs/$f"
done
grep -l "$HOME" "$HERE"/docs/*.md && { echo "personal path still present" >&2; exit 1; } || echo "docs synced"
