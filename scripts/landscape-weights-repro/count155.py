"""Thema 155: count red / green / blue dominated pixels in HE_DUMP BMPs.

The HE_DUMP_LANDSCAPELAYERS witness renders pastel at midday (tonemapped
0.9/0.1/0.1 etc.), so absolute thresholds count nothing — classify by how far
the dominant channel stands above the other two.

Reference (RTX 4070, 1280x720, cap155.ps1 defaults, 2026-10-06):
  D3D12 before the fix  red=459634 green=0     blue=0      (layer 0 only)
  D3D12 after           red=317451 green=74142 blue=23445
  D3D11                 red=317451 green=74142 blue=23445
  OpenGL                red=322179 green=78473 blue=23696

--base BASE.bmp (repaint witness, HE_DUMP_LAYERREPAINT=mid): also count the
pixels that are red in BASE and green in each capture — the new disc only.
Reference (same machine, -Extra HE_DUMP_AA=0, HE_DUMP_GI=0|1, red->green):
  GI off  D3D12 29002  D3D11 29002  OpenGL 32947  Vulkan 29042
  GI on   D3D12 31955  D3D11 31955  OpenGL 33061  Vulkan 32031
  D3D12 with InvalidateTexture a no-op (negative control): 0
GI on draws dark probe-tile stripes across the terrain on every backend —
known (docs/gi-ddgi-material-path-analysis-2026-10-02.md §7.2), not the blend.
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

def classify(R, G, B):
    if R - max(G, B) > 40: return 'r'
    if G - max(R, B) > 40: return 'g'
    if B - max(R, G) > 25: return 'b'
    return ''

args = sys.argv[1:]
base = None
if args[:1] == ['--base']:
    base = [classify(*p) for p in load(args[1])[2]]
    args = args[2:]
for path in args:
    w, h, px = load(path)
    cls = [classify(*p) for p in px]
    line = f"{path}: {w}x{h} red={cls.count('r')} green={cls.count('g')} blue={cls.count('b')}"
    if base is not None:
        line += f" red->green={sum(1 for a, c in zip(base, cls) if a == 'r' and c == 'g')}"
    print(line)
