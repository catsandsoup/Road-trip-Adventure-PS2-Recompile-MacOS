#!/usr/bin/env python3
"""Check that every configured jump-table target is dispatchable in the generated code.

Usage: game/tests/check_jump_tables.py [rta.toml] [generated_dir]   (defaults: work/rta.toml, work/generated)

Every target listed under [[jump_tables.table]] must appear in register_functions.cpp, either as a
function start or as a resume entry of its owner function. Otherwise a JR inside a mid-function entry
slice whose case lies outside the slice falls through to dispatchGuestBranch and the game stops with
"guest-branch:missing-target" (Laguna Raceway crash, CHANGELOG 2026-10-07). Exit 1 on any miss.
Run after every regeneration of work/generated."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(os.path.dirname(HERE)), "work")
toml_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORK, "rta.toml")
gen_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(WORK, "generated")

targets = {int(t, 16) for t in re.findall(r'target = "0x([0-9a-fA-F]+)"', open(toml_path).read())}
registry = open(os.path.join(gen_dir, "register_functions.cpp")).read().lower()
registered = {int(a, 16) for a in re.findall(r"0x([0-9a-f]{5,8})u?", registry)}
missing = sorted(t for t in targets if t not in registered)
print(f"jump-table targets: {len(targets)}, unregistered: {len(missing)}")
for t in missing[:50]:
    print(f"  missing 0x{t:x}")
sys.exit(1 if missing else 0)
