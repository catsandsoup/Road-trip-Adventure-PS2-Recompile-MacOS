#!/usr/bin/env python3
"""SSIM between frame pairs (pure Python + PIL, no numpy). Metal plan C8 gate tool.

Usage:
  ssim.py A.png B.png                     one pair
  ssim.py --tee DIR                       pairs fNNNNNN_cpu.ppm / fNNNNNN_mtl.ppm written by PS2X_GS_TEE_DIR
  ssim.py DIR_A DIR_B                     frames matched by name (like compare_frames.py: tick prefix frame_NNNNNN)
Options: --min X (exit 1 if any SSIM < X; default 0.98), --gaussian (11x11 sigma 1.5 window, slow;
default is the 8x8 uniform window of Wang et al. 2004 via integral images), --quiet.

SSIM is computed on luma (PIL "L", ITU-R 601) with C1=(0.01*255)^2, C2=(0.03*255)^2, every window
position where the window fits, averaged. Identical images short-circuit to 1.0.
Also reports the exact-pixel fraction on RGB.
"""
import glob
import math
import os
import re
import sys

from PIL import Image

C1 = (0.01 * 255) ** 2
C2 = (0.03 * 255) ** 2


def integral(vals, w, h):
    """Summed-area table with a zero row/column: size (w+1)*(h+1)."""
    W = w + 1
    s = [0.0] * (W * (h + 1))
    for y in range(h):
        row = 0.0
        base = y * w
        o = (y + 1) * W
        p = y * W
        for x in range(w):
            row += vals[base + x]
            s[o + x + 1] = s[p + x + 1] + row
    return s


def ssim_uniform(a, b, w, h, win=8):
    n = win * win
    sa = integral(a, w, h)
    sb = integral(b, w, h)
    saa = integral([v * v for v in a], w, h)
    sbb = integral([v * v for v in b], w, h)
    sab = integral([a[i] * b[i] for i in range(len(a))], w, h)
    W = w + 1
    total = 0.0
    count = 0
    for y in range(h - win + 1):
        r0 = y * W
        r1 = (y + win) * W
        for x in range(w - win + 1):
            i00, i01, i10, i11 = r0 + x, r0 + x + win, r1 + x, r1 + x + win
            ma = (sa[i11] - sa[i01] - sa[i10] + sa[i00]) / n
            mb = (sb[i11] - sb[i01] - sb[i10] + sb[i00]) / n
            va = (saa[i11] - saa[i01] - saa[i10] + saa[i00]) / n - ma * ma
            vb = (sbb[i11] - sbb[i01] - sbb[i10] + sbb[i00]) / n - mb * mb
            cov = (sab[i11] - sab[i01] - sab[i10] + sab[i00]) / n - ma * mb
            # unbiased covariance as in Wang et al. (n/(n-1)) for the uniform window
            k = n / (n - 1.0)
            va *= k
            vb *= k
            cov *= k
            total += ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2))
            count += 1
    return total / count if count else 1.0


def blur_sep(vals, w, h, ker):
    r = len(ker) // 2
    tmp = [0.0] * (w * h)
    for y in range(h):
        base = y * w
        for x in range(r, w - r):
            acc = 0.0
            for k, kv in enumerate(ker):
                acc += kv * vals[base + x + k - r]
            tmp[base + x] = acc
    out = [0.0] * (w * h)
    for y in range(r, h - r):
        for x in range(r, w - r):
            acc = 0.0
            for k, kv in enumerate(ker):
                acc += kv * tmp[(y + k - r) * w + x]
            out[y * w + x] = acc
    return out


def ssim_gaussian(a, b, w, h):
    ker = [math.exp(-((i - 5) ** 2) / (2 * 1.5 * 1.5)) for i in range(11)]
    s = sum(ker)
    ker = [k / s for k in ker]
    ma = blur_sep(a, w, h, ker)
    mb = blur_sep(b, w, h, ker)
    saa = blur_sep([v * v for v in a], w, h, ker)
    sbb = blur_sep([v * v for v in b], w, h, ker)
    sab = blur_sep([a[i] * b[i] for i in range(len(a))], w, h, ker)
    total = 0.0
    count = 0
    for y in range(5, h - 5):
        for x in range(5, w - 5):
            i = y * w + x
            m1, m2 = ma[i], mb[i]
            v1, v2, c = saa[i] - m1 * m1, sbb[i] - m2 * m2, sab[i] - m1 * m2
            total += ((2 * m1 * m2 + C1) * (2 * c + C2)) / ((m1 * m1 + m2 * m2 + C1) * (v1 + v2 + C2))
            count += 1
    return total / count if count else 1.0


def compare(pa, pb, gaussian=False):
    ia = Image.open(pa).convert("RGB")
    ib = Image.open(pb).convert("RGB")
    if ia.size != ib.size:
        return None, 0.0
    w, h = ia.size
    ra, rb = ia.tobytes(), ib.tobytes()
    if ra == rb:
        return 1.0, 1.0
    same = sum(1 for i in range(0, len(ra), 3) if ra[i:i + 3] == rb[i:i + 3])
    exact = same / (w * h)
    la = [float(v) for v in ia.convert("L").tobytes()]
    lb = [float(v) for v in ib.convert("L").tobytes()]
    s = ssim_gaussian(la, lb, w, h) if gaussian else ssim_uniform(la, lb, w, h)
    return s, exact


def pairs_from_args(args):
    if args[0] == "--tee":
        d = args[1]
        out = []
        for c in sorted(glob.glob(os.path.join(d, "f*_cpu.ppm"))):
            m = c[: -len("_cpu.ppm")] + "_mtl.ppm"
            if os.path.exists(m):
                out.append((os.path.basename(c)[:-8], c, m))
        return out
    a, b = args[0], args[1]
    if os.path.isfile(a):
        return [(os.path.basename(a), a, b)]
    key = re.compile(r"frame_(\d+)")
    ma = {key.match(os.path.basename(f)).group(1): f for f in glob.glob(os.path.join(a, "frame_*.png")) if key.match(os.path.basename(f))}
    mb = {key.match(os.path.basename(f)).group(1): f for f in glob.glob(os.path.join(b, "frame_*.png")) if key.match(os.path.basename(f))}
    return [(k, ma[k], mb[k]) for k in sorted(set(ma) & set(mb))]


def main():
    argv = sys.argv[1:]
    thr = 0.98
    gaussian = "--gaussian" in argv
    quiet = "--quiet" in argv
    argv = [x for x in argv if x not in ("--gaussian", "--quiet")]
    if "--min" in argv:
        i = argv.index("--min")
        thr = float(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) < 2:
        print(__doc__)
        return 2
    pairs = pairs_from_args(argv)
    if not pairs:
        print("no pairs")
        return 2
    vals = []
    for name, pa, pb in pairs:
        try:
            s, ex = compare(pa, pb, gaussian)
        except OSError as e:  # e.g. an empty (0x0) frame
            print(f"{name}: skipped ({e.__class__.__name__})")
            continue
        if s is None:
            print(f"{name}: size mismatch")
            vals.append(0.0)
            continue
        vals.append(s)
        if not quiet or s < 1.0:
            print(f"{name}: ssim={s:.6f} exact={ex * 100:.3f}%")
    if not vals:
        print("no comparable pairs")
        return 2
    mn = min(vals)
    print(f"pairs={len(vals)} ssim_min={mn:.6f} ssim_mean={sum(vals) / len(vals):.6f} below_{thr}={sum(1 for v in vals if v < thr)}")
    return 0 if mn >= thr else 1


if __name__ == "__main__":
    sys.exit(main())
