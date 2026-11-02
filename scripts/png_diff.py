#!/usr/bin/env python3
"""png_diff — mean abs difference, differing-pixel share and bounding box of two PNGs.

No PIL/numpy on the Mac's system Python, so this decodes 8-bit non-interlaced
PNGs itself (what he_shot / sips write). Usage:
    scripts/png_diff.py A.png B.png [x0 y0 x1 y1]     # optional crop box (pixels)
"""
import struct, sys, zlib


def load(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and inter == 0 and ctype in (2, 6), f"unsupported PNG ({depth},{ctype},{inter})"
            bpp = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def main():
    wa, ha, ba, ra = load(sys.argv[1])
    wb, hb, bb, rb = load(sys.argv[2])
    assert (wa, ha) == (wb, hb), "size differs"
    x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 0, wa, ha)
    tot = cnt = n = 0
    bx0, by0, bx1, by1 = wa, ha, -1, -1
    for y in range(y0, y1):
        la, lb = ra[y], rb[y]
        for x in range(x0, x1):
            d = max(abs(la[x * ba + c] - lb[x * bb + c]) for c in range(3))
            n += 1
            tot += d
            if d:
                cnt += 1
                bx0, by0, bx1, by1 = min(bx0, x), min(by0, y), max(bx1, x), max(by1, y)
    print(f"mean abs diff {tot / max(n, 1):.4f}/255, differing pixels {100.0 * cnt / max(n, 1):.3f} %, "
          f"bbox {(bx0, by0, bx1, by1) if cnt else None}")


if __name__ == "__main__":
    main()
