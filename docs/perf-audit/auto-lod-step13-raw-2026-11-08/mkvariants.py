#!/usr/bin/env python3
"""Variants of the Chain+Impostor scene for the pass breakdown (Thema 331 step 13).
usage: mkvariants.py <impostor.hescene> <outdir>"""
import copy, json, sys
src, out = sys.argv[1], sys.argv[2]
base = json.load(open(src))

def variant(name, hide_layers=False, env=None, shadows_off=False):
    d = copy.deepcopy(base)
    for e in d["entities"]:
        c = e.get("components", {})
        if hide_layers and "foliage" in c:
            c["foliage"]["layerVisible"] = False      # flat form of FoliageLayer::visible
        if shadows_off and "foliage" in c:
            c["foliage"]["castsShadow"] = False
            c["foliage"]["contributesAO"] = False
        if env and "environment" in c:
            c["environment"].update(env)
    json.dump(d, open(f"{out}/{name}.hescene", "w"))

NOCLOUD = {"cloudCoverage": 0.0, "cirrusAmount": 0.0, "cloudShadows": False}
variant("imp")                                        # Chain + Impostor as measured in steps 7/9
variant("imp_nocloud", env=NOCLOUD)                   # same, no clouds
variant("imp_dome", env={"cloudMode": 0})             # same, dome clouds instead of 3D
variant("ground")                                     # ground + sky + clouds, no plants
variants_ground = dict(hide_layers=True)
variant("ground", **variants_ground)
variant("ground_nocloud", hide_layers=True, env=NOCLOUD)
variant("imp_noshadow", shadows_off=True)             # plants do not cast shadow / AO
print("ok")
