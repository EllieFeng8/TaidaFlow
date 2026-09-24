#!/usr/bin/env python3
"""Pixel-compare two screenshots (desktop "no regression" check for the font change).

  python scripts/image_diff.py BEFORE.png AFTER.png [--mask x0,y0,x1,y1 ...]
                               [--tolerance N] [--out diff.png]

Pixels inside --mask rectangles (e.g. the live clock, the OS title bar whose Mica
backdrop follows the wallpaper) are ignored. A pixel counts as changed when any colour
channel differs by more than --tolerance (default 0). Prints counts and the bounding box;
exit code 0 only when no pixel is changed. Requires Pillow.
"""
from __future__ import annotations

import argparse
import sys

from PIL import Image, ImageChops, ImageDraw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--mask", action="append", default=[], help="x0,y0,x1,y1 region to ignore")
    ap.add_argument("--tolerance", type=int, default=0, help="max per-channel difference ignored")
    ap.add_argument("--out", help="write a diff visualisation PNG")
    args = ap.parse_args()

    a = Image.open(args.before).convert("RGB")
    b = Image.open(args.after).convert("RGB")
    if a.size != b.size:
        print(f"size mismatch {a.size} vs {b.size}")
        return 2
    for m in args.mask:
        box = tuple(int(v) for v in m.split(","))
        for img in (a, b):
            ImageDraw.Draw(img).rectangle(box, fill=(0, 0, 0))
    r, g, bl = ImageChops.difference(a, b).split()
    peak = ImageChops.lighter(ImageChops.lighter(r, g), bl)          # per-pixel max channel diff
    hist = peak.histogram()
    any_diff = sum(hist[1:])
    changed = sum(hist[args.tolerance + 1:])
    max_diff = max((v for v in range(256) if hist[v]), default=0)
    mask_img = peak.point(lambda v: 255 if v > args.tolerance else 0)
    total = a.size[0] * a.size[1]
    print(f"size={a.size[0]}x{a.size[1]} masks={len(args.mask)} tolerance={args.tolerance} "
          f"pixels_any_diff={any_diff} max_channel_diff={max_diff} "
          f"changed_pixels={changed} ({changed / total:.6%}) bbox={mask_img.getbbox()}")
    if args.out:
        mask_img.save(args.out)
    return 0 if changed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
