#!/usr/bin/env python3
"""Thema 210: pixel difference of two uncompressed BMP captures (24/32 bit).

    bmpdiff.py A.bmp B.bmp     -> "<differing px> / <total>  max channel delta <d>  mean |delta| <m>"

No PIL on the build Macs, so the BMP is read by hand (what writeBMP in the editor writes).
"""
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        b = f.read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp not in (24, 32):
        sys.exit(f"{path}: {bpp} bpp not supported")
    step = bpp // 8
    row = (w * step + 3) & ~3
    px = []
    for y in range(abs(h)):
        r = b[off + y * row: off + y * row + w * step]
        px.append(bytes(r[i] for i in range(len(r)) if i % step < 3))
    return w, abs(h), px


def main():
    wa, ha, a = read_bmp(sys.argv[1])
    wb, hb, b = read_bmp(sys.argv[2])
    if (wa, ha) != (wb, hb):
        sys.exit(f"size differs: {wa}x{ha} vs {wb}x{hb}")
    diff = 0
    mx = 0
    tot = 0
    for ra, rb in zip(a, b):
        for x in range(wa):
            d = max(abs(ra[3 * x + c] - rb[3 * x + c]) for c in range(3))
            if d:
                diff += 1
                tot += d
                mx = max(mx, d)
    n = wa * ha
    print(f"{diff} / {n} px differ  max channel delta {mx}  mean |delta| over differing {tot / max(diff, 1):.1f}")


if __name__ == "__main__":
    main()
