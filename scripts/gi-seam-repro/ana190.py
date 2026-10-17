"""Thema 190 step 6: count seam dashes in cap190.ps1 captures.

usage: python ana190.py SHOTDIR NAME X0 X1 Y0 Y1 [BACKENDS...] [--montage out.png]

For each backend it reads NAME-{lod0,nostitch,stitched}-<RHI>.bmp and counts, in
the window x0..x1, y0..y1, the "dash pixels": luma below 0.75 x the median of
its own row in the window AND at least 12 levels below it. The lod0 frame (every
chunk LOD0, no mixed seam) is the relief's own count; nostitch is the negative
control (it must be clearly above lod0, or the capture proves nothing); the fix
is good when stitched comes down to about lod0. --montage writes the three
windows side by side, per backend one row, enlarged 2x.
"""
import sys
import numpy as np
from PIL import Image

VARIANTS = ("lod0", "nostitch", "stitched")


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2], a


def dashes(l, x0, x1, y0, y1):
    w = l[y0:y1, x0:x1]
    med = np.median(w, axis=1, keepdims=True)
    return int(np.count_nonzero((w < 0.75 * med) & (w < med - 12.0)))


def main():
    args = sys.argv[1:]
    montage = None
    if "--montage" in args:
        i = args.index("--montage")
        montage = args[i + 1]
        del args[i:i + 2]
    shots, name = args[0], args[1]
    x0, x1, y0, y1 = (int(v) for v in args[2:6])
    backends = args[6:] or ["D3D11", "D3D12", "Vulkan"]
    rows = []
    print(f"window x {x0}..{x1} y {y0}..{y1}")
    print(f"{'backend':8} " + " ".join(f"{v:>9}" for v in VARIANTS) + "   mean-luma(stitched vs lod0)")
    for b in backends:
        counts, crops, means = [], [], []
        for v in VARIANTS:
            p = f"{shots}/{name}-{v}-{b}.bmp"
            try:
                l, rgb = luma(p)
            except FileNotFoundError:
                counts.append(None); crops.append(None); means.append(None)
                continue
            counts.append(dashes(l, x0, x1, y0, y1))
            means.append(float(l[y0:y1, x0:x1].mean()))
            crops.append(rgb[y0:y1, x0:x1].astype(np.uint8))
        cs = " ".join(f"{c:>9}" if c is not None else f"{'-':>9}" for c in counts)
        mm = f"{means[2]:.1f} vs {means[0]:.1f}" if means[0] is not None and means[2] is not None else ""
        print(f"{b:8} {cs}   {mm}")
        rows.append(crops)
    if montage:
        h, w = y1 - y0, x1 - x0
        gap = 6
        out = np.full(((h + gap) * len(rows), (w + gap) * 3, 3), 255, np.uint8)
        for r, crops in enumerate(rows):
            for c, crop in enumerate(crops):
                if crop is not None:
                    out[r * (h + gap):r * (h + gap) + h, c * (w + gap):c * (w + gap) + w] = crop
        im = Image.fromarray(out)
        im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
        im.save(montage)
        print("montage:", montage, "(columns lod0 | nostitch | stitched, rows", ", ".join(backends) + ")")


if __name__ == "__main__":
    main()
