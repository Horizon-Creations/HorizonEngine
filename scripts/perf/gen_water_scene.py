#!/usr/bin/env python3
"""A live scene with one lake for the water close-up audit (Thema 235, Schritt 1).

  python3 scripts/perf/gen_water_scene.py --out /tmp/he235/scenes/lake.hescene
  python3 scripts/perf/gen_water_scene.py --no-water --out /tmp/he235/scenes/lake_nowater.hescene

The template (docs/perf-audit/scenes/landscape_noclouds.hescene) provides the sky and the weather;
its terrain is replaced by a square landscape of --extent metres (default 512) with a round basin in the
middle: the bed falls from the level-0 shoreline at --radius to --depth below at the centre, outside the
shore the ground rises gently with a little rolling relief. One water body (id 1, level 0) covers every
cell inside the shoreline plus a margin; the surface mesh is clipped to the ground by the engine
(WaterSurface), so the shore is the real, soft one of Thema 206 step 2.
The terrain keeps the SHIPPED Auto Landscape material, like the player's landscapes (--plain drops it:
see the option for why that matters to the water).

--no-water writes the same terrain without the body: the control the audit divides by.
The headless dumps never tick the world, so WaterSurface::update (mesh build, ripples) only exists in a
live run: this file is what he_perf_capture.py --scene opens.

Terrain-local coordinates are centred (x in -extent/2..extent/2), cells are row-major, run-length
encoded as (varint count, owner lo, owner hi, coverage) -- WaterField.cpp encodeCells.
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


def bed(x, z, radius, depth):
    r = math.hypot(x, z)
    if r < radius:
        t = r / radius
        return -depth * (1.0 - t * t)                       # parabolic bowl, level 0 at the rim
    return 0.045 * (r - radius) + 1.2 * math.sin(x * 0.05) * math.cos(z * 0.043)   # gently rising shore


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def encode_cells(res, extent, radius, margin):
    cell = extent / res
    runs, cur, n = bytearray(), None, 0
    for zi in range(res):
        for xi in range(res):
            x = -extent / 2 + (xi + 0.5) * cell
            z = -extent / 2 + (zi + 0.5) * cell
            v = (1, 255) if math.hypot(x, z) < radius + margin else (0, 0)
            if v == cur:
                n += 1
                continue
            if cur is not None:
                runs += varint(n) + struct.pack("<HB", cur[0], cur[1])
            cur, n = v, 1
    runs += varint(n) + struct.pack("<HB", cur[0], cur[1])
    return bytes(runs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extent", type=float, default=512.0, help="side of the landscape, metres")
    ap.add_argument("--radius", type=float, default=120.0, help="shoreline radius of the basin, metres")
    ap.add_argument("--depth", type=float, default=6.0, help="depth at the centre, metres")
    ap.add_argument("--water-res", type=int, default=256)
    ap.add_argument("--no-water", action="store_true")
    ap.add_argument("--plain", action="store_true",
                    help="no Auto Landscape material on the terrain: the EngineContentCache textures are not loaded "
                         "(Thema 230: a loaded T_Landscape_Grass_Albedo shares UUID {0x400,1} with the water material "
                         "and makes the water draw as the white fallback)")
    ap.add_argument("--template", default=str(TEMPLATE))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    tpl = json.loads(Path(a.template).read_text())
    terrains = [e for e in tpl["entities"] if "terrain" in e.get("components", {})]
    if len(terrains) != 1:
        raise SystemExit("the template needs exactly one terrain entity")
    t = terrains[0]
    tc = t["components"]["terrain"]

    res = int(a.extent) // 2 + 1                  # one vertex per 2 m, 2^n + 1 for 512
    step = a.extent / (res - 1)
    heights = bytearray()
    for zi in range(res):
        for xi in range(res):
            heights += struct.pack("<f", bed(-a.extent / 2 + xi * step, -a.extent / 2 + zi * step, a.radius, a.depth))
    tc.update(sizeX=a.extent, sizeZ=a.extent, resolution=res, heightScale=0.0, seed=0)
    tc["sculptHeightsB64"] = base64.b64encode(bytes(heights)).decode()
    if a.plain:
        t["components"].pop("material", None)
    else:
        t["components"]["material"] = {"asset": AUTO_LANDSCAPE_MATERIAL}
    for k in ("waterBodies", "waterCellsB64", "waterRes", "waterNextId"):
        tc.pop(k, None)
    if not a.no_water:
        tc["waterRes"] = a.water_res
        tc["waterNextId"] = 2
        tc["waterBodies"] = [{"id": 1, "level": 0.0}]
        tc["waterCellsB64"] = base64.b64encode(encode_cells(a.water_res, a.extent, a.radius, 6.0)).decode()

    out = {"entities": tpl["entities"], "levelScript": tpl.get("levelScript", {}), "version": tpl.get("version", "1.1")}
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(out))
    print(f"wrote {a.out}: extent {a.extent:g} m, terrain {res}x{res}, "
          f"{'no water' if a.no_water else f'lake radius {a.radius:g} m depth {a.depth:g} m'}")


if __name__ == "__main__":
    main()
