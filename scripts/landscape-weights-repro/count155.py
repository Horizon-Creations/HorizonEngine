"""Thema 155: count red / green / blue dominated pixels in HE_DUMP BMPs.

The HE_DUMP_LANDSCAPELAYERS witness renders pastel at midday (tonemapped
0.9/0.1/0.1 etc.), so absolute thresholds count nothing — classify by how far
the dominant channel stands above the other two.

Reference (RTX 4070, 1280x720, cap155.ps1 defaults, 2026-10-06):
  D3D12 before the fix  red=459634 green=0     blue=0      (layer 0 only)
  D3D12 after           red=317451 green=74142 blue=23445
  D3D11                 red=317451 green=74142 blue=23445
  OpenGL                red=322179 green=78473 blue=23696
"""
import struct, sys

def load(path):
    b = open(path, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bpp = struct.unpack_from('<H', b, 28)[0]
    step = bpp // 8
    row = (w * step + 3) & ~3
    px = []
    for y in range(abs(h)):
        base = off + y * row
        for x in range(w):
            B, G, R = b[base + x * step: base + x * step + 3]
            px.append((R, G, B))
    return w, abs(h), px

for path in sys.argv[1:]:
    w, h, px = load(path)
    r = g = bl = 0
    for R, G, B in px:
        if R - max(G, B) > 40: r += 1
        elif G - max(R, B) > 40: g += 1
        elif B - max(R, G) > 25: bl += 1
    print(f"{path}: {w}x{h} red={r} green={g} blue={bl}")
