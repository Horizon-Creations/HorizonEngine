#!/usr/bin/env python3
"""foliage_lod_shot — the foliage detail levels (Thema 182, 3a) on a real backend.

Renders the HE_DUMP_FOLIAGETEST witness four times with one editor binary:

    none     the layer without levels (cubes out to the draw distance)
    levels   HE_DUMP_FOLIAGELOD (default "25:sphere,50:card:0.5"): cubes near, spheres
             from 25 m, the engine's cross-card file from 50 m at half density
    ordered  the same levels, HE_FOLIAGE_CLUSTERS=ordered (one cluster per mesh, the
             per-instance draw order): the reference the cluster path is held to
    perinst  the same levels, HE_FOLIAGE_CLUSTERS=0 (one object per plant)

and prints, per shot, the draws and the extractor's per-level counts (HE_FOLIAGE_STATS),
then levels against ordered and against perinst, over the frame and over the ground only. Opaque geometry that touches resolves
depth ties by draw order, so clusters against the other two may differ in the odd pixel
along an intersection (see the lesson on depth ties); a difference beyond that, or a
level count that does not add up, is a finding. "none" against "levels" must differ.

    python scripts/perf/foliage_lod_shot.py --editor C:/hw182/deploy/Editor --rhi OpenGL
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

CAMERA = ["CAMX=0", "CAMY=6", "CAMZ=60", "PITCH=-6", "SHADOW=0.1"]


def editor_exe(deploy: Path) -> Path:
    for name in ("HorizonEditor.exe", "HorizonEditor"):
        if (deploy / name).is_file():
            return deploy / name
    return deploy / "HorizonEditor"


def shoot(deploy: Path, out: Path, rhi: str, cfg: Path, env_extra: dict) -> tuple[bool, int, str, str]:
    env = dict(os.environ)
    env.update({"HE_SKY_TIME": "3.0", "HE_CONFIG_DIR": str(cfg), "HE_COLLAB_OFFLINE": "1",
                "HE_NET_LOOPBACK_ONLY": "1", "HE_HIDDEN_WINDOW": "1", "HE_FOLIAGE_STATS": "1",
                "HE_DUMP_PATH": str(out), "HE_DUMP_QUIT": "1", "HE_DUMP_SKYTEST": "1",
                "HE_DUMP_RHI": rhi, "HE_DUMP_WINDSPEED": "0",
                "HE_DUMP_FOLIAGETEST": "6000", "HE_DUMP_FOLIAGESIZE": "200", "HE_DUMP_FOLIAGEDIST": "90"})
    for kv in CAMERA:
        k, v = kv.split("=", 1)
        env["HE_DUMP_" + k] = v
    env.update(env_extra)
    if out.exists():
        out.unlink()
    log = out.with_suffix(".log")
    try:
        with open(log, "wb") as f:
            subprocess.run([str(editor_exe(deploy))], env=env, cwd=str(deploy), timeout=300,
                           stdout=f, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        pass   # the dump may have been written before a hang on teardown; the file decides
    text = log.read_text(errors="replace") if log.exists() else ""
    m = re.search(r"dump counters\D+draws=(\d+)", text)
    stats = re.findall(r"FoliageExtract: .*", text)
    card = re.search(r"HE_DUMP_FOLIAGELOD cross card (.*)", text)
    ok = out.exists() and out.stat().st_size > 1024
    return ok, (int(m.group(1)) if m else -1), (stats[-1].strip() if stats else "-"), (card.group(1).strip() if card else "")


def pixels(path: Path):
    """((width, height, bpp), pixel bytes, top_down): the dumps are written top-down (negative height)."""
    data = path.read_bytes()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h, _planes, bpp = struct.unpack_from("<iiHH", data, 18)
    return (w, abs(h), bpp), data[off:], h < 0


def compare(a: Path, b: Path, ground_from: int = 0) -> str:
    """Differing pixels; with ground_from > 0 only the rows from that one (top-down) to the
    bottom: the ground with the plants. On D3D and Vulkan the sky differs from run to run
    (several % of the frame), the ground does not, so the ground is the number to read."""
    (ga, pa, ta), (gb, pb, tb) = pixels(a), pixels(b)
    if ga != gb or ta != tb:
        return f"DIFFERENT SIZE {ga} {gb}"
    w, h, bpp = ga
    bp = bpp // 8
    stride = ((w * bpp + 31) // 32) * 4
    rows = range(ground_from, h) if ta else range(0, h - ground_from)
    differing = 0
    for r in rows:
        ra, rb = pa[r * stride:r * stride + w * bp], pb[r * stride:r * stride + w * bp]
        if ra != rb:
            differing += sum(1 for i in range(0, w * bp, bp) if ra[i:i + bp] != rb[i:i + bp])
    total = w * len(rows)
    return "identical" if differing == 0 else f"{differing} of {total} pixels ({100.0 * differing / total:.3f} %)"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True, help="deploy dir of the editor to test")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "foliage_lod"), help="BMPs and logs")
    ap.add_argument("--rhi", default="Metal", choices=["Metal", "OpenGL", "D3D11", "D3D12", "Vulkan"])
    ap.add_argument("--levels", default="25:sphere,50:card:0.5", help="HE_DUMP_FOLIAGELOD")
    ap.add_argument("--ground", type=int, default=330, help="first image row (top-down) of the ground")
    args = ap.parse_args()

    deploy, out = Path(args.editor).resolve(), Path(args.out).resolve()
    if not editor_exe(deploy).is_file():
        print(f"no HorizonEditor in {deploy}", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "config"
    cfg.mkdir(exist_ok=True)
    shots = {
        "none":    {},
        "levels":  {"HE_DUMP_FOLIAGELOD": args.levels},
        "ordered": {"HE_DUMP_FOLIAGELOD": args.levels, "HE_FOLIAGE_CLUSTERS": "ordered"},
        "perinst": {"HE_DUMP_FOLIAGELOD": args.levels, "HE_FOLIAGE_CLUSTERS": "0"},
    }
    ok = True
    for name, extra in shots.items():
        bmp = out / f"{args.rhi}_{name}.bmp"
        good, draws, stats, card = shoot(deploy, bmp, args.rhi, cfg, extra)
        ok = ok and good
        print(f"{name:8} {'ok' if good else 'NO IMAGE':8} draws={draws:<6} {stats}" + (f"  [card {card}]" if card else ""))
    p = lambda n: out / f"{args.rhi}_{n}.bmp"
    if ok:
        g = args.ground
        for x, y in (("levels", "ordered"), ("levels", "perinst"), ("ordered", "perinst")):
            print(f"{x:7} vs {y:7}: frame {compare(p(x), p(y))}; ground {compare(p(x), p(y), g)}")
        none_vs = compare(p('none'), p('levels'), g)
        print(f"none    vs levels : ground {none_vs}  (must differ)")
        ok = none_vs != "identical"
    print(f"images and logs in {out}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
