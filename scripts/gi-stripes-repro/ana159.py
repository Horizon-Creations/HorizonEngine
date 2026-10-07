"""Thema 159 S3: before/after numbers for the black GI stripes.

Reads the cap159.ps1 / run159.ps1 captures of three frozen deploys:
  pre   C:\\hw159\\pre2\\cap\\pre_*     (d1b8c36a, before the fix; *_s3 = re-taken in S3)
  fixA  C:\\hw159\\s3fixA\\cap\\fixA_*  (HEAD with Fix B patched out: fp32 position only)
  post  C:\\hw159\\s3post\\cap\\post_*  (HEAD 4e2e4d90; *_r2 = run-to-run repeat)

Stripe energy = mean |delta luminance| between vertically adjacent pixels, luminance =
mean of R, G, B (identical to stripe_energy in scripts/he_vk_imagetests.py, same boxes).
Image diff = mean / max |delta| over all channels (8-bit steps) and the share of pixels
with any channel changed. Usage: python ana159.py [--pre DIR] [--fixa DIR] [--post DIR]
"""
import argparse
import pathlib

import numpy as np
from PIL import Image

RHIS = ["opengl", "vulkan", "d3d11", "d3d12"]
BOXES = {"terrain": (0.08, 0.78, 0.92, 0.98), "sphere": (0.37, 0.17, 0.63, 0.47)}


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int32)


def box(img, b):
    h, w, _ = img.shape
    x0, y0, x1, y1 = b
    return img[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]


def stripe_energy(img, b):
    lum = box(img, b).sum(axis=2)
    return float(np.abs(np.diff(lum, axis=0)).mean() / 3.0)


def diff(a, b):
    d = np.abs(a - b)
    return float(d.mean()), int(d.max()), float((d.max(axis=2) > 0).mean() * 100.0)


def mean_lum(img, b):
    return float(box(img, b).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pre", default=r"C:\hw159\pre2\cap")
    ap.add_argument("--fixa", default=r"C:\hw159\s3fixA\cap")
    ap.add_argument("--post", default=r"C:\hw159\s3post\cap")
    a = ap.parse_args()
    dirs = {"pre": pathlib.Path(a.pre), "fixA": pathlib.Path(a.fixa), "post": pathlib.Path(a.post)}

    def img(tag, rhi, case, suffix=""):
        p = dirs[tag] / f"{tag}_{rhi}_{case}{suffix}.bmp"
        return load(p) if p.exists() else None

    print("## Stripe energy (terrain / sphere), GI on")
    print("| case | rhi | pre | fixA | post | post_r2 |")
    print("|---|---|---|---|---|---|")
    for case in ("y300_gi1", "layers_y300_gi1", "y0_gi1"):
        for rhi in RHIS:
            cells = []
            for tag, suf in (("pre", ""), ("fixA", ""), ("post", ""), ("post", "_r2")):
                im = img(tag, rhi, case, suf)
                cells.append("-" if im is None else
                             f"{stripe_energy(im, BOXES['terrain']):.3f} / {stripe_energy(im, BOXES['sphere']):.3f}")
            print(f"| {case} | {rhi} | " + " | ".join(cells) + " |")

    print("\n## Stripe energy, GI off (control: the boxes hold no stripes of their own)")
    print("| case | rhi | pre | post |")
    print("|---|---|---|---|")
    for case in ("y300_gi0", "layers_y300_gi0", "y0_gi0"):
        for rhi in RHIS:
            cells = []
            for tag in ("pre", "post"):
                im = img(tag, rhi, case) if not (tag == "pre" and case.startswith("layers")) \
                    else img(tag, rhi, case, "_s3")
                cells.append("-" if im is None else
                             f"{stripe_energy(im, BOXES['terrain']):.3f} / {stripe_energy(im, BOXES['sphere']):.3f}")
            print(f"| {case} | {rhi} | " + " | ".join(cells) + " |")

    pairs = [
        # (label, (tag, case, suffix), (tag, case, suffix))
        ("GI off y300: pre vs post", ("pre", "y300_gi0", ""), ("post", "y300_gi0", "")),
        ("GI off y0: pre vs post", ("pre", "y0_gi0", ""), ("post", "y0_gi0", "")),
        ("GI off layers y300: pre vs post", ("pre", "layers_y300_gi0", "_s3"), ("post", "layers_y300_gi0", "")),
        ("GI off y0: fixA vs post", ("fixA", "y0_gi0", ""), ("post", "y0_gi0", "")),
        ("noise GI on y0: post vs post_r2", ("post", "y0_gi1", ""), ("post", "y0_gi1", "_r2")),
        ("noise GI on y300: post vs post_r2", ("post", "y300_gi1", ""), ("post", "y300_gi1", "_r2")),
        ("noise GI on layers: post vs post_r2", ("post", "layers_y300_gi1", ""), ("post", "layers_y300_gi1", "_r2")),
        ("noise GI on y0: pre (S2) vs pre (S3)", ("pre", "y0_gi1", ""), ("pre", "y0_gi1", "_s3")),
        ("Fix A at origin, GI on y0: pre vs fixA", ("pre", "y0_gi1", ""), ("fixA", "y0_gi1", "")),
        ("Fix B alone, GI on y0: fixA vs post", ("fixA", "y0_gi1", ""), ("post", "y0_gi1", "")),
        ("Fix B alone, GI on y300: fixA vs post", ("fixA", "y300_gi1", ""), ("post", "y300_gi1", "")),
        ("Fix B alone, GI on layers: fixA vs post", ("fixA", "layers_y300_gi1", ""), ("post", "layers_y300_gi1", "")),
        ("A+B, GI on y0: pre vs post", ("pre", "y0_gi1", ""), ("post", "y0_gi1", "")),
        ("A+B, GI on y300: pre vs post", ("pre", "y300_gi1", ""), ("post", "y300_gi1", "")),
        ("A+B, GI on layers: pre vs post", ("pre", "layers_y300_gi1", ""), ("post", "layers_y300_gi1", "")),
    ]
    print("\n## Image diffs: mean |d| / max |d| / % px changed")
    print("| comparison | " + " | ".join(RHIS) + " |")
    print("|---|" + "---|" * len(RHIS))
    for label, (ta, ca, sa), (tb, cb, sb) in pairs:
        cells = []
        for rhi in RHIS:
            ia, ib = img(ta, rhi, ca, sa), img(tb, rhi, cb, sb)
            if ia is None or ib is None:
                cells.append("-")
                continue
            m, mx, pc = diff(ia, ib)
            cells.append(f"{m:.3f} / {mx} / {pc:.2f} %")
        print(f"| {label} | " + " | ".join(cells) + " |")

    print("\n## Mean terrain-box value (0-255), GI on y0 / y300: pre -> fixA -> post")
    print("| rhi | y0 pre | y0 fixA | y0 post | y300 pre | y300 fixA | y300 post |")
    print("|---|---|---|---|---|---|---|")
    for rhi in RHIS:
        cells = []
        for case in ("y0_gi1", "y300_gi1"):
            for tag in ("pre", "fixA", "post"):
                im = img(tag, rhi, case)
                cells.append("-" if im is None else f"{mean_lum(im, BOXES['terrain']):.1f}")
        print(f"| {rhi} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
