#!/usr/bin/env python3
"""Scene variants of the Poly Haven foliage test scene (Thema 331, step 1).

    foliage_variants.py <StartupScene.hescene> <out-dir>

Reads the generated scene (12 single-layer `Foliage <name>` entities) and writes one
variant per quick mitigation, each ALONE on top of the unchanged scene, plus the
combination. Nothing else in the scene changes, so a pass-time difference between two
variants is the mitigation. The scene is never edited in place (the generator script of
the test project overwrites it).

Mitigations (the serializer writes only deviations from the defaults, so a `false`
for castsShadow/contributesAO has to be written explicitly, see SceneSerializer.cpp:193):
  shadowao   small plants: castsShadow=false, contributesAO=false
  shadowdist every layer: shadowDistance=SHADOW_DIST (0 = drawDistance)
  lods       hand-made FoliageLod chain per size class: the layer's own mesh thinned
             out, then the engine cross card (kEngineCrossCardMeshId = {0x108, 1})
  drawdist   drawDistance per size class
  all        shadowao (small) + shadowdist (medium/large) + lods + drawdist
  nofoliage  floor: every layer hidden (layerVisible=false), terrain/sky/clouds only

Size classes come from the largest bounding-box side of the glTF mesh (not measured
in the engine, node scales ignored): small < 0.7 m, medium < 1.5 m, large above.
"""
import json
import sys
from pathlib import Path

CROSS_CARD = [0x108, 1]          # HE::kEngineCrossCardMeshId (DefaultAssets.h:120)

SIZE = {                          # largest bbox side of the glTF, metres
    "celandine_01": 0.31, "dandelion_01": 0.26, "grass_medium_01": 0.34,
    "nettle_plant": 0.22, "periwinkle_plant": 0.41, "shrub_03": 0.40,
    "shrub_04": 0.58, "weed_plant_02": 0.29,
    "anthurium_botany_01": 1.14, "fern_02": 0.99,
    "flower_gazania": 2.11, "shrub_02": 2.34,
}


def size_class(name):
    s = SIZE[name]
    return "small" if s < 0.7 else ("medium" if s < 1.5 else "large")


SHADOW_DIST = 15.0
# drawDistance per class (small plants vanish first); large keep the generator's value.
DRAW = {"small": 25.0, "medium": 40.0}
# LOD chain per class: (distance, own mesh or cross card, share of plants kept).
LODS = {
    "small":  [(8.0, None, 0.5), (16.0, CROSS_CARD, 0.5)],
    "medium": [(15.0, None, 0.7), (28.0, CROSS_CARD, 0.7)],
    "large":  [(30.0, CROSS_CARD, 1.0)],
}


def lod_json(dist, mesh, dens):
    return {"mesh": mesh if mesh else [0, 0], "material": [0, 0],
            "distance": dist, "density": dens}


def apply(fol, name, measures):
    cls = size_class(name)
    if "hide" in measures:
        fol["layerVisible"] = False       # flat form of FoliageLayer::visible (SceneSerializer.cpp)
        return
    if "shadowao" in measures and cls == "small":
        fol["castsShadow"] = False
        fol["contributesAO"] = False
    if "shadowdist" in measures and not ("shadowao" in measures and cls == "small"):
        fol["shadowDistance"] = SHADOW_DIST
    if "drawdist" in measures and cls in DRAW:
        fol["drawDistance"] = DRAW[cls]
    if "lods" in measures:
        fol["lods"] = [lod_json(*l) for l in LODS[cls]]


VARIANTS = {
    "v0_base": [],
    "v1_shadowao": ["shadowao"],
    "v2_shadowdist": ["shadowdist"],
    "v3_lods": ["lods"],
    "v4_drawdist": ["drawdist"],
    "v5_all": ["shadowao", "shadowdist", "lods", "drawdist"],
    "v6_nofoliage": ["hide"],
}


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for vname, measures in VARIANTS.items():
        scene = json.loads(src.read_text())
        n = 0
        for e in scene["entities"]:
            fol = e.get("components", {}).get("foliage")
            if not fol:
                continue
            apply(fol, fol["name"], measures)
            n += 1
        assert n == 12, n
        (out / f"{vname}.hescene").write_text(json.dumps(scene, indent=1))
        print(vname, measures)


if __name__ == "__main__":
    main()
