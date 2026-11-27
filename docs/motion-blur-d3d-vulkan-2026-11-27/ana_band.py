"""ana.py dir prefix: motion-blur witness numbers below the sky band (rows from 35 % down).
same-pose diffs (still MB vs base, base vs base2) and the gradient ratios MB / no-MB at the stepped pose:
a yaw step must lower the HORIZONTAL gradient much more than the vertical one, a pitch step the other way round."""
import sys
from PIL import Image, ImageChops, ImageStat

d, pre = sys.argv[1], sys.argv[2]
def img(n):
    im = Image.open(f"{d}/{pre}-{n}.bmp").convert("L")
    w, h = im.size
    return im.crop((int(w * 0.22), int(h * 0.22), int(w * 0.78), int(h * 0.40)))   # the cubes and their shadows
def diff(a, b):
    x = ImageChops.difference(img(a), img(b))
    return sum(ImageStat.Stat(x).mean), sum(1 for p in x.get_flattened_data() if p > 0)
def grads(n):
    im = img(n); w, h = im.size; px = im.load()
    gh = sum(abs(px[x + 1, y] - px[x, y]) for y in range(0, h, 2) for x in range(0, w - 1, 2))
    gv = sum(abs(px[x, y + 1] - px[x, y]) for y in range(0, h - 1, 2) for x in range(0, w, 2))
    return gh, gv
for a, b in (("base", "base2"), ("base", "mb-still")):
    m, n = diff(a, b)
    print(f"{a} vs {b}: mean {m:.3f}, {n} px differ")
for mb, ref in (("mb-yaw", "nomb-yaw"), ("mb-pitch", "nomb-pitch")):
    (h1, v1), (h0, v0) = grads(mb), grads(ref)
    print(f"{mb} / {ref}: horizontal gradient x{h1 / h0:.2f}, vertical x{v1 / v0:.2f}")

