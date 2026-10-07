"""Thema 158, Schritt 3: compare texture-array witness captures (cap158arr.ps1).

    python ana158arr.py REF.bmp B.bmp [C.bmp ...]

The HE_DUMP_TEXARRAY terrain fills a box in the middle of the frame: five
stripes across (slices 0..4 = Grass, Dirt, Rock, Snow, WetGround) and four
bands down (Albedo array, Normal array, Mask array, the plain 2D Rock albedo).
The box is found in the FIRST image (everything that differs from the sky
colour at the frame's left edge), then cut into 5 x 4 cells; each cell is
averaged over its inner part (a margin keeps the seams out). Printed:

  - the mean RGB of every cell, per image;
  - per image against the first: the largest per-channel cell difference and
    mean|d| / share of pixels with |d| > 8 over the whole terrain box.

No third-party modules -- BMP read with struct.
"""
import struct
import sys

SLICES = ["Grass", "Dirt", "Rock", "Snow", "WetGnd"]
BANDS = ["AlbedoArr", "NormalArr", "MaskArr", "2D Rock"]


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


def cell_means(w, px, box, margin=0.18):
    x0, y0, x1, y1 = box
    cw, ch = (x1 - x0) / 5.0, (y1 - y0) / 4.0
    out = {}
    for b in range(4):
        for s in range(5):
            ax, bx = int(x0 + cw * (s + margin)), int(x0 + cw * (s + 1 - margin))
            ay, by = int(y0 + ch * (b + margin)), int(y0 + ch * (b + 1 - margin))
            acc = [0, 0, 0]
            n = 0
            for y in range(ay, by):
                for x in range(ax, bx):
                    p = px[y * w + x]
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]
                    n += 1
            out[(b, s)] = tuple(a / n for a in acc)
    return out


def main(paths):
    imgs = [(p, read_bmp(p)) for p in paths]
    w, h, px = imgs[0][1]
    box = terrain_box(w, h, px)
    print(f"terrain box (from {paths[0]}): x {box[0]}..{box[2]}, y {box[1]}..{box[3]}")
    means = {p: cell_means(img[0], img[2], box) for p, img in imgs}
    for p, _ in imgs:
        print(f"\n{p}")
        print("             " + "".join(f"{s:>16}" for s in SLICES))
        for b in range(4):
            row = "".join("  ({:3.0f},{:3.0f},{:3.0f})".format(*means[p][(b, s)]) for s in range(5))
            print(f"{BANDS[b]:<12}{row}")
    ref = paths[0]
    x0, y0, x1, y1 = box
    for p, (wb, hb, pb) in imgs[1:]:
        worst = max(max(abs(a - b) for a, b in zip(means[ref][k], means[p][k])) for k in means[ref])
        diff = 0.0
        big = 0
        n = 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                pa, pq = px[y * w + x], pb[y * wb + x]
                dd = sum(abs(a - b) for a, b in zip(pa, pq)) / 3.0
                diff += dd
                big += dd > 8
                n += 1
        print(f"\n{p} vs {ref}: worst cell-mean channel diff {worst:.2f}, "
              f"terrain mean|d| {diff / n:.3f}, >8: {100.0 * big / n:.3f}%")


if __name__ == "__main__":
    main(sys.argv[1:])
