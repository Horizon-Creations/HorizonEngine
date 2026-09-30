#!/usr/bin/env python3
"""imgdiff.py A.png B.png [--band 90] [--diffout D.png] [--gain 8]
Pure-Python (no numpy/PIL): sips -> 24/32-bit BMP, then per-band stats.
Prints overall mean |d| (per channel, /255), max, % pixels != and per band.
Also mean luminance of each image per band (to see "darker ground")."""
import struct, subprocess, sys, os, tempfile


def load(png):
    tmp = tempfile.mktemp(suffix=".bmp")
    subprocess.run(["sips", "-s", "format", "bmp", png, "--out", tmp],
                   check=True, capture_output=True)
    b = open(tmp, "rb").read()
    os.remove(tmp)
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    bp = bpp // 8
    stride = (w * bp + 3) & ~3
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top_down else h - 1 - y
        r = b[off + sy * stride: off + sy * stride + w * bp]
        rows.append(bytes(r[i] for i in range(len(r)) if i % bp < 3))  # BGR
    return w, h, rows


def write_bmp(path, w, h, rows):
    stride = (w * 3 + 3) & ~3
    pad = b"\0" * (stride - w * 3)
    data = b"".join(rows[h - 1 - y] + pad for y in range(h))
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, "wb").write(hdr + dib + data)


def main():
    a, b = sys.argv[1], sys.argv[2]
    band = 90
    diffout = None
    gain = 8
    args = sys.argv[3:]
    for i, x in enumerate(args):
        if x == "--band": band = int(args[i + 1])
        if x == "--diffout": diffout = args[i + 1]
        if x == "--gain": gain = int(args[i + 1])
    w, h, A = load(a)
    w2, h2, B = load(b)
    assert (w, h) == (w2, h2), "size mismatch"
    tot = mx = ndiff = 0
    lumA = lumB = 0.0
    bands = []
    drows = []
    for y0 in range(0, h, band):
        bt = bm = bn = 0
        la = lb = 0.0
        for y in range(y0, min(h, y0 + band)):
            ra, rb = A[y], B[y]
            drow = bytearray(len(ra))
            for x in range(0, len(ra), 3):
                d0 = abs(ra[x] - rb[x]); d1 = abs(ra[x+1] - rb[x+1]); d2 = abs(ra[x+2] - rb[x+2])
                s = d0 + d1 + d2
                if s:
                    bn += 1
                    m = max(d0, d1, d2)
                    if m > bm: bm = m
                    bt += s
                    v = min(255, m * gain)
                    drow[x] = drow[x+1] = drow[x+2] = v
                la += 0.0722 * ra[x] + 0.7152 * ra[x+1] + 0.2126 * ra[x+2]
                lb += 0.0722 * rb[x] + 0.7152 * rb[x+1] + 0.2126 * rb[x+2]
            drows.append(bytes(drow))
        npx = w * (min(h, y0 + band) - y0)
        bands.append((y0, bt / (3 * npx), bm, 100.0 * bn / npx, la / npx, lb / npx))
        tot += bt; mx = max(mx, bm); ndiff += bn; lumA += la; lumB += lb
    n = w * h
    print(f"{os.path.basename(a)} vs {os.path.basename(b)}: mean {tot/(3*n):.3f}/255 max {mx} "
          f"px!= {100.0*ndiff/n:.2f}%  lum {lumA/n:.1f} -> {lumB/n:.1f}")
    for y0, m, bm, pct, la, lb in bands:
        print(f"  rows {y0:4d}-{min(h,y0+band)-1:4d}: mean {m:6.3f} max {bm:3d} px!= {pct:6.2f}%  lum {la:6.1f} -> {lb:6.1f}")
    if diffout:
        tmp = diffout + ".bmp"
        write_bmp(tmp, w, h, drows)
        subprocess.run(["sips", "-s", "format", "png", tmp, "--out", diffout], capture_output=True)
        os.remove(tmp)


if __name__ == "__main__":
    main()
