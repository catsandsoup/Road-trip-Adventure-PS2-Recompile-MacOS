#!/usr/bin/env python3
"""Apply game/config/stub_policy.toml to a generated PS2Recomp config (work/rta.toml).

  apply_stub_policy.py <rta.toml> [--functions <rta_functions.csv>] [--check]

* run_as_game_code: removed from `stubs`; added to `entry_points` with the same address, so the
  recompiler translates the game's own code (the call sites stay exact).
* force_stub: kept in / added to `stubs` (address taken from the existing stubs, entry_points or the
  functions CSV "name,address,..." if given).
--check only reports violations (exit 1 if any). Pure text edit; keeps every other line unchanged.
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
POLICY = os.path.join(os.path.dirname(HERE), "config", "stub_policy.toml")


def read_list(text, key):
    m = re.search(key + r"\s*=\s*\[(.*?)\]", text, re.S)
    return re.findall(r'"([^"]+)"', m.group(1)) if m else []


def section(text, key):
    m = re.search(r"(?m)^" + key + r"\s*=\s*\[\n(.*?)^\]", text, re.S)
    if not m:
        raise SystemExit(f"{key} list not found")
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("--functions")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    policy = open(POLICY).read()
    game, force = read_list(policy, "run_as_game_code"), read_list(policy, "force_stub")
    text = open(a.config).read()
    stubs = dict(re.findall(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"', section(text, "stubs").group(1)))
    entries = dict(re.findall(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"', section(text, "entry_points").group(1)))
    known = dict(stubs); known.update(entries)
    if a.functions and os.path.exists(a.functions):
        for line in open(a.functions):
            parts = line.strip().split(",")
            if len(parts) >= 2 and re.match(r"(0x)?[0-9A-Fa-f]+$", parts[1]):
                known.setdefault(parts[0], parts[1] if parts[1].startswith("0x") else "0x" + parts[1])
    problems = [f"{n} is stubbed but must run as game code" for n in game if n in stubs]
    problems += [f"{n} must be a stub but is not" for n in force if n not in stubs]
    if a.check:
        print("\n".join(problems) or "policy OK")
        return 1 if problems else 0
    for n in game:
        if n in stubs:
            addr = stubs.pop(n)
            text = re.sub(r'\n\s*"' + re.escape(n) + "@" + addr + r'",?', "", text, count=1)
            if n not in entries:
                m = section(text, "entry_points")
                text = text[:m.start(1)] + f'  "{n}@{addr}",\n' + text[m.start(1):]
                entries[n] = addr
    for n in force:
        if n not in stubs:
            if n not in known:
                print(f"warning: no address known for force_stub {n}", file=sys.stderr)
                continue
            m = section(text, "stubs")
            text = text[:m.start(1)] + f'  "{n}@{known[n]}",\n' + text[m.start(1):]
    open(a.config, "w").write(text)
    print(f"applied: {len(game)} run-as-game-code, {len(force)} force-stub")
    return 0


if __name__ == "__main__":
    sys.exit(main())
