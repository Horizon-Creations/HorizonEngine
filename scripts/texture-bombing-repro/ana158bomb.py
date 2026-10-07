"""Thema 158, Schritt 4: evaluate texture-bombing witness captures (cap158bomb.ps1).

    python ana158bomb.py REF.bmp [B.bmp ...]

The HE_DUMP_TEXBOMB terrain fills a box in the middle of the frame: five
stripes across (slices 0..4 = Grass, Dirt, Rock, Snow, WetGround), texture
tiling 10 over the box, and four bands down:

  0  the PLAIN albedo array read (the reference: it repeats every tile)
  1  the albedo array through Texture Array Bombing
  2  (N.x*0.5+0.5, height, N.z*0.5+0.5): bombed normal X/Z around the bombed
     mask's height
  3  the mask array through Texture Array Bombing

Printed per image:

  - REPETITION: per band, mean |I(p) - I(p + one tile)| (0..255, mean of RGB,
    horizontal and vertical shift). The tile period in pixels is FITTED on band
    0 of the first image (the shift that makes the plain read most self-similar),
    so the slight perspective of the -89 degree camera cannot fake a number.
    Plain sampling: ~0 (a texture repeats exactly). Bombing: large.
  - SLOPE: per stripe of band 2, the correlation of the height's image gradient
    (dG/dx, dG/dy) with the normal's packed X (R) and Z (B). The placeholder
    normal is the slope of the height, so with normal and height bombed on the
    SAME hexes and each hex's normal turned back correctly, the correlation is
    strong in every stripe and of one sign. A normal read on other hexes
    (=mismatch) or turned the wrong way loses it.
  - against the first image: largest cell-mean difference, mean|d| and the share
    of pixels with |d| > 8 over the whole box and per band.

No third-party modules -- BMP read with struct.
"""
import math
import struct
import sys

SLICES = ["Grass", "Dirt", "Rock", "Snow", "WetGnd"]
BANDS = ["plain albedo", "bombed albedo", "N.x|H|N.z", "bombed mask"]
TILES = 10


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    step = bpp // 8
    stride = (w * step + 3) & ~3
    flip = h > 0
    h = abs(h)
    px = [None] * (w * h)
    for y in range(h):
        row = (h - 1 - y) if flip else y
        base = off + row * stride
        for x in range(w):
            i = base + x * step
            px[y * w + x] = (d[i + 2], d[i + 1], d[i])
    return w, h, px


def terrain_box(w, h, px):
    sky = px[(h // 2) * w + 2]
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            p = px[y * w + x]
            if sum(abs(a - b) for a, b in zip(p, sky)) > 24:
                xs.append(x)
                ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)


def cell_rect(box, b, s, margin):
    x0, y0, x1, y1 = box
    cw, ch = (x1 - x0) / 5.0, (y1 - y0) / 4.0
    return (int(x0 + cw * (s + margin)), int(y0 + ch * (b + margin)),
            int(x0 + cw * (s + 1 - margin)), int(y0 + ch * (b + 1 - margin)))


def cell_means(w, px, box):
    out = {}
    for b in range(4):
        for s in range(5):
            ax, ay, bx, by = cell_rect(box, b, s, 0.18)
            acc = [0, 0, 0]
            n = 0
            for y in range(ay, by):
                for x in range(ax, bx):
                    p = px[y * w + x]
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]
                    n += 1
            out[(b, s)] = tuple(a / n for a in acc)
    return out


def self_diff(w, px, box, band, dx, dy, step=1):
    """Mean |I(p) - I(p + (dx, dy))| over the band's cells (shift stays inside a cell)."""
    acc = 0.0
    n = 0
    for s in range(5):
        ax, ay, bx, by = cell_rect(box, band, s, 0.04)
        for y in range(ay, by - dy, step):
            for x in range(ax, bx - dx, step):
                p = px[y * w + x]
                q = px[(y + dy) * w + x + dx]
                acc += (abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])) / 3.0
                n += 1
    return acc / n if n else float("nan")


