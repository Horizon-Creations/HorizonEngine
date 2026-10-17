#!/usr/bin/env python3
"""Graph-material foliage drawn instanced against the per-instance loop, wind included (Thema 182, 2e).

Renders the HE_DUMP_FOLIAGETEST witness with a node-graph material on the layer
(HE_DUMP_FOLIAGEMAT=graph: a flat magenta ConstColor; =wind: the same plus Wind Sway on the
WPO pin) with ONE deployed editor, twice per case:

    HE_GRAPH_INSTANCING=1 (default)  the batch draws once with the material's instanced twin
    HE_GRAPH_INSTANCING=0            the old per-instance loop with the plain program

The two must be IDENTICAL (raw BMP bytes), and the instanced run must report fewer draws in
its "dump counters" line. Two controls make sure the wind case is not identical by accident,
i.e. that the offset really moves geometry: wind speed 0 against 20, and the graph clock
(HE_SKY_TIME) 3.0 against 7.3, each must change the PLANT MASK (pixels with red and blue well
above green). A whole-image diff is useless for that: the wind speed also moves the clouds and
their shadows, and the clock moves the sky.

Pinned: HE_SKY_TIME (the graph's Time input), HE_DUMP_WINDSPEED, a private HE_CONFIG_DIR,
camera on the field (the dump camera looks along -Z; a 100 m field needs CAMZ=60), shadows at
0.1 so the comparison is the material's, not the cascade fit's. BMPs are read with struct.

    python3 scripts/perf/foliage_graph_ab.py --editor out/deploy/Editor --out /tmp/graph_ab [--rhi Metal]
    python  scripts/perf/foliage_graph_ab.py --editor C:/hw182/deploy/Editor --out C:/tmp/graph_ab --rhi OpenGL

Exit status 0 when every case is identical with fewer draws and both controls move the plants.
Note for GL on Windows: the program-binary cache (glprogcache) lives under SDL_GetPrefPath, which a
private config dir does NOT redirect; the log says per run whether a program was built or loaded.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

VIEW = ["CAMX=0", "CAMY=25", "CAMZ=60", "PITCH=-20", "TOD=0.4", "SHADOW=0.1"]
FIELD = ["FOLIAGETEST=2000", "FOLIAGESIZE=100"]
CASES = {
    "WF": ("Wind Sway, forward",  FIELD + VIEW + ["FOLIAGEMAT=wind", "RENDERPATH=0"]),
    "WD": ("Wind Sway, deferred", FIELD + VIEW + ["FOLIAGEMAT=wind", "RENDERPATH=1"]),
    "GF": ("flat graph, forward",  FIELD + VIEW + ["FOLIAGEMAT=graph", "RENDERPATH=0"]),
    "GD": ("flat graph, deferred", FIELD + VIEW + ["FOLIAGEMAT=graph", "RENDERPATH=1"]),
}


def editor_exe(deploy: Path) -> Path:
    for name in ("HorizonEditor.exe", "HorizonEditor"):
        if (deploy / name).is_file():
            return deploy / name
    return deploy / "HorizonEditor"


def shoot(deploy: Path, out: Path, keys, rhi: str, cfg: Path, extra_env=None):
    env = dict(os.environ)
    env.update({"HE_SKY_TIME": "3.0", "HE_CONFIG_DIR": str(cfg), "HE_COLLAB_OFFLINE": "1",
                "HE_NET_LOOPBACK_ONLY": "1", "HE_HIDDEN_WINDOW": "1",
                "HE_DUMP_PATH": str(out), "HE_DUMP_QUIT": "1", "HE_DUMP_SKYTEST": "1",
                "HE_DUMP_RHI": rhi, "HE_DUMP_WINDSPEED": "20"})
    for kv in keys:
        k, v = kv.split("=", 1)
        env["HE_DUMP_" + k.upper()] = v
    env.update(extra_env or {})
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
    ok = out.exists() and out.stat().st_size > 1024
    return ok, (int(m.group(1)) if m else -1)


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
    differing = sum(1 for i in range(0, len(pa) - bp + 1, bp) if pa[i:i + bp] != pb[i:i + bp])
    total = ga[0] * ga[1]
    return f"{differing} of {total} pixels ({100.0 * differing / total:.3f} %)"


def plant_mask(path: Path):
    (w, h, bpp), px = pixels(path)
    bp = bpp // 8
    row = ((w * bp + 3) // 4) * 4
    m = bytearray(w * h)
    for y in range(h):
        base = y * row
        for x in range(w):
            i = base + x * bp
            b, g, r = px[i], px[i + 1], px[i + 2]
            m[y * w + x] = 1 if (r - g > 60 and b - g > 60) else 0
    return m


def mask_moved(a: Path, b: Path):
    ma, mb = plant_mask(a), plant_mask(b)
    na = sum(ma)
    moved = sum(1 for i in range(len(ma)) if ma[i] != mb[i])
    return na, moved


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True, help="deploy dir of the editor to test")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "graph_ab"), help="BMPs and logs")
    ap.add_argument("--rhi", default="Metal", choices=["Metal", "OpenGL", "D3D11", "D3D12", "Vulkan"])
    ap.add_argument("--only", default="", help="comma list of case names (default: all)")
    args = ap.parse_args()

    deploy, out = Path(args.editor).resolve(), Path(args.out).resolve()
    if not editor_exe(deploy).is_file():
        print(f"no HorizonEditor in {deploy}", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "config"
    cfg.mkdir(exist_ok=True)
    ok = True
    print(f"{'case':4} {'instanced vs loop':22} draws inst/loop")
    for n in [n for n in CASES if not args.only or n in args.only.split(",")]:
        desc, keys = CASES[n]
        a, b = out / f"{n}_inst.bmp", out / f"{n}_loop.bmp"
        oka, da = shoot(deploy, a, keys, args.rhi, cfg, {"HE_GRAPH_INSTANCING": "1"})
        okb, db = shoot(deploy, b, keys, args.rhi, cfg, {"HE_GRAPH_INSTANCING": "0"})
        verdict = compare(a, b) if oka and okb else "NO IMAGE"
        good = verdict == "identical" and 0 < da < db
        ok &= good
        print(f"{n:4} {verdict:22} {da}/{db}   [{desc}]{'' if good else '  <-- FAILED'}")

    # Controls: the wind really moves the plants (instanced path, forward).
    wind = CASES["WF"][1]
    ref = out / "WF_inst.bmp"
    if not ref.exists():
        shoot(deploy, ref, wind, args.rhi, cfg)
    for name, extra in (("wind speed 0 vs 20", {"HE_DUMP_WINDSPEED": "0"}),
                        ("clock 7.3 vs 3.0", {"HE_SKY_TIME": "7.3"})):
        img = out / f"control_{name.split()[0]}.bmp"
        if not shoot(deploy, img, wind, args.rhi, cfg, extra)[0] or not ref.exists():
            print(f"control {name}: NO IMAGE")
            ok = False
            continue
        na, moved = mask_moved(ref, img)
        good = na > 0 and moved > na // 50   # more than 2 % of the plant pixels move
        ok &= good
        print(f"control {name}: plant mask moved at {moved} of {na} plant px{'' if good else '  <-- FAILED'}")
    print("OK: instanced == loop, fewer draws, and the wind moves the plants" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
