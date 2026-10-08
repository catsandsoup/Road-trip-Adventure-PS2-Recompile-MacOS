#!/usr/bin/env python3
"""Summarise a PS2X_GS_CENSUS file (Metal plan M0). Counts only.

Usage: gs_census_report.py <census.txt> [--from F] [--to F] [--top N] [--group PREFIX ...]
  Per-frame counters: total, frames with a non-zero value, mean and max per frame (over frames
  that had at least one primitive, and over all presents).
  Keyed state counts: the last complete BEGIN/END block (cumulative), grouped by the first word.
"""
import sys


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    lo, hi, top, groups = 0, 1 << 62, 25, []
    i = 1
    while i < len(args):
        if args[i] == "--from":
            lo = int(args[i + 1]); i += 2
        elif args[i] == "--to":
            hi = int(args[i + 1]); i += 2
        elif args[i] == "--top":
            top = int(args[i + 1]); i += 2
        elif args[i] == "--group":
            groups.append(args[i + 1]); i += 2
        else:
            i += 1
    names = []
    frames = []
    block, cur = None, None
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("C "):
                names = line.split()[1:]
            elif line.startswith("F "):
                p = line.split()
                idx = int(p[1])
                if lo <= idx <= hi:
                    frames.append([int(v) for v in p[2:]])
            elif line.startswith("BEGIN "):
                cur = {"tag": line.split()[1], "frame": int(line.split()[2]), "keys": []}
            elif line.startswith("K ") and cur is not None:
                p = line.rstrip("\n").split(" ", 3)
                cur["keys"].append((int(p[1]), int(p[2]), p[3]))
            elif line.startswith("END ") and cur is not None:
                block, cur = cur, None
    drawing = [fr for fr in frames if fr and fr[0] > 0]
    print(f"presents={len(frames)} drawing_frames={len(drawing)}")
    print(f"{'counter':26s} {'total':>12s} {'frames>0':>9s} {'mean/drawfr':>12s} {'max':>10s}")
    for ci, n in enumerate(names):
        vals = [fr[ci] for fr in frames if ci < len(fr)]
        dv = [fr[ci] for fr in drawing if ci < len(fr)]
        tot = sum(vals)
        nz = sum(1 for v in vals if v)
        mean = (sum(dv) / len(dv)) if dv else 0.0
        mx = max(vals) if vals else 0
        print(f"{n:26s} {tot:12d} {nz:9d} {mean:12.1f} {mx:10d}")
    if block:
        print(f"\nkeyed counts (cumulative, block {block['tag']} at present {block['frame']}): total frames key")
        by = {}
        for tot, nfr, key in block["keys"]:
            by.setdefault(key.split(" ", 1)[0], []).append((tot, nfr, key))
        for g in sorted(by):
            if groups and g not in groups:
                continue
            rows = sorted(by[g], reverse=True)
            print(f"-- {g}: {len(rows)} distinct, {sum(r[0] for r in rows)} total")
            for tot, nfr, key in rows[:top]:
                print(f"   {tot:10d} {nfr:6d}  {key}")
            if len(rows) > top:
                print(f"   ... {len(rows) - top} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
