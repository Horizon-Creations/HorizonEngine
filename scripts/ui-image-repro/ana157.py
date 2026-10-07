"""Thema 157: does the Image tile of HE_DUMP_UITEST=image show its picture?

Tile 12 of the witness (EditorApplication.cpp) is an Image element at x 60..300,
y 590..700 (canvas = pixels, ConstantPixel), white tint, showing a generated
picture: red top-left, green top-right, blue bottom-left, yellow bottom-right.
GL and Metal draw that picture; a backend whose UI pass ignores textureAssetId
draws the tint instead, i.e. a white quad.

Per backend this prints the colour at the centre of each quadrant (a 9x9 mean),
the plain tile 0 as a control that the widget is drawn at all, and a verdict:
  picture  - all four quadrants within 12 of the expected colour
  white    - all four quadrants within 12 of (255,255,255): the tint, no texture
  missing  - tile 12 is the background (tile not drawn at all)
  other    - anything else (look at the image)
Also counts '[ERROR]' and 'Validation' lines in <tag>_<backend>.log.

usage: python ana157.py <dir> <tag> [backends...]
Exit code = number of backends whose verdict is not 'picture'.
"""
import os, struct, sys

EXPECT = {'TL': (230, 30, 30), 'TR': (30, 200, 60), 'BL': (30, 80, 230), 'BR': (240, 220, 40)}
CENTRES = {'TL': (120, 617), 'TR': (240, 617), 'BL': (120, 672), 'BR': (240, 672)}
TILE0 = (180, 135)     # centre of tile 0 (plain panel, (66,76,97) on every backend)

def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bypp = struct.unpack_from('<H', d, 28)[0] // 8
    stride = (w * bypp + 3) & ~3
    flip = h > 0
    h = abs(h)
    def px(x, y):
        o = off + ((h - 1 - y) if flip else y) * stride + x * bypp
        return d[o + 2], d[o + 1], d[o]
    return w, h, px

def mean(px, cx, cy, r=4):
    acc = [0, 0, 0]; n = 0
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            p = px(x, y)
            for i in range(3): acc[i] += p[i]
            n += 1
    return tuple(round(a / n) for a in acc)

def close(a, b, tol=12):
    return max(abs(a[i] - b[i]) for i in range(3)) <= tol

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    d, tag = argv[1], argv[2]
    rhis = argv[3:] or ['OpenGL', 'Vulkan', 'D3D11', 'D3D12']
    bad = 0
    for rhi in rhis:
        bmp = os.path.join(d, f'{tag}_{rhi}.bmp')
        if not os.path.exists(bmp):
            print(f'{rhi:7s}  no capture'); bad += 1; continue
        w, h, px = read_bmp(bmp)
        q = {k: mean(px, *c) for k, c in CENTRES.items()}
        t0 = mean(px, *TILE0)
        if all(close(q[k], EXPECT[k]) for k in q):            verdict = 'picture'
        elif all(close(q[k], (255, 255, 255)) for k in q):    verdict = 'white'
        elif all(max(q[k]) <= 12 for k in q):                 verdict = 'missing'
        else:                                                 verdict = 'other'
        errs = val = 0
        lp = os.path.join(d, f'{tag}_{rhi}.log')
        if os.path.exists(lp):
            for line in open(lp, encoding='utf-8', errors='replace'):
                if '[ERROR]' in line: errs += 1
                if 'Validation' in line or 'VUID' in line: val += 1
        print(f'{rhi:7s} {w}x{h}  tile0={t0}  TL={q["TL"]} TR={q["TR"]} BL={q["BL"]} BR={q["BR"]}'
              f'  -> {verdict}   log: {errs} [ERROR], {val} validation')
        if verdict != 'picture': bad += 1
    return bad

if __name__ == '__main__':
    sys.exit(main(sys.argv))
