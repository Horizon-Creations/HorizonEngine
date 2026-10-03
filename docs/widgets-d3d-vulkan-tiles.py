"""Per-tile comparison of HE_DUMP_UITEST captures against the GL reference (Thema 133, Schritt 3).

The witness (EditorApplication.cpp, HE_DUMP_UITEST) lays out 12 "Schicht 0" tiles on a
1280x720 ConstantPixel canvas: tile i sits at x = 60 + 300*(i%4), y = 80 + 170*(i//4),
240x110. Each tile is measured in its rect grown by 30 px (the drop-shadow halo; the
gaps between tiles are 60 px, so neighbours never overlap).

Per tile and backend:
  diff  = share of pixels whose max RGB channel differs from GL by more than 8
  edge  = the same, but only pixels whose GL value differs from a neighbour by > 8
          (the antialiased rim) -- what is left when the shapes match
  a<255 = pixels whose render-target ALPHA is below 255. The editor shows the
          viewport RT through ImGui::Image with alpha blending, so these pixels
          are see-through in the editor/PIE, though the RGB dump looks fine.

usage: python widgets-d3d-vulkan-tiles.py <dir> <tag> [backends...]
       reads <dir>/<tag>_<backend>.bmp, reference <dir>/<tag>_OpenGL.bmp
       (or --ref <path> to compare against another GL capture)

Exit code = number of (tile, backend) pairs that FAIL: diff above 0.5 % of the
tile, or any pixel with alpha < 255 (GL itself is not held to the alpha rule:
its glBlendFunc blends the alpha channel too, see the analysis doc).
Step 2 state (tag fix): 32 failures. Step 3 (tag sdf): 0.
"""
import struct, sys

NAMES = ['plain', 'round 24', 'tab', 'leaf', 'border', 'grad lin', 'grad rad', 'grad 90',
         'drop shadow', 'inner shadow', 'shadow+border', 'capsule rad+inner']

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
        return d[o + 2], d[o + 1], d[o], (d[o + 3] if bypp == 4 else 255)
    return w, h, px

def tile_rect(i, m=30):
    x, y = 60 + 300 * (i % 4), 80 + 170 * (i // 4)
    return x - m, y - m, x + 240 + m, y + 110 + m

def main(argv):
    ref = None
    if '--ref' in argv:
        k = argv.index('--ref'); ref = argv[k + 1]; del argv[k:k + 2]
    d, tag = argv[0], argv[1]
    backends = argv[2:] or ['D3D11', 'D3D12', 'Vulkan']
    _, _, g = read_bmp(ref or f'{d}/{tag}_OpenGL.bmp')
    imgs = {b: read_bmp(f'{d}/{tag}_{b}.bmp')[2] for b in backends}
    hdr = f'{"tile":<20}' + ''.join(f'{b + " diff/edge/a<255":>26}' for b in backends) + f'{"GL a<255":>10}'
    print(hdr)
    tot = {b: [0, 0] for b in backends}
    fails = 0
    for i, name in enumerate(NAMES):
        x0, y0, x1, y1 = tile_rect(i)
        n = 0; ga = 0
        row = {b: [0, 0, 0] for b in backends}
        for y in range(y0, y1):
            for x in range(x0, x1):
                r = g(x, y); n += 1
                if r[3] < 255: ga += 1
                rim = any(max(abs(p - q) for p, q in zip(r[:3], g(x + dx, y + dy)[:3])) > 8
                          for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                for b, p in imgs.items():
                    v = p(x, y)
                    bad = max(abs(a - c) for a, c in zip(r[:3], v[:3])) > 8
                    if bad:
                        row[b][0] += 1
                        if not rim: row[b][1] += 1
                    if v[3] < 255: row[b][2] += 1
        line = f'{i:>2} {name:<17}'
        for b in backends:
            bad, inner, al = row[b]
            tot[b][0] += bad; tot[b][1] += n
            if 100.0 * bad / n > 0.5 or al > 0: fails += 1
            line += f'{100.0 * bad / n:>11.2f}%{100.0 * (bad - inner) / n:>6.2f}%{al:>8}'
        print(line + f'{ga:>10}')
    print('all tiles' + ''.join(f'{100.0 * t[0] / t[1]:>26.2f}%' for b, t in tot.items()))
    print(f'failed (tile, backend) pairs: {fails}')
    return fails

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
