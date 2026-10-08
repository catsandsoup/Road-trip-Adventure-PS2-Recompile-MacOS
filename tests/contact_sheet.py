#!/usr/bin/env python3
"""Tile frame dumps into one labelled contact sheet (local review aid; output stays in work/).

Usage: contact_sheet.py <frames_dir> <out.png> [--step N] [--from R] [--to R] [--cols C] [--scale S]
Frames are ordered by name; --from/--to filter on the _r<padreads> tag when present."""
import argparse, glob, os, re
from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("dir"); ap.add_argument("out")
ap.add_argument("--step", type=int, default=1); ap.add_argument("--cols", type=int, default=6)
ap.add_argument("--scale", type=float, default=0.33)
ap.add_argument("--from", dest="lo", type=int, default=0); ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
a = ap.parse_args()
files = []
for f in sorted(glob.glob(os.path.join(a.dir, "*.png"))):
    m = re.search(r"_r(\d+)", f)
    r = int(m.group(1)) if m else 0
    if a.lo <= r <= a.hi:
        files.append(f)
files = files[::a.step][:60]
if not files:
    raise SystemExit("no frames")
ims = [Image.open(f).convert("RGB") for f in files]
w, h = int(ims[0].width * a.scale), int(ims[0].height * a.scale)
rows = (len(ims) + a.cols - 1) // a.cols
sheet = Image.new("RGB", (w * a.cols, (h + 14) * rows), "black")
d = ImageDraw.Draw(sheet)
for i, (f, im) in enumerate(zip(files, ims)):
    x, y = (i % a.cols) * w, (i // a.cols) * (h + 14)
    sheet.paste(im.resize((w, h)), (x, y + 14))
    d.text((x + 2, y + 1), os.path.basename(f).replace("frame_", "").replace(".png", ""), fill="yellow")
sheet.save(a.out)
print(a.out, len(files), "frames")
