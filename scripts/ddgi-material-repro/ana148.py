"""Thema 148: DDGI on graph materials, hardware acceptance -- analysis of run148.ps1 captures.

Masks (1280x720, camera of cap148.ps1 'bleed'): the graph sphere is centred at (640,360),
r = 205 px; the built-in control sphere (GIBLEED 1/2) sits 6 m to the right, centre ~(1107,360),
cut by the frame edge. 'lower' = inside 0.93 r and below centre + 0.35 r: the part that faces
the floor slab and receives its bounce. Terrain: the witness square, x 300..980, y 40..700.

Metrics (8-bit sRGB values of the dumped BMP):
  bleed  = mean over the mask of (R-G)[red floor] - (R-G)[grey floor]  -> red tint from the probes
  |d|    = mean abs difference over the mask, all channels
Usage: python ana148.py [capdir] [--overlay]
"""
import sys, os
import numpy as np
from PIL import Image

CAP = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else r'C:\hw148\cap'
RHIS = ['opengl', 'd3d11', 'd3d12', 'vulkan']
H, W = 720, 1280
yy, xx = np.mgrid[0:H, 0:W]


def disk_lower(cx, cy, r):
    return (((xx - cx) ** 2 + (yy - cy) ** 2) < (0.93 * r) ** 2) & (yy > cy + 0.35 * r)


M_GRAPH = disk_lower(640, 360, 205)
M_BUILTIN = disk_lower(1107, 360, 205) & (xx < 1270)
M_TERRAIN = (xx >= 300) & (xx < 980) & (yy >= 40) & (yy < 700)


def load(name):
    p = os.path.join(CAP, name + '.bmp')
    if not os.path.exists(p):
        return None
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float64)


def rg(img, m):
    return float((img[..., 0] - img[..., 1])[m].mean())


def mad(a, b, m):
    return float(np.abs(a - b)[m].mean())


def bleed(build, rhi, gi, red, grey, m, tag=''):
    a, b = load(f'{build}_{rhi}_b{red}_gi{gi}{tag}'), load(f'{build}_{rhi}_b{grey}_gi{gi}{tag}')
    if a is None or b is None:
        return None
    return rg(a, m) - rg(b, m)


def fmt(v, nd=2):
    return '   -  ' if v is None else f'{v:6.{nd}f}'


def main():
    if '--overlay' in sys.argv:
        img = load('post_opengl_b1_gi1').copy()
        for m, c in ((M_GRAPH, (0, 255, 0)), (M_BUILTIN, (0, 0, 255))):
            img[m] = img[m] * 0.5 + np.array(c) * 0.5
        Image.fromarray(img.astype(np.uint8)).save(os.path.join(CAP, 'overlay_masks.png'))

    print('## A. Red-floor bleed on the lower hemisphere: (R-G)[red] - (R-G)[grey]')
    print('| backend | graph GI on post | graph GI on ctl | graph GI off post | built-in GI on post | built-in GI on ctl | built-in GI off post | graph/built-in (post) |')
    print('|---|---|---|---|---|---|---|---|')
    for r in RHIS:
        gp = bleed('post', r, 1, 3, 4, M_GRAPH)
        gc = bleed('ctl', r, 1, 3, 4, M_GRAPH)
        g0 = bleed('post', r, 0, 3, 4, M_GRAPH)
        bp = bleed('post', r, 1, 1, 2, M_BUILTIN)
        bc = bleed('ctl', r, 1, 1, 2, M_BUILTIN)
        b0 = bleed('post', r, 0, 1, 2, M_BUILTIN)
        ratio = None if gp is None or not bp else gp / bp
        print(f'| {r} | {fmt(gp)} | {fmt(gc)} | {fmt(g0)} | {fmt(bp)} | {fmt(bc)} | {fmt(b0)} | {fmt(ratio)} |')

    print('\n## B. Gate witness: post vs ctl, graph sphere lower hemisphere, mean |d|')
    print('| backend | b3 GI on | b4 GI on | b3 GI off | b3 GI on, post vs post_r2 (noise) |')
    print('|---|---|---|---|---|')
    for r in RHIS:
        row = []
        for (b1, n1, b2, n2) in (('post', 'b3_gi1', 'ctl', 'b3_gi1'), ('post', 'b4_gi1', 'ctl', 'b4_gi1'),
                                 ('post', 'b3_gi0', 'ctl', 'b3_gi0'), ('post', 'b3_gi1', 'post', 'b3_gi1_r2')):
            a, b = load(f'{b1}_{r}_{n1}'), load(f'{b2}_{r}_{n2}')
            row.append(None if a is None or b is None else mad(a, b, M_GRAPH))
        print(f'| {r} | ' + ' | '.join(fmt(v) for v in row) + ' |')

    print('\n## C. Cross-backend vs OpenGL, mean |d| over the mask (post build)')
    print('| backend | graph b3 GI on | graph b3 GI off | graph b4 GI on | built-in b1 GI on | built-in b1 GI off |')
    print('|---|---|---|---|---|---|')
    for r in RHIS[1:]:
        row = []
        for n, m in (('b3_gi1', M_GRAPH), ('b3_gi0', M_GRAPH), ('b4_gi1', M_GRAPH),
                     ('b1_gi1', M_BUILTIN), ('b1_gi0', M_BUILTIN)):
            a, b = load(f'post_{r}_{n}'), load(f'post_opengl_{n}')
            row.append(None if a is None or b is None else mad(a, b, m))
        print(f'| {r} | ' + ' | '.join(fmt(v) for v in row) + ' |')

    print('\n## D. Painted terrain (graph material), top-down, witness square')
    print('| backend | mean GI off | mean GI on | GI on: post vs ctl |d| | GI on: px > 2 | GI off: post vs ctl |d| | GI on post vs r2 |d| |')
    print('|---|---|---|---|---|---|---|')
    for r in RHIS:
        p0, p1 = load(f'post_{r}_terrain_gi0'), load(f'post_{r}_terrain_gi1')
        c0, c1 = load(f'ctl_{r}_terrain_gi0'), load(f'ctl_{r}_terrain_gi1')
        p1b = load(f'post_{r}_terrain_gi1_r2')
        if p0 is None or p1 is None or c0 is None or c1 is None:
            continue
        d = np.abs(p1 - c1).max(axis=2)[M_TERRAIN]
        print(f'| {r} | {p0[M_TERRAIN].mean():6.2f} | {p1[M_TERRAIN].mean():6.2f} | {mad(p1, c1, M_TERRAIN):6.2f} | '
              f'{100.0 * (d > 2).mean():5.1f} % | {mad(p0, c0, M_TERRAIN):6.2f} | '
              f'{fmt(None if p1b is None else mad(p1, p1b, M_TERRAIN))} |')


if __name__ == '__main__':
    main()
