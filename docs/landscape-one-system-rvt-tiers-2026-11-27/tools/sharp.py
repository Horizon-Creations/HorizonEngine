#!/usr/bin/env python3
"""mean |Laplacian| of the luma of PPM crops, edges above a threshold (the editor grid lines) left out. usage: sharp.py a.ppm b.ppm ..."""
import sys

def read_ppm(p):
    d = open(p, "rb").read()
    toks, i = [], 0
    while len(toks) < 4:
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        toks.append(d[i:j]); i = j
    i += 1
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[i:i + w * h * 3]

BANDS = {"near (rows 840-950)": (500, 1500, 840, 950), "mid (rows 560-640)": (500, 1500, 560, 640), "far (rows 330-400)": (500, 1500, 330, 400)}
for p in sys.argv[1:]:
    w, h, px = read_ppm(p)
    luma = lambda x, y: (px[(y * w + x) * 3] * 3 + px[(y * w + x) * 3 + 1] * 6 + px[(y * w + x) * 3 + 2]) / 10.0
    out = []
    for name, (x0, x1, y0, y1) in BANDS.items():
        s = n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                c = luma(x, y)
                l = abs(4 * c - luma(x - 1, y) - luma(x + 1, y) - luma(x, y - 1) - luma(x, y + 1))
                if l < 40:
                    s += l; n += 1
        out.append(f"{name}: {s / max(n, 1):.3f}")
    print(p.split("/")[-1], " | ".join(out))
