#!/usr/bin/env python3
"""Foliage picture before against after Thema 182, on one backend (Thema 182 step 5).

Renders the HE_DUMP_FOLIAGETEST witness with two deployed editors, BEFORE (a build of the
release/0.7.0 merge base: clusters already, but no graph-material instancing and no instance arena)
and AFTER, and compares ground and plant mask the way foliage_graph_ab.py does. The AFTER editor
runs twice, so every case also prints its own run-to-run noise floor (after against after):

    S   6000 plants, forward        built-in cube; below every ring: must match the noise floor
    SD  6000 plants, deferred       the same through the G-buffer
    C   100k, drawDistance 100 m    camera in the field, about 20 % in range
    A   100k, everything in range   past the old 65536-instance ring: D3D12 drew sky and grey only
    M   500k, everything in range   past the arena's 1M instances per frame slot on D3D12/Vulkan
    G   800 plants, graph material  BEFORE loops per plant, AFTER draws the instanced twin

`draws`/`tris` come from each run's "dump counters" line (D3D12's include the shadow cascades).
D3D11/D3D12/Vulkan: the sky differs between two identical runs, compare from row 100 down with
--ground 100 --tol 8, on Vulkan also --mask-tol 4 (see foliage_graph_ab.py).

    python scripts/perf/foliage_backend_ab.py --before C:/hw182b/deploy/Editor \\
        --after C:/hw182/deploy/Editor --rhi D3D12 --out C:/hw182/s5/img/D3D12 --ground 100 --tol 8
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from foliage_graph_ab import compare, editor_exe, mask_moved, shoot  # noqa: E402

PINS = ["COVERAGE=0", "CLOUDMODE=0", "CLOUDSHADOWS=0", "AA=0"]
FAR = ["CAMX=0", "CAMY=40", "CAMZ=-120", "PITCH=-20", "TOD=0.5"]
NEAR = ["FOLIAGESIZE=100", "CAMX=0", "CAMY=25", "CAMZ=60", "PITCH=-20", "TOD=0.4", "SHADOW=0.1"]
CASES = {
    "S":  ("6000 plants, forward",            ["FOLIAGETEST=6000"] + NEAR + ["RENDERPATH=0"]),
    "SD": ("6000 plants, deferred",           ["FOLIAGETEST=6000"] + NEAR + ["RENDERPATH=1"]),
    "C":  ("100k, drawDistance 100 m",        ["FOLIAGETEST=100000", "FOLIAGEDIST=100", "CAMX=0", "CAMY=30",
                                               "CAMZ=0", "PITCH=-20", "TOD=0.5", "RENDERPATH=0"]),
    "A":  ("100k, everything in range",       ["FOLIAGETEST=100000"] + FAR + ["RENDERPATH=0"]),
    "M":  ("500k, everything in range",       ["FOLIAGETEST=500000"] + FAR + ["RENDERPATH=0"]),
    "G":  ("800 plants, graph material",     ["FOLIAGETEST=800", "FOLIAGEMAT=graph"] + NEAR + ["RENDERPATH=0"]),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--rhi", required=True, choices=["Metal", "OpenGL", "D3D11", "D3D12", "Vulkan"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--ground", type=int, default=0)
    ap.add_argument("--tol", type=int, default=0)
    ap.add_argument("--frames", default="", help="HE_DUMP_FRAMES (D3D12 slot growth shows from 8)")
    args = ap.parse_args()

    before, after, out = Path(args.before).resolve(), Path(args.after).resolve(), Path(args.out).resolve()
    for d in (before, after):
        if not editor_exe(d).is_file():
            print(f"no HorizonEditor in {d}", file=sys.stderr)
            return 2
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "config"
    cfg.mkdir(exist_ok=True)
    extra = [f"FRAMES={args.frames}"] if args.frames else []
    print(f"{'case':4} {'before vs after':30} {'noise (after vs after)':30} mask px b-a/noise   draws b/a   tris b/a")
    for n in [n for n in CASES if not args.only or n in args.only.split(",")]:
        desc, keys = CASES[n]
        keys = PINS + keys + extra
        b, a, a2 = out / f"{n}_before.bmp", out / f"{n}_after.bmp", out / f"{n}_after2.bmp"
        okb, db, tb = shoot(before, b, keys, args.rhi, cfg)
        oka, da, ta = shoot(after, a, keys, args.rhi, cfg)
        ok2, _, _ = shoot(after, a2, keys, args.rhi, cfg)
        if not (okb and oka and ok2):
            print(f"{n:4} NO IMAGE (before {okb}, after {oka}, after2 {ok2})   [{desc}]")
            continue
        ba = compare(b, a, args.ground, args.tol)
        noise = compare(a, a2, args.ground, args.tol)
        mb, mn = mask_moved(b, a)[1], mask_moved(a, a2)[1]
        print(f"{n:4} {ba:30} {noise:30} {mb}/{mn}   {db}/{da}   {tb}/{ta}   [{desc}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
