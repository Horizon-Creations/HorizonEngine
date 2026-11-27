#!/usr/bin/env python3
"""cloud_track.py <frames dir> [first] [count]: per frame, the centroid of the bright (cloud) pixels in the upper sky band,
and its step from the frame before. A smooth flight gives small, even steps; a jitter shows as steps that flip sign."""
import glob
import sys

d = sys.argv[1]
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
fs = sorted(glob.glob(d + "/frame_*.ppm"))[first:first + count]
prev = None
steps = []
for f in fs:
    b = open(f, "rb").read()
    q = b.split(maxsplit=4)
    w, h, px = int(q[1]), int(q[2]), q[4]
    sx = sy = n = 0
    for y in range(20, 300, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            i = row + x * 3
            r, g, bl = px[i], px[i + 1], px[i + 2]
            if r > 200 and g > 200 and bl > 200 and bl - r < 25:   # white-ish, not the blue sky
                sx += x; sy += y; n += 1
    c = (sx / n, sy / n) if n else (0.0, 0.0)
    if prev:
        steps.append(c[0] - prev[0])
        print(f"{f[-9:-4]}  n={n:6d}  cx={c[0]:7.1f} cy={c[1]:6.1f}  dx={c[0] - prev[0]:+6.1f}")
    prev = c
if steps:
    flips = sum(1 for a, b in zip(steps, steps[1:]) if a * b < 0)
    print(f"mean |dx| {sum(abs(s) for s in steps) / len(steps):.1f} px, sign flips {flips} of {len(steps) - 1}")
