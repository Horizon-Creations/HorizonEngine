#!/usr/bin/env python3
"""A loose HorizonGame directory with a physics load scene (Thema 178, step 1). Measurement tool only.

  python3 scripts/perf/physics_load_scene.py --runtime /tmp/he178/gamebase --out /tmp/he178/g_dense5k \
      --layout dense --bodies 5000 [--characters 100] [--seed 1]

--layout sparse  bodies on a 3 m grid, dropped from 4..24 m: many ACTIVE bodies, hardly any contacts
--layout dense   a lattice 1.2 m apart, five layers high, dropped together: contact-rich and, with restitution
                 0.9 on both sides, it keeps moving for the whole capture instead of falling asleep (Jolt
                 sleeps a resting pile after 0.5 s and the engine has no switch to forbid it; a settled pile
                 costs next to nothing, which is the wrong thing to baseline)
--characters N   N standing characters (CharacterController + kinematic proxy body + capsule) spread over the
                 field: they run CharacterVirtual::ExtendedUpdate every step. They do not walk (no script).

The game dir is a copy of --runtime (APFS clone) plus Bench.hescene and a version-2 project.hcfg, run with
  HE_PROFILE_CAPTURE=600 HE_PROFILE_WARMUP=300 HE_PROFILE_COUNTERS=0 HE_HIDDEN_WINDOW=1 <out>/HorizonGame
(see scripts/perf/physics_load_run.sh). Entity count is bodies + characters + 4.
"""
import argparse
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


def tf(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--layout", choices=["sparse", "dense"], default="sparse")
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
    else:
        layers = 5
        side = max(1, math.ceil(math.sqrt(a.bodies / layers)))
        spacing, extent = 1.2, side * 1.2
    half = extent / 2.0

    # the ground: one big static box (collider half extents are scaled by the transform)
    add("Ground", {"mesh": {"asset": CUBE, "castsShadow": False, "lodBias": 0, "receivesShadow": True, "visible": True},
                   "material": {"asset": NO_ASSET},
                   "transform": tf((0, -1.0, 0), scale=(extent + 60, 2.0, extent + 60)),
                   "collider": {"halfEx": [0.5, 0.5, 0.5], "height": 2.0, "isTrigger": False, "radius": 0.5, "shape": 0},
                   "rigidbody": {"friction": 0.5, "is2D": False, "mass": 0.0, "restitution": 0.95, "type": 0}})

    for i in range(a.bodies):
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

    add("Bench camera", {"camera": {"fovDegrees": 60.0, "nearPlane": 0.1, "farPlane": 4000.0, "isMain": True, "orthographic": False},
                         "transform": tf((0.0, 0.45 * extent + 20, 0.6 * extent + 30), (-30.0, 0.0, 0.0))})

    if a.out.exists():
        shutil.rmtree(a.out)
    subprocess.run(["cp", "-cR", str(a.runtime), str(a.out)], check=True)
    (a.out / "Bench.hescene").write_text(json.dumps({"entities": entities, "levelScript": tpl.get("levelScript", {}),
                                                    "version": tpl.get("version", "1.1")}))
    write_hcfg(a.out, "Bench.hescene")
    print(f"{a.out}: {len(entities)} entities ({a.bodies} {a.layout} bodies, {a.characters} characters), extent {extent:.0f} m")


if __name__ == "__main__":
    main()