def fit_period(w, px, box, horizontal):
    x0, y0, x1, y1 = box
    nominal = ((x1 - x0) if horizontal else (y1 - y0)) / TILES
    best = None
    for p in range(int(nominal * 0.85), int(nominal * 1.15) + 1):
        d = self_diff(w, px, box, 0, p if horizontal else 0, 0 if horizontal else p, step=2)
        if best is None or d < best[1]:
            best = (p, d)
    return best[0]


def corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    sab = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    saa = sum((x - ma) ** 2 for x in a)
    sbb = sum((y - mb) ** 2 for y in b)
    return sab / math.sqrt(saa * sbb) if saa > 0 and sbb > 0 else float("nan")


def slope_corr(w, px, box, s):
    ax, ay, bx, by = cell_rect(box, 2, s, 0.06)
    gx, nx, gy, nz = [], [], [], []
    for y in range(ay + 1, by - 1):
        for x in range(ax + 1, bx - 1):
            gx.append((px[y * w + x + 1][1] - px[y * w + x - 1][1]) / 2.0)
            nx.append(px[y * w + x][0])
            gy.append((px[(y + 1) * w + x][1] - px[(y - 1) * w + x][1]) / 2.0)
            nz.append(px[y * w + x][2])
    return corr(gx, nx), corr(gy, nz)


def main(paths):
    imgs = [(p, read_bmp(p)) for p in paths]
    w, h, px = imgs[0][1]
    box = terrain_box(w, h, px)
    px_x = fit_period(w, px, box, True)
    px_y = fit_period(w, px, box, False)
    print(f"terrain box (from {paths[0]}): x {box[0]}..{box[2]}, y {box[1]}..{box[3]}; "
          f"tile period fitted on band 0: {px_x} x {px_y} px")
    ref_cells = None
    for path, (iw, ih, ipx) in imgs:
        print(f"\n{path}")
        cells = cell_means(iw, ipx, box)
        for b in range(4):
            print(f"  {BANDS[b]:14s} " + "  ".join(
                "(%3.0f,%3.0f,%3.0f)" % cells[(b, s)] for s in range(5)))
        print("  REPETITION  mean|I(p) - I(p + 1 tile)|   (horizontal / vertical)")
        for b in range(4):
            rx = self_diff(iw, ipx, box, b, px_x, 0)
            ry = self_diff(iw, ipx, box, b, 0, px_y)
            print(f"    {BANDS[b]:14s} {rx:7.2f} / {ry:7.2f}")
        print("  SLOPE  corr(dH/dx, N.x)  corr(dH/dy, N.z)   per stripe")
        for s in range(5):
            cx, cz = slope_corr(iw, ipx, box, s)
            print(f"    {SLICES[s]:7s} {cx:+.3f}  {cz:+.3f}")
        if ref_cells is None:
            ref_cells = cells
            continue
        worst = max(abs(a - b) for k in cells for a, b in zip(cells[k], ref_cells[k]))
        rpx = imgs[0][1][2]
        x0, y0, x1, y1 = box
        tot = big = 0
        acc = 0.0
        band_acc = [[0.0, 0, 0] for _ in range(4)]
        ch = (y1 - y0) / 4.0
        for y in range(y0, y1 + 1):
            b = min(3, int((y - y0) / ch))
            for x in range(x0, x1 + 1):
                p, q = ipx[y * iw + x], rpx[y * iw + x]
                d = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
                m = (abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])) / 3.0
                acc += m
                tot += 1
                big += d > 8
                band_acc[b][0] += m
                band_acc[b][1] += 1
                band_acc[b][2] += d > 8
        print(f"  vs {paths[0]}: worst cell-mean diff {worst:.2f}, mean|d| {acc / tot:.3f}, "
              f"{100.0 * big / tot:.3f}% px > 8")
        for b in range(4):
            a, n, g = band_acc[b]
            print(f"    {BANDS[b]:14s} mean|d| {a / n:.3f}, {100.0 * g / n:.3f}% px > 8")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
