#!/usr/bin/env python3
"""A live scene for the Auto Landscape 2.0 audit (Thema 180, Schritt 6).

  python3 scripts/perf/gen_autoland_scene.py --extent 128 --density 1 --out /tmp/al2/s128_x1.hescene
  python3 scripts/perf/gen_autoland_scene.py --extent 128 --meshes none --out /tmp/al2/s128_off.hescene

The template (docs/perf-audit/scenes/landscape_noclouds.hescene) provides the sky and the weather;
its terrain is replaced by a square landscape of --extent metres with rolling analytic relief
(slopes from flat to ~0.3 in 1-N.y on the flanks, so tufts, pebbles and rocks all find ground under their placement
rules), the SHIPPED Auto Landscape material (kAutoLandscapeMaterialId) and one Foliage component whose
layers are the LayerMeshPresets.h presets (Tuft, Pebble, Rock A/B/C), each density times --density.
Snow Drift is left out: the shipped snow line (60 m) lies above this relief.

The presets are copied here by value (LayerMeshPresets.h, applyLayerMeshPreset); a drift between the two
shows as a different instance count in the editor log ('FoliageExtract: ...'), which the audit reads.
--meshes none writes the same terrain without a Foliage component: the control the audit divides by.

The headless dumps never tick the world, so the scatter, the cull and the per-frame extract of the
layers only exist in a live run: this file is what he_perf_capture.py --scene opens.
"""
import argparse
import base64
import json
import math
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TEMPLATE = REPO / "docs" / "perf-audit" / "scenes" / "landscape_noclouds.hescene"

AUTO_LANDSCAPE_MATERIAL = [0x412, 1]          # kAutoLandscapeMaterialId (AutoLandscapeMaterial.h)
# kEngine*MeshId (DefaultAssets.h): level n of a kind is hi + n.
MESH_HI = {"tuft": 0x109, "pebble": 0x10C, "rocka": 0x10E, "rockb": 0x111, "rockc": 0x114}
NO_ASSET = [0, 0]
KINDS = ["tuft", "pebble", "rocka", "rockb", "rockc"]
NAMES = {"tuft": "Grass Tuft", "pebble": "Pebble", "rocka": "Rock A", "rockb": "Rock B", "rockc": "Rock C"}


def mesh(kind, level=0):
    return [MESH_HI[kind] + level, 1]


def preset_layer(kind, density_mul, seed, shadows):
    """applyLayerMeshPreset (LayerMeshPresets.h) as the serializer writes it (foliageLayerToJson)."""
    # FoliageLayer defaults castsShadow / contributesAO to TRUE and the serializer writes them only when
    # they differ, so a preset layer that must not cast (tuft, pebble) has to say so explicitly.
    j = {"mesh": mesh(kind), "material": NO_ASSET, "name": NAMES[kind], "seed": seed, "maskRes": 128,
         "castsShadow": False, "contributesAO": False}
    if kind == "tuft":
        j.update(density=0.30, minScale=0.7, maxScale=1.5, drawDistance=70.0, slopeMax=0.12,
                 lods=[(1, 15.0, 0.7), (2, 35.0, 0.4)])
    elif kind == "pebble":
        j.update(density=0.20, minScale=0.5, maxScale=1.6, drawDistance=45.0, slopeMax=0.25,
                 lods=[(1, 15.0, 0.6)])
    else:
        j.update(density=0.004, minScale=0.6, maxScale=2.4, drawDistance=160.0, slopeMin=0.15,
                 lods=[(1, 30.0, 1.0), (2, 75.0, 0.7)])
        if shadows:
            j.update(castsShadow=True, shadowDistance=60.0, contributesAO=True)
    j["density"] = j["density"] * density_mul
    j["lods"] = [{"mesh": mesh(kind, n), "material": NO_ASSET, "distance": d, "density": dens}
                 for n, d, dens in j["lods"]]
    return j


def relief(x, z):
    """Rolling hills, metres. Flat enough in the troughs for tufts, steep enough on the flanks for rock."""
    return 26.0 * math.sin(x * 0.021) * math.cos(z * 0.017) + 6.0 * math.sin(x * 0.07 + z * 0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extent", type=float, default=128.0, help="side of the landscape, metres")
    ap.add_argument("--density", type=float, default=1.0, help="multiplier on every preset density")
    ap.add_argument("--meshes", choices=["presets", "none"], default="presets")
    ap.add_argument("--no-shadows", action="store_true", help="rocks do not cast / count for AO (the preset default is on)")
    ap.add_argument("--template", default=str(TEMPLATE))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    tpl = json.loads(Path(a.template).read_text())
    terrains = [e for e in tpl["entities"] if "terrain" in e.get("components", {})]
    if len(terrains) != 1:
        raise SystemExit("the template needs exactly one terrain entity")
    t = terrains[0]
    tc = t["components"]["terrain"]

    # One vertex per metre at 128 m (what the witness uses); at larger extents one per 2^k metres
    # keeps 2^n + 1 (no resample) and the chunk count in a range a real landscape has.
    res = int(a.extent) + 1 if a.extent <= 256 else int(a.extent) // 2 + 1
    step = a.extent / (res - 1)
    heights = bytearray()
    for zi in range(res):
        for xi in range(res):
            heights += struct.pack("<f", relief(-a.extent / 2 + xi * step, -a.extent / 2 + zi * step))
    tc.update(sizeX=a.extent, sizeZ=a.extent, resolution=res, heightScale=0.0, seed=0)
    tc["sculptHeightsB64"] = base64.b64encode(bytes(heights)).decode()
    t["components"]["material"] = {"asset": AUTO_LANDSCAPE_MATERIAL}

    t["components"].pop("foliage", None)
    n_layers = 0
    if a.meshes == "presets":
        layers = [preset_layer(k, a.density, 1000 + 17 * i, not a.no_shadows) for i, k in enumerate(KINDS)]
        t["components"]["foliage"] = {"visible": True, "layers": layers}
        n_layers = len(layers)

    out = {"entities": tpl["entities"], "levelScript": tpl.get("levelScript", {}), "version": tpl.get("version", "1.1")}
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(out))
    print(f"wrote {a.out}: extent {a.extent:g} m, terrain {res}x{res}, {n_layers} layer(s), density x{a.density:g}")


if __name__ == "__main__":
    main()
