#!/usr/bin/env python3
"""water_baseline_shots — the fixed before/after views of the water topic (Thema 206).

Renders the same views on Metal and OpenGL with everything pinned (sky clock,
render path, AA, a throw-away config dir), so a later step can shoot the identical
frames and compare. Output: <outdir>/baseline-<backend>-<view>.png (names kept
stable on purpose; pass --prefix after to get after-<backend>-<view>.png).

Views:
  open     engine water plane, standard camera of docs/water-shader-plan.md 9.1
           (waves, normals, reflection; no shore)
  open-hi  the same plane from 14 m up at 40 degrees, wave detail
  far      the 400 m plane from 3 m up at 6 degrees: the distance fades, horizon aliasing
  shore    HE_DUMP_WATERLAKE=bowl, the curved bank, grid 256 (0.5 m cells)
  shore33  the same with HE_DUMP_WATERRES=33 (3.9 m cells = a 1000 m landscape at res 256)
  shoresheet33  shore33 with the landscape hidden (the sheet's own outline)
  shallow, shallow-hi  the wide shallow bank seen from 6 m / 4 m up (the ground through the water)
  beach, beach33, beach-smooth, beach-sheet   the jagged shore of a gentle rough beach (see VIEWS)

Usage: scripts/water_baseline_shots.py OUTDIR [--prefix baseline] [--backends Metal,OpenGL]
                                        [--views open,shore,...] [--repeat 2]
--repeat 2 renders every frame twice and prints both md5s (the reproducibility check).
"""
import argparse, hashlib, os, pathlib, shutil, subprocess, sys, tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SHOT = REPO / "scripts" / "he_shot.py"

COMMON = dict(TOD="0.4", RENDERPATH="0", AA="0")
VIEWS = {
    "open":    dict(WATERTEST="floor", CAMY="4", CAMZ="6", PITCH="-25"),
    "open-hi": dict(WATERTEST="floor", CAMY="14", CAMZ="10", PITCH="-40"),
    # A 400 m plane seen at a low angle: the chop's distance fades and the horizon (Thema 206 step 3).
    "far":     dict(WATERTEST="floor", WATERSIZE="400", CAMY="3", CAMZ="0", PITCH="-6"),
    "shore":   dict(WATERLAKE="bowl", CAMY="375", CAMZ="48", PITCH="-60"),
    "shore33": dict(WATERLAKE="bowl", CAMY="375", CAMZ="48", PITCH="-60", WATERRES="33"),
    "shoresheet33": dict(WATERLAKE="bowl", CAMY="375", CAMZ="48", PITCH="-60", WATERRES="33",
                         WATERHIDEGROUND="1"),
    # The jagged shore of a gentle beach: slope 0.04 m/m, 3 cm of height-field jitter, water
    # 0.3 m over the ground (HE_DUMP_WATERSLOPE / WATERROUGH / WATERLEVEL). Grid 256, then 33
    # (3.9 m cells), then the sheet alone. The three show the zigzag is the terrain facets cutting
    # a hard-edged sheet, not the water grid.
    "beach":   dict(WATERLAKE="slope", WATERSLOPE="0.04", WATERROUGH="0.03", WATERLEVEL="0.3",
                    CAMX="8", CAMY="318", CAMZ="34", PITCH="-45"),
    "beach33": dict(WATERLAKE="slope", WATERSLOPE="0.04", WATERROUGH="0.03", WATERLEVEL="0.3",
                    CAMX="8", CAMY="318", CAMZ="34", PITCH="-45", WATERRES="33"),
    # Shallow water over a wide gentle bank (slope 0.02, 0.5 m of water at the shore line): the ground
    # through the water and the blend to the edge (Thema 206 step 4). Low camera, 30 degrees.
    "shallow": dict(WATERLAKE="slope", WATERSLOPE="0.02", WATERROUGH="0.015", WATERLEVEL="0.5",
                    CAMX="8", CAMY="306", CAMZ="20", PITCH="-30"),
    "shallow-hi": dict(WATERLAKE="slope", WATERSLOPE="0.02", WATERROUGH="0.015", WATERLEVEL="0.5",
                       CAMX="2", CAMY="304", CAMZ="14", PITCH="-40"),
    "beach-smooth": dict(WATERLAKE="slope", WATERSLOPE="0.04", WATERLEVEL="0.3",
                         CAMX="8", CAMY="318", CAMZ="34", PITCH="-45"),
    "beach-sheet": dict(WATERLAKE="slope", WATERSLOPE="0.04", WATERROUGH="0.03", WATERLEVEL="0.3",
                        CAMX="8", CAMY="318", CAMZ="34", PITCH="-45", WATERHIDEGROUND="1"),
}


def md5(p):
    return hashlib.md5(pathlib.Path(p).read_bytes()).hexdigest()


def shoot(out, view, backend):
    cfg = tempfile.mkdtemp(prefix="hecfg_")
    env = dict(os.environ, HE_SKY_TIME="1.0", HE_CONFIG_DIR=cfg, HE_HIDDEN_WINDOW="1",
               HE_SHOT_TIMEOUT=os.environ.get("HE_SHOT_TIMEOUT", "400"))
    kv = dict(COMMON, **VIEWS[view], RHI=backend)
    args = [sys.executable, str(SHOT), str(out)] + [f"{k}={v}" for k, v in kv.items()]
    r = subprocess.run(args, env=env, capture_output=True, text=True)
    shutil.rmtree(cfg, ignore_errors=True)
    ok = pathlib.Path(out).exists() and pathlib.Path(out).stat().st_size > 1000
    if not ok:
        print(f"FAILED {out}\n{r.stdout[-800:]}\n{r.stderr[-800:]}", file=sys.stderr)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--prefix", default="baseline")
    ap.add_argument("--backends", default="Metal,OpenGL")
    ap.add_argument("--views", default=",".join(VIEWS))
    ap.add_argument("--repeat", type=int, default=1)
    a = ap.parse_args()
    out = pathlib.Path(a.outdir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for be in a.backends.split(","):
        for v in a.views.split(","):
            sums = []
            for i in range(a.repeat):
                p = out / (f"{a.prefix}-{be.lower()}-{v}.png" if i == 0 else f"_repeat{i}-{be.lower()}-{v}.png")
                if shoot(p, v, be):
                    sums.append(md5(p))
                else:
                    bad += 1
            print(f"{a.prefix}-{be.lower()}-{v}: {' '.join(sums)}"
                  + ("  (identical)" if len(sums) > 1 and len(set(sums)) == 1 else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
