#!/usr/bin/env python3
"""Generate a reference world for the world-streaming baseline (Thema 153).

Writes a .hescene with COUNT mesh entities spread over a square of EXTENT
metres (centred on the origin), optionally offset far from the origin, so
load time, entity count and float precision can be measured with the same
scene before and after the streaming rework.

  python3 scripts/perf/gen_reference_world.py --count 10000 --extent 8000 \
      --template docs/perf-audit/scenes/landscape_noclouds.hescene \
      --out /tmp/ws/ref_10k.hescene [--offset 100000,0,0] [--lights 64] \
      [--physics 0.1] [--groups 100] [--seed 1]

The template supplies the environment / weather / terrain entities and the
World root; everything else in it is dropped. Mesh entities use the built-in
cube and sphere (asset ids [1,1] and [257,1]) so no project content is needed.
--groups N parents the meshes under N group entities instead of directly
under World (hierarchy depth 2, the shape a hand-built level has).
The output is deterministic for a given seed.

--foliage N (Thema 163) adds a foliage layer of N instances of a built-in mesh
to the template's terrain: no entity per plant, one FoliageComponent on the
terrain, which scatters them when the scene loads. The terrain is made a flat
square of --foliage-extent metres (default 400) and the density is derived from
N and its area, so the layer holds N instances (the scene log and the editor's
HE_DUMP_FOLIAGETEST witness print the count really placed). --foliage-distance
is the layer's drawDistance: the default 1e6 puts every instance in range, a
small value is the "only a part is in range" case. --count 0 gives a scene with
foliage alone.

  python3 scripts/perf/gen_reference_world.py --count 0 --foliage 100000 \
      --template docs/perf-audit/scenes/landscape_noclouds.hescene \
      --out /tmp/fol/fol_100k.hescene [--foliage-extent 400] [--foliage-distance 1e6] \
      [--foliage-mesh cube|sphere] [--foliage-scale 0.5,1.0]
"""
import argparse
import json
import math
import random
from pathlib import Path

BUILTIN_CUBE = [1, 1]
BUILTIN_SPHERE = [257, 1]
NO_ASSET = [0, 0]
# SceneSerializer's default for a FoliageComponent mask; unused here (no mask is
# written, the layer is uniform) but the serializer reads the key.
FOLIAGE_MASK_RES = 128


def make_uuid(rng):
    # The scene stores a 128-bit UUID as two uint64; 0 is reserved for "none".
    return [rng.getrandbits(64) | 1, rng.getrandbits(64) | 1]


