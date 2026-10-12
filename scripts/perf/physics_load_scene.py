#!/usr/bin/env python3
"""A loose HorizonGame directory with a physics load scene (Thema 178, step 1). Measurement tool only.

  python3 scripts/perf/physics_load_scene.py --runtime /tmp/he178/gamebase --out /tmp/he178/g_dense5k \
      --layout dense --bodies 5000 [--characters 100] [--seed 1]

--layout sparse  bodies on a 3 m grid, dropped from 4..24 m: many ACTIVE bodies, hardly any contacts
--layout dense   a lattice 1.2 m apart, five layers high, dropped together: contact-rich and, with restitution
                 0.9 on both sides, it keeps moving for the whole capture instead of falling asleep (Jolt
                 sleeps a resting pile after 0.5 s and the engine has no switch to forbid it; a settled pile
                 costs next to nothing, which is the wrong thing to baseline)
--layout lake    (Thema 179, step 6) a flat 96 m landscape with an 80 x 80 m lake, its surface at y = 3 over a bed at
                 y = 0, and --bodies one-metre crates in one lattice layer 2 m apart, dropped from 1.5 m above the
                 surface. Densities cycle through --densities (kg/m3, default 300,600,900,1200,2500: three float, two
                 sink and rest on the bed). Restitution 0, so a crate that reaches the bed lies still instead of
                 bouncing for the whole capture. --no-lake leaves the water out of the same scene (the crates then
                 drop onto the bed); HE_PHYSICS_WATER=0 in the environment keeps the lake and switches its effect off.
--characters N   N standing characters (CharacterController + kinematic proxy body + capsule) spread over the
                 field: they run CharacterVirtual::ExtendedUpdate every step. They do not walk (no script).

The game dir is a copy of --runtime (APFS clone) plus Bench.hescene and a version-2 project.hcfg, run with
  HE_PROFILE_CAPTURE=600 HE_PROFILE_WARMUP=300 HE_PROFILE_COUNTERS=0 HE_HIDDEN_WINDOW=1 <out>/HorizonGame
(see scripts/perf/physics_load_run.sh). Entity count is bodies + characters + 4.
"""
import argparse
import base64
import json
import math
import random
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_cell_game_dir import write_hcfg  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
CUBE, SPHERE, NO_ASSET = [1, 1], [257, 1], [0, 0]


def uid(rng):
    return [rng.getrandbits(64) | 1, rng.getrandbits(64) | 1]


def varint(v):
    """LEB128, as HE::water::putVarint writes it."""
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def lake_cells(res, half_cells):
    """HE::water::encodeCells for one body (id 1): cells within +-half_cells of the middle, full coverage, row-major z*res+x."""
    lo, hi = res // 2 - half_cells, res // 2 + half_cells
    out = bytearray()
    cell = lambda wet: bytes([1, 0, 255]) if wet else bytes([0, 0, 0])   # owner u16 LE, coverage u8
    runs = []   # (count, wet)
    for z in range(res):
        for x in range(res):
            wet = lo <= x < hi and lo <= z < hi
            if runs and runs[-1][1] == wet:
                runs[-1][0] += 1
            else:
                runs.append([1, wet])
    for count, wet in runs:
        out += varint(count) + cell(wet)
    return bytes(out)


