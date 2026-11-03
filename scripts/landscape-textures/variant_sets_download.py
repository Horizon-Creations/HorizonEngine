#!/usr/bin/env python3
"""Landscape slope/ground variants (2026-11-03): look up and download the Poly Haven sets of
`variant_sets_plan` below, 2k, one folder per set (<root>/sets/<id>/), plus <root>/plan.json for
variant_sets_stage.py.

    python3 -I variant_sets_download.py <root>      # needs nothing but the standard library

Per set: Diffuse .jpg, Normal GL .png (NOT jpg: it wrecks normals), Rough .jpg, AO .jpg, Displacement .jpg.
Poly Haven (polyhaven.com) is CC0; the API wants a User-Agent. The list is the one this step used:
<group> -> <role label without the space> -> Poly Haven ids, variant 1, 2, 3 in that order. Mountains keeps
its existing set as variant 1 (variant_sets_stage.py extracts it from the server arrays).
"""
import concurrent.futures as cf
import json
import pathlib
import sys
import urllib.request

PLAN = {   # variant_sets_plan
 "Desert":    {"Hardpan":   ["sandy_gravel_02","dry_ground_01","dirt_floor"],
               "Sandstone": ["cliff_side","rock_06","dry_riverbed_rock"]},
 "Mountains": {"Grass": ["grass_path_3","grass_ground"],
               "Dirt":  ["brown_mud_dry","park_dirt"],
               "Rock":  ["rock_face_03","rock_05"]},
 "Forest":    {"ForestFloor": ["forest_ground_04","brown_mud_leaves_01","brown_mud_rocks_01"],
               "Roots":       ["forrest_ground_01","forrest_ground_03","forest_leaves_02"],
               "MossyRock":   ["mossy_rock","rock_3","rock_face_03"]},
 "Grassland": {"Meadow":   ["grass_path_2","grass_ground","grass_path_3"],
               "BareSoil": ["brown_mud_03","brown_mud_02","brown_mud_dry"],
               "Stones":   ["rocks_ground_03","pebble_ground_01","rock_ground_02"]},
}

UA = {"User-Agent": "curl/8"}


def api(url):
    return json.load(urllib.request.urlopen(urllib.request.Request(url, headers=UA)))


def main():
    root = pathlib.Path(sys.argv[1]); root.mkdir(parents=True, exist_ok=True)
    json.dump(PLAN, open(root / "plan.json", "w"), indent=1)
    ids = sorted({i for g in PLAN.values() for r in g.values() for i in r})
    jobs, total = [], 0
    for i in ids:
        d = api(f"https://api.polyhaven.com/files/{i}")
        (root / "sets" / i).mkdir(parents=True, exist_ok=True)
        for key, fmt in (("Diffuse", "jpg"), ("nor_gl", "png"), ("Rough", "jpg"), ("AO", "jpg"), ("Displacement", "jpg")):
            try:
                x = d[key]["2k"][fmt]
            except KeyError:
                print(f"{i}: no {key} {fmt}"); continue
            jobs.append((root / "sets" / i / x["url"].split("/")[-1], x["url"], x["size"])); total += x["size"]
    print(len(ids), "sets,", len(jobs), "files,", round(total / 1e6), "MB")

    def get(j):
        p, url, size = j
        if p.exists() and p.stat().st_size == size:
            return p.name, True
        for _ in range(3):
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r, open(p, "wb") as f:
                    f.write(r.read())
                if p.stat().st_size == size:
                    return p.name, True
            except OSError:
                pass
        return p.name, False
    with cf.ThreadPoolExecutor(4) as ex:
        bad = [n for n, ok in ex.map(get, jobs) if not ok]
    print("failed:", bad)


if __name__ == "__main__":
    main()