def transform(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=0, help="mesh entities")
    ap.add_argument("--extent", type=float, default=2000.0, help="square side in metres")
    ap.add_argument("--offset", default="0,0,0", help="x,y,z added to every mesh position")
    ap.add_argument("--lights", type=int, default=0, help="point lights spread over the area")
    ap.add_argument("--physics", type=float, default=0.0,
                    help="fraction of meshes that get a static box collider + rigidbody")
    ap.add_argument("--groups", type=int, default=0, help="parent meshes under N group entities")
    ap.add_argument("--foliage", type=int, default=0,
                    help="instances in one foliage layer on the template's terrain (no entities)")
    ap.add_argument("--foliage-extent", type=float, default=400.0,
                    help="side of the (flat) terrain the layer grows on, metres")
    ap.add_argument("--foliage-distance", type=float, default=1.0e6,
                    help="the layer's drawDistance; the default keeps every instance in range")
    ap.add_argument("--foliage-mesh", choices=["cube", "sphere"], default="cube")
    ap.add_argument("--foliage-scale", default="0.5,1.0", help="min,max instance scale")
    ap.add_argument("--foliage-seed", type=int, default=42)
    ap.add_argument("--template", required=True, help="scene providing World root + environment")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    rng = random.Random(a.seed)
    off = [float(v) for v in a.offset.split(",")]
    tpl = json.loads(Path(a.template).read_text())

    roots = [e for e in tpl["entities"] if e.get("parent") is None]
    if len(roots) != 1:
        raise SystemExit(f"template needs exactly one root entity, has {len(roots)}")
    world = roots[0]
    kept = [e for e in tpl["entities"] if e.get("parent") is None
            or any(k in e.get("components", {}) for k in ("environment", "weather", "terrain"))]
    world["children"] = [e["uuid"] for e in kept if e is not world]
    entities = kept

    parents = [world]
    for g in range(a.groups):
        grp = {"children": [], "components": {"transform": transform((0.0, 0.0, 0.0))},
               "name": f"Group {g}", "parent": world["uuid"], "uuid": make_uuid(rng)}
        world["children"].append(grp["uuid"])
        entities.append(grp)
        parents.append(grp)
    mesh_parents = parents[1:] or parents

    # Jittered grid instead of pure random: an even density makes "objects in
    # view" comparable between runs with different counts.
    side = max(1, math.ceil(math.sqrt(a.count)))
    cell = a.extent / side
    half = a.extent / 2.0
    n_phys = int(round(a.count * a.physics))
    for i in range(a.count):
        gx, gz = i % side, i // side
        x = -half + (gx + rng.uniform(0.15, 0.85)) * cell + off[0]
        z = -half + (gz + rng.uniform(0.15, 0.85)) * cell + off[2]
        s = rng.uniform(0.5, 3.0)
        y = s * 0.5 + off[1]
        parent = mesh_parents[i % len(mesh_parents)]
        comps = {
            "material": {"asset": NO_ASSET},
            "mesh": {"asset": BUILTIN_CUBE if i % 4 else BUILTIN_SPHERE, "castsShadow": True,
                     "lodBias": 0, "receivesShadow": True, "visible": True},
            "transform": transform((x, y, z), (0.0, rng.uniform(0.0, 360.0), 0.0), (s, s, s)),
        }
        if i < n_phys:
            comps["collider"] = {"halfEx": [0.5, 0.5, 0.5], "height": 2.0, "isTrigger": False,
                                 "radius": 0.5, "shape": 0}
            comps["rigidbody"] = {"friction": 0.5, "is2D": False, "mass": 0.0,
                                  "restitution": 0.3, "type": 0}  # RigidBodyType::Static
        e = {"children": [], "components": comps, "name": f"Prop {i}",
             "parent": parent["uuid"], "uuid": make_uuid(rng)}
        parent["children"].append(e["uuid"])
        entities.append(e)

    n_foliage = 0
    if a.foliage > 0:
        terrains = [e for e in entities if "terrain" in e.get("components", {})]
        if not terrains:
            raise SystemExit("--foliage needs a terrain entity in the template")
        t = terrains[0]
        tc = t["components"]["terrain"]
        # Flat on purpose: the instance height is then 0 for every seed and the
        # picture does not depend on the template's noise. The terrain chunks
        # are the same however many instances grow on them.
        tc["sizeX"] = tc["sizeZ"] = a.foliage_extent
        tc["heightScale"] = 0.0
        tc.pop("sculptHeightsB64", None)
        smin, _, smax = a.foliage_scale.partition(",")
        smin = float(smin)
        smax = float(smax) if smax else smin
        # +0.5: the scatter takes int(area * density), and float rounding must
        # not cost the last instance.
        t["components"]["foliage"] = {
            "visible": True,
            "mesh": BUILTIN_SPHERE if a.foliage_mesh == "sphere" else BUILTIN_CUBE,
            "material": NO_ASSET,
            "density": (a.foliage + 0.5) / (a.foliage_extent * a.foliage_extent),
            "seed": a.foliage_seed,
            "minScale": smin,
            "maxScale": smax,
            "drawDistance": a.foliage_distance,
            "maskRes": FOLIAGE_MASK_RES,
        }
        n_foliage = a.foliage

    for i in range(a.lights):
        x = rng.uniform(-half, half) + off[0]
        z = rng.uniform(-half, half) + off[2]
        e = {"children": [], "components": {
                "light": {"castsShadow": False, "color": [1.0, 0.85, 0.7], "cullDistance": 0.0,
                          "intensity": 4.0, "range": 12.0, "spotAngle": 30.0, "type": 1,
                          "visible": True},
                "transform": transform((x, 3.0 + off[1], z))},
             "name": f"Light {i}", "parent": world["uuid"], "uuid": make_uuid(rng)}
        world["children"].append(e["uuid"])
        entities.append(e)

    out = {"entities": entities, "levelScript": tpl.get("levelScript", {}),
           "version": tpl.get("version", "1.1")}
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(out))
    print(f"wrote {a.out}: {len(entities)} entities ({a.count} meshes, {a.lights} lights, "
          f"{n_phys} physics, {a.groups} groups, {n_foliage} foliage instances), "
          f"extent {a.extent} m, offset {off}")


if __name__ == "__main__":
    main()