def tf(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--layout", choices=["sparse", "dense", "lake"], default="sparse")
    ap.add_argument("--no-lake", action="store_true", help="layout lake: the same crates and landscape without any water")
    ap.add_argument("--densities", default="300,600,900,1200,2500", help="layout lake: kg/m3, cycled over the crates")
    ap.add_argument("--bodies", type=int, default=5000)
    ap.add_argument("--characters", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--template", type=Path, default=REPO / "docs/perf-audit/scenes/skyonly_noclouds.hescene")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    tpl = json.loads(a.template.read_text())
    world = next(e for e in tpl["entities"] if e.get("parent") is None)
    entities = [world] + [e for e in tpl["entities"] if "environment" in e.get("components", {})]
    world["children"] = [e["uuid"] for e in entities if e is not world]

    def add(name, comps):
        e = {"children": [], "components": comps, "name": name, "parent": world["uuid"], "uuid": uid(rng)}
        world["children"].append(e["uuid"])
        entities.append(e)

    if a.layout == "sparse":
        side = max(1, math.ceil(math.sqrt(a.bodies)))
        spacing, extent = 3.0, side * 3.0
    elif a.layout == "lake":
        side = max(1, math.ceil(math.sqrt(a.bodies)))
        spacing, extent = 2.0, side * 2.0
        if extent > 76.0:
            sys.exit(f"--bodies {a.bodies}: the lattice is {extent:.0f} m wide, the lake 80 m")
    else:
        layers = 5
        side = max(1, math.ceil(math.sqrt(a.bodies / layers)))
        spacing, extent = 1.2, side * 1.2
    half = extent / 2.0

    densities = [float(v) for v in a.densities.split(",") if v.strip()]
    lake_level = 3.0
    if a.layout == "lake":
        # A flat landscape (seed 0) 96 m across with one-metre water cells; its collider is the bed at y = 0.
        res, half_cells = 96, 40
        terrain = {"sizeX": 96.0, "sizeZ": 96.0, "resolution": 9, "seed": 0}
        if not a.no_lake:
            terrain.update({"waterRes": res, "waterNextId": 2, "waterBodies": [{"id": 1, "level": lake_level}],
                            "waterCellsB64": base64.b64encode(lake_cells(res, half_cells)).decode()})
        add("Landscape", {"transform": tf((0.0, 0.0, 0.0)), "terrain": terrain})
    else:
        # the ground: one big static box (collider half extents are scaled by the transform)
        add("Ground", {"mesh": {"asset": CUBE, "castsShadow": False, "lodBias": 0, "receivesShadow": True, "visible": True},
                       "material": {"asset": NO_ASSET},
                       "transform": tf((0, -1.0, 0), scale=(extent + 60, 2.0, extent + 60)),
                       "collider": {"halfEx": [0.5, 0.5, 0.5], "height": 2.0, "isTrigger": False, "radius": 0.5, "shape": 0},
                       "rigidbody": {"friction": 0.5, "is2D": False, "mass": 0.0, "restitution": 0.95, "type": 0}})

    for i in range(a.bodies):
        if a.layout == "lake":
            x, z = (i % side) * spacing - half + 1.0, (i // side) * spacing - half + 1.0
            d = densities[i % len(densities)]
            add(f"Crate {i}", {
                "mesh": {"asset": CUBE, "castsShadow": False, "lodBias": 0, "receivesShadow": True, "visible": True},
                "material": {"asset": NO_ASSET},
                "transform": tf((x, lake_level + 2.0, z)),   # a metre cube: 1.5 m of air under it
                "collider": {"halfEx": [0.5, 0.5, 0.5], "height": 2.0, "isTrigger": False, "radius": 0.5, "shape": 0},
                "rigidbody": {"friction": 0.5, "is2D": False, "mass": d, "density": d, "restitution": 0.0, "type": 1}})
            continue
        if a.layout == "sparse":
            x, z = (i % side) * spacing - half, (i // side) * spacing - half
            y = 4.0 + rng.random() * 20.0
        else:
            layer, k = i // (side * side), i % (side * side)
            x, z = (k % side) * spacing - half, (k // side) * spacing - half
            y = 2.0 + layer * 1.2 + rng.random() * 0.05
        sphere = i % 2 == 1
        add(f"Body {i}", {
            "mesh": {"asset": SPHERE if sphere else CUBE, "castsShadow": False, "lodBias": 0, "receivesShadow": True, "visible": True},
            "material": {"asset": NO_ASSET},
            "transform": tf((x, y, z)),
            "collider": {"halfEx": [0.5, 0.5, 0.5], "height": 2.0, "isTrigger": False, "radius": 0.5, "shape": 1 if sphere else 0},
            "rigidbody": {"friction": 0.5, "is2D": False, "mass": 1.0, "restitution": 0.9, "type": 1}})

    for c in range(a.characters):
        ang = c * 2.399963   # golden angle: an even spread, deterministic
        r = (0.15 + 0.8 * math.sqrt((c + 0.5) / a.characters)) * half
        add(f"Character {c}", {
            "transform": tf((math.cos(ang) * r, 1.2, math.sin(ang) * r)),
            "collider": {"halfEx": [0.5, 0.5, 0.5], "height": 1.8, "isTrigger": False, "radius": 0.4, "shape": 2},
            "rigidbody": {"friction": 0.5, "is2D": False, "mass": 70.0, "restitution": 0.0, "type": 2},
            "characterController": {"slopeLimit": 50.0, "stepHeight": 0.4, "skinWidth": 0.02, "mass": 70.0,
                                    "gravity": 9.81, "jumpSpeed": 5.0, "layer": 0}})

    cam = (tf((0.0, 0.35 * extent + 6, 0.8 * extent + 12), (-20.0, 0.0, 0.0)) if a.layout == "lake"
           else tf((0.0, 0.45 * extent + 20, 0.6 * extent + 30), (-30.0, 0.0, 0.0)))
    add("Bench camera", {"camera": {"fovDegrees": 60.0, "nearPlane": 0.1, "farPlane": 4000.0, "isMain": True, "orthographic": False},
                         "transform": cam})

    if a.out.exists():
        shutil.rmtree(a.out)
    subprocess.run(["cp", "-cR", str(a.runtime), str(a.out)], check=True)
    (a.out / "Bench.hescene").write_text(json.dumps({"entities": entities, "levelScript": tpl.get("levelScript", {}),
                                                    "version": tpl.get("version", "1.1")}))
    write_hcfg(a.out, "Bench.hescene")
    print(f"{a.out}: {len(entities)} entities ({a.bodies} {a.layout} bodies, {a.characters} characters), extent {extent:.0f} m")


if __name__ == "__main__":
    main()
