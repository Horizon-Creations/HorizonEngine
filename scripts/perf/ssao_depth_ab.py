#!/usr/bin/env python3
"""SSAO position pre-pass depth D16 -> D32 on D3D11/D3D12/Vulkan (Thema 182 step 6).

The pre-pass that feeds SSAO (and, with SSR on, the reflection MRT pre-pass) had its own
D16 depth on D3D11, D3D12 and Vulkan (GL 24 bit, Metal 32). From some tens of metres two
surfaces round to the same 16-bit value and the draw order decides which one lands in the
position target: foliage clusters against the ordered cluster differed in 0.21 % (D3D11,
D3D12) and 0.51 % (Vulkan) of the ground. Renders each case with the BEFORE and the AFTER
editor, AFTER twice for its own noise floor, and prints the ground comparison (rows from
--ground down, a pixel differs when a channel moves by more than --tol) plus the largest
channel step:

    FC   6000 plants, clusters, no levels      the foliage_lod_shot witness (50-90 m)
    FO   the same, HE_FOLIAGE_CLUSTERS=ordered  AFTER: FC == FO is the point of the change
    MT   mountain landscape, camera 200 m off   SSAO over terrain at 100+ m
    M0   MT with SSAO off                       control: before == after (only SSAO changed)
    SR   FC with SSR on                         runs the reflection MRT pre-pass (Vulkan :13201)

    python scripts/perf/ssao_depth_ab.py --before C:/hw182/deploy_before/Editor \\
        --after C:/hw182/deploy/Editor --rhi D3D11 --out C:/hw182/ssao_ab/D3D11
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from foliage_graph_ab import compare, editor_exe, pixels, shoot  # noqa: E402

PINS = ["COVERAGE=0", "CLOUDMODE=0", "CLOUDSHADOWS=0", "AA=0", "SSR=0", "TOD=0.4"]   # day: AO shows
FOLIAGE = ["FOLIAGETEST=6000", "FOLIAGESIZE=200", "FOLIAGEDIST=90", "WINDSPEED=0",
           "CAMX=0", "CAMY=6", "CAMZ=60", "PITCH=-6", "SHADOW=0.1"]
MOUNTAIN = ["MOUNTAINTEST=before", "CAMX=-90", "CAMY=340", "CAMZ=-200", "PITCH=-10", "YAW=180"]
CASES = {
    "FC": ("6000 plants, clusters",   FOLIAGE, {}),
    "FO": ("6000 plants, ordered",    FOLIAGE, {"HE_FOLIAGE_CLUSTERS": "ordered"}),
    "MT": ("mountain, SSAO on",       MOUNTAIN + ["SSAO=1"], {}),
    "M0": ("mountain, SSAO off",      MOUNTAIN + ["SSAO=0"], {}),
    "SR": ("6000 plants, SSR on",     FOLIAGE + ["SSR=1"], {}),
}


def max_step(a: Path, b: Path, ground: int) -> int:
    (ga, pa), (gb, pb) = pixels(a), pixels(b)
    if ga != gb:
        return -1
    w, h, bpp = ga
    bp = bpp // 8
    row = ((w * bp + 3) // 4) * 4
    m = 0
    for y in range(ground, h):
        ra, rb = pa[y * row:y * row + w * bp], pb[y * row:y * row + w * bp]
        if ra != rb:
            m = max(m, max(abs(x - z) for x, z in zip(ra, rb)))
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--rhi", required=True, choices=["OpenGL", "D3D11", "D3D12", "Vulkan", "Metal"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--ground", type=int, default=330, help="first row (top-down) compared")
    ap.add_argument("--tol", type=int, default=0)
    args = ap.parse_args()

    before, after, out = Path(args.before).resolve(), Path(args.after).resolve(), Path(args.out).resolve()
    for d in (before, after):
        if not editor_exe(d).is_file():
            print(f"no HorizonEditor in {d}", file=sys.stderr)
            return 2
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "config"
    cfg.mkdir(exist_ok=True)
    g, t = args.ground, args.tol
    names = [n for n in CASES if not args.only or n in args.only.split(",")]
    print(f"{'case':4} {'before vs after':34} {'noise (after vs after)':34} max step b-a/noise  [what]")
    for n in names:
        desc, keys, env = CASES[n]
        b, a, a2 = out / f"{n}_before.bmp", out / f"{n}_after.bmp", out / f"{n}_after2.bmp"
        ok = all(shoot(d, p, PINS + keys, args.rhi, cfg, env)[0] for d, p in ((before, b), (after, a), (after, a2)))
        if not ok:
            print(f"{n:4} NO IMAGE   [{desc}]")
            continue
        print(f"{n:4} {compare(b, a, g, t):34} {compare(a, a2, g, t):34} "
              f"{max_step(b, a, g)}/{max_step(a, a2, g)}   [{desc}]")
    if "FC" in names and "FO" in names:
        for side in ("before", "after"):
            fc, fo = out / f"FC_{side}.bmp", out / f"FO_{side}.bmp"
            if fc.exists() and fo.exists():
                print(f"clusters vs ordered, {side:6}: {compare(fc, fo, g, t)} (max step {max_step(fc, fo, g)})")
    print(f"images and logs in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
