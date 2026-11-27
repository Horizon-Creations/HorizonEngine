#!/usr/bin/env python3
"""Decodes a tile landscape's far image cache (.far, LandscapeFarMap.h) into one PNG of the whole world (rgb = albedo)."""
import struct, subprocess, sys, zlib

src, dst = sys.argv[1], sys.argv[2]
b = open(src, "rb").read()
magic, ver, tx, tz, rt, tpt = struct.unpack_from("<6I", b, 0)
assert magic == 0x4d464c48 and ver == 1
side = rt * tpt
rx, rz = (tx + rt - 1) // rt, (tz + rt - 1) // rt
W, H = tx * tpt, tz * tpt
img = bytearray(W * H * 3)
p = 24
baked = 0
for r in range(rx * rz):
    h, size = struct.unpack_from("<QI", b, p); p += 12
    blob = b[p:p + size]; p += size
    if not size:
        continue
    gm, gv, be, fl, kind, ch = struct.unpack_from("<IBBBBB", blob, 0)
    w, hh, raw, stored = struct.unpack_from("<4I", blob, 12)
    pay = blob[28:28 + stored]
    if be == 2:
        pay = subprocess.run(["zstd", "-dc"], input=pay, capture_output=True, check=True).stdout
    elif be != 0:
        sys.exit("lz4 not handled")
    assert len(pay) == raw and ch == 4 and w == side
    plane = w * hh
    px = bytearray(raw)
    for c in range(ch):
        for z in range(hh):
            acc = 0
            for x in range(w):
                i = z * w + x
                acc = (acc + pay[c * plane + i]) & 255 if fl & 1 else pay[i * ch + c]
                px[i * ch + c] = acc
    ox, oz = (r % rx) * side, (r // rx) * side
    for z in range(hh):
        if oz + z >= H: break
        for x in range(w):
            if ox + x >= W: break
            i = (z * w + x) * 4
            o = ((oz + z) * W + ox + x) * 3
            img[o:o + 3] = px[i:i + 3]
    baked += 1

rows = b"".join(b"\x00" + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
print(f"{tx}x{tz} tiles, {tpt} texels a tile, {baked}/{rx*rz} regions baked, image {W}x{H}")
