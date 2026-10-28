#!/usr/bin/env python3
"""shore_edge_sharpness — how wide is the water's edge along a pixel row? (Thema 206 Schritt 2)

A hard shoreline is a step of the brightness from the dark water to the sand within a pixel
or two; a soft one is a ramp tens of pixels wide. Per row this smooths the luma over 5 pixels
and reports how many pixels lie between the 20 % and the 80 % crossing of the way from the
water's level (the mean of the first 20 pixels of the window) to the bank's (the last 20).
Rows that cross a sun glint or a wave crest read wider than the edge: pick rows of calm water
(beach views: 300 and 400), and compare the same rows of the before and after shot.

Usage: scripts/shore_edge_sharpness.py A.png [B.png ...] [--rows 300,400] [--window 150,1000]
No PIL/numpy: png_diff.load decodes the PNG.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import load


def edge_width(path, row, x0, x1):
    w, h, bpp, rows = load(path)
    r = rows[row]
    y = [0.299 * r[x * bpp] + 0.587 * r[x * bpp + 1] + 0.114 * r[x * bpp + 2] for x in range(w)]
    s = [sum(y[max(0, i - 2):i + 3]) / len(y[max(0, i - 2):i + 3]) for i in range(w)]
    lo = sum(s[x0:x0 + 20]) / 20
    hi = sum(s[x1 - 20:x1]) / 20
    t20, t80 = lo + 0.2 * (hi - lo), lo + 0.8 * (hi - lo)
    a = next((i for i in range(x0, x1) if s[i] >= t20), None)
    b = next((i for i in range(x1 - 1, x0, -1) if s[i] <= t80), None)
    return (b - a) if a is not None and b is not None else None, round(lo), round(hi)


def main():
    args = sys.argv[1:]
    rows, window = [300, 400], (150, 1000)
    if "--rows" in args:
        k = args.index("--rows")
        rows = [int(v) for v in args[k + 1].split(",")]
        del args[k:k + 2]
    if "--window" in args:
        k = args.index("--window")
        window = tuple(int(v) for v in args[k + 1].split(","))
        del args[k:k + 2]
    for p in args:
        out = [f"row {r}: {edge_width(p, r, *window)[0]} px" for r in rows]
        print(f"{os.path.basename(p)}: " + ", ".join(out))


if __name__ == "__main__":
    main()
