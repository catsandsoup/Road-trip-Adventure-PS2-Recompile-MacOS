#!/usr/bin/env python3
"""Compare two deterministic frame-dump directories frame by frame (matched on the tick index).

Usage: compare_frames.py <golden_dir> <candidate_dir> [--max-diff-pct P]
Deterministic runs (PS2X_DETERMINISTIC=1) must produce byte-identical PNGs at the same tick; any
renderer change that claims to be output-neutral is checked with this. Exit 1 if any common frame
differs by more than P percent of pixels (default 0: exact)."""
import argparse, glob, hashlib, os, re, sys

def frames(d):
    out = {}
    for f in glob.glob(os.path.join(d, "*.png")):
        m = re.match(r"frame_(\d+)", os.path.basename(f))
        if m:
            out[int(m.group(1))] = f
    return out

def pixels(path):
    from PIL import Image
    return Image.open(path).convert("RGB")

ap = argparse.ArgumentParser()
ap.add_argument("golden"); ap.add_argument("candidate")
ap.add_argument("--max-diff-pct", type=float, default=0.0)
a = ap.parse_args()
g, c = frames(a.golden), frames(a.candidate)
common = sorted(set(g) & set(c))
same = 0; worst = []
for t in common:
    if hashlib.sha1(open(g[t], "rb").read()).digest() == hashlib.sha1(open(c[t], "rb").read()).digest():
        same += 1
        continue
    A, B = pixels(g[t]), pixels(c[t])
    if A.size != B.size:
        worst.append((100.0, t)); continue
    pa, pb = A.tobytes(), B.tobytes()
    diff = sum(1 for i in range(0, len(pa), 3) if pa[i:i + 3] != pb[i:i + 3])
    worst.append((100.0 * diff / (len(pa) // 3), t))
worst.sort(reverse=True)
print(f"common frames {len(common)}, byte-identical {same}, differing {len(worst)}")
for pct, t in worst[:10]:
    print(f"  tick {t}: {pct:.3f}% pixels differ  ({os.path.basename(g[t])})")
bad = [w for w in worst if w[0] > a.max_diff_pct]
sys.exit(1 if bad or not common else 0)
