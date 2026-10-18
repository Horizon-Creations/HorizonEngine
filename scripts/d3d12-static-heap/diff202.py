#!/usr/bin/env python3
"""Pixel diff of two HE_DUMP BMP captures (REN-202 A/B), no third-party deps.

    python diff202.py a.bmp b.bmp

Prints mean |a-b| over RGB, the number of pixels differing by more than 8/255 in
any channel, and the same split into four horizontal bands (the animated sky puts
its noise in the top band only). Run it on two captures of the SAME side first:
that is the noise floor the A/B has to be read against.
"""
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp not in (24, 32):
        raise SystemExit(f"{path}: {bpp} bpp not supported")
    step = bpp // 8
    row = (w * step + 3) & ~3
    flip = h > 0
    h = abs(h)
    px = []
    for y in range(h):
        sy = h - 1 - y if flip else y
        base = off + sy * row
        px.append([d[base + x * step: base + x * step + 3] for x in range(w)])
    return w, h, px


def main():
    wa, ha, a = load(sys.argv[1])
    wb, hb, b = load(sys.argv[2])
    if (wa, ha) != (wb, hb):
        raise SystemExit(f"size differs: {wa}x{ha} vs {wb}x{hb}")
    bands = 4
    tot = [0] * bands
    big = [0] * bands
    n = [0] * bands
    for y in range(ha):
        k = min(bands - 1, y * bands // ha)
        ra, rb = a[y], b[y]
        for x in range(wa):
            pa, pb = ra[x], rb[x]
            d0, d1, d2 = abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])
            tot[k] += d0 + d1 + d2
            n[k] += 1
            if max(d0, d1, d2) > 8:
                big[k] += 1
    allN = sum(n)
    print(f"mean|d|={sum(tot) / (3 * allN):.4f} px>8={sum(big)} of {allN}")
    for k in range(bands):
        print(f"  band{k}: mean|d|={tot[k] / (3 * n[k]):.4f} px>8={big[k]}")


if __name__ == "__main__":
    main()
