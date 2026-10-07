"""Thema 158, Schritt 2: compare landscape-layer witness captures (cap158.ps1).

    python ana158.py A.bmp B.bmp [C.bmp ...]

For every capture: how many pixels show each layer colour of the
HE_DUMP_LANDSCAPELAYERS material (classified by the nearest of the eight
reference hues after normalising brightness away, so tonemapping/sRGB
differences between backends do not move a pixel between classes). For every
pair: mean |a-b| over RGB and the share of pixels differing by more than 8.
No third-party modules — BMP read with struct.
"""
import struct
import sys

LAYERS = {  # the witness material's base colours (EditorApplication.cpp)
    "red":     (0.90, 0.10, 0.10), "green":  (0.10, 0.85, 0.15),
    "blue":    (0.15, 0.25, 0.95), "yellow": (0.95, 0.90, 0.10),
    "magenta": (0.90, 0.10, 0.90), "cyan":   (0.10, 0.90, 0.90),
    "white":   (0.95, 0.95, 0.95), "orange": (0.95, 0.50, 0.05),
}


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
    px = []
    for y in range(h):
        row = (h - 1 - y) if flip else y
        base = off + row * stride
        for x in range(w):
            b, g, r = d[base + x * step], d[base + x * step + 1], d[base + x * step + 2]
            px.append((r, g, b))
    return w, h, px


def classify(p):
    m = max(p)
    if m < 40:
        return "dark"
    n = tuple(c / m for c in p)
    best, bd = None, 1e9
    for name, ref in LAYERS.items():
        rm = max(ref)
        r = tuple(c / rm for c in ref)
        dd = sum((a - b) ** 2 for a, b in zip(n, r))
        if dd < bd:
            best, bd = name, dd
    return best if bd < 0.12 else "other"


def main(paths):
    imgs = {p: read_bmp(p) for p in paths}
    for p, (w, h, px) in imgs.items():
        counts = {}
        for q in px:
            k = classify(q)
            counts[k] = counts.get(k, 0) + 1
        total = w * h
        line = ", ".join(f"{k}={100.0 * v / total:.2f}%" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))
        print(f"{p}: {w}x{h}  {line}")
    keys = list(imgs)
    for i in range(len(keys)):
        for j in range(i + 1, len(keys)):
            a, b = imgs[keys[i]], imgs[keys[j]]
            if a[:2] != b[:2]:
                print(f"{keys[i]} vs {keys[j]}: size differs")
                continue
            diff = 0
            big = 0
            for pa, pb in zip(a[2], b[2]):
                dd = sum(abs(x - y) for x, y in zip(pa, pb)) / 3.0
                diff += dd
                big += dd > 8
            n = len(a[2])
            print(f"{keys[i]} vs {keys[j]}: mean|d|={diff / n:.3f}  >8: {100.0 * big / n:.3f}%")


if __name__ == "__main__":
    main(sys.argv[1:])
