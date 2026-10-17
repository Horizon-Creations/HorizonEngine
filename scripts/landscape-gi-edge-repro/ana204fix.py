"""Thema 204 step 2: did the D16 -> D32 GI G-buffer depth fix take the seam lines away?

  python ana204fix.py dump BEFORE_DIR AFTER_DIR NAME
      cap190.ps1 captures (NAME-{lod0,nostitch,stitched}-<RHI>.bmp): per variant
      the pixels of D3D11/D3D12/OpenGL that are > 25 luma DARKER than Vulkan of
      the same build. Vulkan is the reference (D32 all along); defect B is D3D only.

  python ana204fix.py live BEFORE_DIR AFTER_DIR CAM [MASK_OUT_DIR]
      live190.py shots (<rhi>_<CAM>_<shot>.png): per backend the pixels that got
      > 25 luma brighter / darker from BEFORE to AFTER. Vulkan before->after is
      the noise floor (its code did not change). On a project whose material
      differs between backends (the hex-tiled placeholder on release/0.7.0) the
      D3D-vs-Vulkan count of the dump mode is swamped by texture filtering, the
      same-backend delta is not. MASK_OUT_DIR: the brightened pixels painted red
      over the BEFORE picture, per D3D shot.
"""
import os
import sys
import numpy as np
from PIL import Image

SHOTS = ("flat_close", "flat_mid", "flat_far", "mtn_close", "mtn_far")


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def dump(before, after, name):
    for side, d in (("before", before), ("after", after)):
        for v in ("lod0", "nostitch", "stitched"):
            ref = luma(f"{d}/{name}-{v}-Vulkan.bmp")
            row = []
            for b in ("D3D11", "D3D12", "OpenGL"):
                p = f"{d}/{name}-{v}-{b}.bmp"
                if os.path.exists(p):
                    row.append(f"{b}={int(np.count_nonzero(luma(p) < ref - 25)):6d}")
            print(f"{side:6} {v:9}", " ".join(row))


def live(before, after, cam, mask_dir=None):
    print("px > 25 brighter | darker, before -> after, same backend")
    for b in ("vk", "d3d11", "d3d12", "gl"):
        row = []
        for s in SHOTS:
            pb, pa = f"{before}/{b}_{cam}_{s}.png", f"{after}/{b}_{cam}_{s}.png"
            if not (os.path.exists(pb) and os.path.exists(pa)):
                continue
            d = luma(pa) - luma(pb)
            row.append(f"{s}={int(np.count_nonzero(d > 25))}|{int(np.count_nonzero(d < -25))}")
            if mask_dir and b.startswith("d3d"):
                rgb = np.asarray(Image.open(pb).convert("RGB")).copy()
                rgb[d > 25] = (255, 0, 0)
                Image.fromarray(rgb).save(os.path.join(mask_dir, f"mask_{b}_{cam}_{s}.png"))
        if row:
            print(f"{b:6}", "  ".join(row))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "dump" and len(a) == 4:
        dump(a[1], a[2], a[3])
    elif a and a[0] == "live" and len(a) in (4, 5):
        live(a[1], a[2], a[3], a[4] if len(a) == 5 else None)
    else:
        print(__doc__)
        sys.exit(2)
