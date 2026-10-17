#!/usr/bin/env python3
"""underwater_backend_compare — the Thema 205 step 5 tables: D3D11/D3D12/Vulkan against OpenGL and Metal.

Input: one folder per backend, each filled by scripts/underwater_look_shots.py --rhi <name> (same view names),
plus optional `<view>_off.bmp` shots taken with HE_DUMP_UWCAUSTICS=0 (the net-off control). Needs numpy and
PIL (both on NN-WS03; the Mac has neither, there the no-numpy scripts underwater_acceptance.py --compare and
underwater_reference_compare.py measure the same things more slowly).

  python scripts/underwater_backend_compare.py --shots C:/hw205/shots --ref GL \
         --backends D3D11,D3D12,Vulkan --metal docs/images --photo .hive/dateien/32-...jpg

Prints, per view: mean |d| per channel (0..255), max and % of pixels with a channel > 2, for every backend
against the reference backend and against the Metal picture in docs/images where one exists (PNG = lossless;
JPG adds ~1 of compression noise, so the reference backend's own row against the JPG is the floor to read
the others against). Then the line share (underwater_reference_compare's measure) and the net on/off delta.
"""
import argparse, pathlib, sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from underwater_reference_compare import measure  # noqa: E402

# view -> Metal picture of the current state (steps 3 and 4) in docs/images
METAL = {
    "up": "underwater-underside-up-metal.png",
    "up_nofog": "underwater-underside-up_nofog-metal.png",
    "zenith": "underwater-underside-zenith-metal.png",
    "deep_up": "underwater-underside-deep_up-metal.png",
    "reference": "underwater-acceptance-reference-metal.jpg",
    "floor": "underwater-acceptance-floor-metal.jpg",
}


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


def delta(a, b):
    d = np.abs(a - b)
    return d.mean(), int(d.max()), 100.0 * (d.max(axis=2) > 2).mean()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", required=True)
    ap.add_argument("--ref", default="GL")
    ap.add_argument("--backends", default="D3D11,D3D12,Vulkan")
    ap.add_argument("--metal", default="docs/images")
    ap.add_argument("--photo")
    a = ap.parse_args()
    root = pathlib.Path(a.shots)
    others = a.backends.split(",")
    views = sorted(p.stem for p in (root / a.ref).glob("*.bmp") if not p.stem.endswith("_off"))

    print(f"## mean |d| per channel, max, % px > 2   (against {a.ref}; 'M' = against Metal docs/images)")
    for v in views:
        ref = load(root / a.ref / f"{v}.bmp")
        cells = []
        metal = METAL.get(v)
        mimg = load(pathlib.Path(a.metal) / metal) if metal else None
        if mimg is not None:
            m = delta(ref, mimg)
            cells.append(f"{a.ref}-M {m[0]:5.2f}/{m[1]:3d}/{m[2]:4.1f}")
        for b in others:
            p = root / b / f"{v}.bmp"
            if not p.exists():
                cells.append(f"{b} missing")
                continue
            img = load(p)
            g = delta(img, ref)
            cell = f"{b} {g[0]:5.2f}/{g[1]:3d}/{g[2]:4.1f}"
            if mimg is not None:
                cell += f" M {delta(img, mimg)[0]:5.2f}"
            cells.append(cell)
        print(f"{v:22s} " + " | ".join(cells))

    print("\n## line share (cell / peak / contrast / lines %)")
    rows = []
    if a.photo:
        rows += [("photo", a.photo + ":0,0,1,0.93"), ("photo near", a.photo + ":0,0.33,1,0.93")]
    for v in ("reference", "reference_nofog", "shore", "floor"):
        for b in [a.ref] + others:
            p = root / b / f"{v}.bmp"
            if p.exists():
                rows.append((f"{v} {b}", str(p)))
    for name, spec in rows:
        m = measure(spec)
        print(f"{name:28s} {m['cell']:6.1f} {m['peak']:6.1f} {m['contrast']:6.2f} {m['lines_pct']:6.1f}")

    print("\n## net on/off (HE_DUMP_UWCAUSTICS=0): mean |d| per channel, % px > 2")
    for b in [a.ref] + others:
        for off in sorted((root / b).glob("*_off.bmp")):
            v = off.stem[:-4]
            on = root / b / f"{v}.bmp"
            if on.exists():
                g = delta(load(on), load(off))
                print(f"{b:8s} {v:18s} {g[0]:6.2f} {g[2]:5.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
