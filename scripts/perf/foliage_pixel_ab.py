#!/usr/bin/env python3
"""Pixel A/B of the foliage cluster path against a build from before it (Thema 163, Teil 2a).

Renders the HE_DUMP_FOLIAGETEST witness with two deployed editors, a BEFORE one (a self-contained
copy of an older deploy, see scripts/perf/selfcontain_deploy_copy.sh) and the AFTER one, and compares
the raw BMPs. The AFTER editor is run three ways:

    HE_FOLIAGE_CLUSTERS=0         one RenderObject per plant, the old path: must be IDENTICAL
    HE_FOLIAGE_CLUSTERS=ordered   one cluster, drawn in the old path's order:  must be IDENTICAL
    (default)                     buckets as clusters: may differ in a few pixels, where two opaque
                                  surfaces meet at the same depth and the draw order decides

so a difference in the first two is a bug in the store, the unfolding or the depth passes, and the third
shows how far the draw order alone moves the picture. Everything is pinned: HE_SKY_TIME, a private
HE_CONFIG_DIR, clouds off, AA off. Nothing is converted (no PIL on the system Python): the BMPs are read
with struct. Typical use:

    cp -cR out/deploy/Editor /tmp/deploy_before          # BEFORE the change is built
    zsh scripts/perf/selfcontain_deploy_copy.sh /tmp/deploy_before
    ... build the change ...
    python3 -I scripts/perf/foliage_pixel_ab.py --before /tmp/deploy_before --after out/deploy/Editor \\
        --out /tmp/foliage_ab [--rhi OpenGL] [--only A,C] [--keep]

Exit status 0 when the old path and `ordered` are identical in every case, 1 otherwise.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

PINS = ["COVERAGE=0", "CLOUDMODE=0", "CLOUDSHADOWS=0", "AA=0"]
VIEW = ["CAMX=0", "CAMY=40", "CAMZ=-120", "PITCH=-20"]
# name -> (description, HE_DUMP_ keys)
CASES = {
    "A": ("100k, everything in range, forward",           ["FOLIAGETEST=100000"] + VIEW + ["TOD=0.5", "RENDERPATH=0"]),
    "B": ("100k, everything in range, deferred",           ["FOLIAGETEST=100000"] + VIEW + ["TOD=0.5", "RENDERPATH=1"]),
    "C": ("100k, drawDistance 100 m, camera in the field", ["FOLIAGETEST=100000", "FOLIAGEDIST=100", "CAMX=0", "CAMY=30", "CAMZ=0",
                                                          "PITCH=-20", "TOD=0.5", "RENDERPATH=0"]),
    "D": ("100k, low sun (long shadows)",                  ["FOLIAGETEST=100000"] + VIEW + ["TOD=0.32", "RENDERPATH=0"]),
    "E": ("100k, scale 0.8..1.6, mesh id nobody registered (default cube, clusters without bounds)",
          ["FOLIAGETEST=100000", "FOLIAGEMESH=sphere", "FOLIAGESCALE=0.8,1.6"] + VIEW + ["TOD=0.32", "RENDERPATH=0"]),
    "G": ("500k, everything in range (past the 65536 instance cap)", ["FOLIAGETEST=500000"] + VIEW + ["TOD=0.5", "RENDERPATH=0"]),
}


def shoot(deploy: Path, out: Path, keys, rhi: str, extra_env=None) -> bool:
    env = dict(os.environ)
    env.update({"HE_SKY_TIME": "30", "HE_CONFIG_DIR": "/tmp/foliage_ab_cfg", "HE_COLLAB_OFFLINE": "1",
                "HE_DUMP_PATH": str(out), "HE_DUMP_QUIT": "1", "HE_DUMP_SKYTEST": "1", "HE_DUMP_RHI": rhi})
    for kv in PINS + keys:
        k, v = kv.split("=", 1)
        env["HE_DUMP_" + k.upper()] = v
    env.update(extra_env or {})
    if out.exists():
        out.unlink()
    try:
        subprocess.run([str(deploy / "HorizonEditor")], env=env, cwd=str(deploy), timeout=300,
                       stdout=open(out.with_suffix(".log"), "wb"), stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        pass   # the dump may have been written before a hang on teardown; the file decides
    return out.exists() and out.stat().st_size > 1024


def pixels(path: Path):
    data = path.read_bytes()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h, _planes, bpp = struct.unpack_from("<iiHH", data, 18)
    return (w, abs(h), bpp), data[off:]


def compare(a: Path, b: Path) -> str:
    (ga, pa), (gb, pb) = pixels(a), pixels(b)
    if ga != gb:
        return f"DIFFERENT SIZE {ga} {gb}"
    if pa == pb:
        return "identical"
    bp = ga[2] // 8
    differing, worst = 0, 0
    for i in range(0, min(len(pa), len(pb)) - bp + 1, bp):
        if pa[i:i + bp] != pb[i:i + bp]:
            differing += 1
            worst = max(worst, max(abs(pa[i + k] - pb[i + k]) for k in range(min(bp, 3))))
    total = ga[0] * ga[1]
    return f"{differing} of {total} pixels ({100.0 * differing / total:.3f} %), worst channel {worst}/255"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True, help="deploy dir of the editor before the change (self-contained copy)")
    ap.add_argument("--after", required=True, help="deploy dir of the editor with the change")
    ap.add_argument("--out", default="/tmp/foliage_ab", help="where the BMPs and logs go")
    ap.add_argument("--rhi", default="Metal", choices=["Metal", "OpenGL"])
    ap.add_argument("--only", default="", help="comma list of case names (default: all)")
    ap.add_argument("--keep", action="store_true", help="keep the BMPs of cases that are identical too")
    args = ap.parse_args()

    before, after, out = Path(args.before).resolve(), Path(args.after).resolve(), Path(args.out).resolve()
    for d in (before, after):
        if not (d / "HorizonEditor").exists():
            print(f"no HorizonEditor in {d}", file=sys.stderr)
            return 2
    out.mkdir(parents=True, exist_ok=True)
    names = [n for n in CASES if not args.only or n in args.only.split(",")]
    ok = True
    print(f"{'case':4} {'old path (=0)':14} {'ordered':14} default clusters")
    for n in names:
        desc, keys = CASES[n]
        t0 = time.time()
        ref = out / f"{n}_before.bmp"
        if not shoot(before, ref, keys, args.rhi):
            print(f"{n:4} no BEFORE image ({desc})")
            ok = False
            continue
        row = {}
        for mode, env in (("old", {"HE_FOLIAGE_CLUSTERS": "0"}), ("ordered", {"HE_FOLIAGE_CLUSTERS": "ordered"}), ("clusters", {})):
            img = out / f"{n}_{mode}.bmp"
            row[mode] = compare(ref, img) if shoot(after, img, keys, args.rhi, env) else "NO IMAGE"
            if mode in ("old", "ordered") and row[mode] != "identical":
                ok = False
            elif not args.keep and row[mode] == "identical":
                img.unlink(missing_ok=True)
        print(f"{n:4} {row['old']:14} {row['ordered']:14} {row['clusters']}   [{desc}; {time.time() - t0:.0f} s]")
    print("OK: the old path and ordered mode reproduce the BEFORE picture" if ok else
          "FAILED: the old path or ordered mode differs from the BEFORE picture")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
