#!/usr/bin/env python3
"""Summarise a PS2X_GS_TEE_LOG (Metal plan M1 tee backend). Counts only.

Usage: tee_report.py <tee.log> [--from F] [--to F]
Sums the per-frame "[gs-tee] frame=..." lines (the TOTAL lines are only written on a clean exit)
and prints the last cumulative fallback line.
"""
import re
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    lo, hi = 0, 1 << 62
    a = sys.argv[2:]
    if "--from" in a:
        lo = int(a[a.index("--from") + 1])
    if "--to" in a:
        hi = int(a[a.index("--to") + 1])
    keys = ["submits", "metal", "fallback", "runs", "run_px", "run_px_match", "run_frame_match", "run_z_match",
            "present_px", "present_match"]
    tot = {k: 0 for k in keys}
    maxd = {"run_max_diff": 0, "present_max_diff": 0}
    frames = compared = exact = metal_frames = metal_exact = 0
    worst = []
    cum = None
    pat = re.compile(r"(\w+)=([0-9.]+)")
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            if line.startswith("[gs-tee] CUM"):
                cum = line.strip()
                continue
            if not line.startswith("[gs-tee] frame="):
                continue
            d = {k: v for k, v in pat.findall(line)}
            fr = int(d["frame"])
            if not (lo <= fr <= hi):
                continue
            frames += 1
            for k in keys:
                tot[k] += int(float(d.get(k, 0)))
            for k in maxd:
                maxd[k] = max(maxd[k], int(d.get(k, 0)))
            px, m = int(d["present_px"]), int(d["present_match"])
            if px:
                compared += 1
                exact += (px == m)
                if int(d["metal"]):
                    metal_frames += 1
                    metal_exact += (px == m)
                if px != m:
                    worst.append((m / px, fr))
    s = tot["submits"]
    print(f"frames={frames} compared={compared} exact={exact} frames_with_metal_draws={metal_frames} of_which_exact={metal_exact}")
    print(f"submits={s} metal={tot['metal']} fallback={tot['fallback']} metal_frac={tot['metal'] / s if s else 0:.4f}")
    rp = tot["run_px"]
    print(f"runs={tot['runs']} run_px={rp} run_px_match={tot['run_px_match']} ({100 * tot['run_px_match'] / rp if rp else 100:.4f}%) "
          f"frame={tot['run_frame_match']} z={tot['run_z_match']} run_max_diff={maxd['run_max_diff']}")
    pp = tot["present_px"]
    print(f"present_px={pp} present_match={tot['present_match']} ({100 * tot['present_match'] / pp if pp else 100:.4f}%) "
          f"present_max_diff={maxd['present_max_diff']}")
    if worst:
        worst.sort()
        print("worst frames (match fraction, frame):", ", ".join(f"{w:.4f}@{fr}" for w, fr in worst[:10]))
    if cum:
        print(cum)
    return 0


if __name__ == "__main__":
    sys.exit(main())
