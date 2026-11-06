#!/usr/bin/env python3
"""Stage the sets variant_sets_download.py fetched for `landscape_tex_gen --pack-group`.

    python3 -I variant_sets_stage.py <download-root> <staging-root> <EngineContentCache/Textures/Landscape> <hasset_tex.py>

<staging-root>/<Group>/<RoleLabel>_<Map>[_v2|_v3].<jpg|png>: every set copied under the name --pack-group reads
(the files stay as downloaded; the importer flips on load). Mountains' variant 1 is not downloaded: it is cut out
of the CURRENT server arrays (RGBA8 T_Landscape_{Albedo,Normal,Mask}_Array in the cache dir, slice = role,
Mask split into AO / Roughness / Height), so repacking keeps what is on the server. Needs Pillow; run with -I.

Then, per group:  landscape_tex_gen <out> --pack-group <staging>/<Group> --group <Group> --size 2048
                  landscape_tex_gen <out> --mix --from <out>
"""
import importlib.util
import json
import pathlib
import shutil
import sys

from PIL import Image
dl, out, cache, ht = map(pathlib.Path, sys.argv[1:5])
spec = importlib.util.spec_from_file_location("hasset_tex", ht); H = importlib.util.module_from_spec(spec); spec.loader.exec_module(H)
plan = json.load(open(dl/"plan.json"))
MAPNAME = {"diff":"Albedo","diffuse":"Albedo","nor_gl":"Normal","rough":"Roughness","ao":"AO","disp":"Height"}
def set_files(sid):
    res = {}
    for p in (dl/"sets"/sid).iterdir():
        stem = p.stem.removesuffix("_2k"); tok = stem[len(sid)+1:]
        if tok in MAPNAME: res[MAPNAME[tok]] = p
    return res
for group, roles in plan.items():
    g = out/group; g.mkdir(parents=True, exist_ok=True)
    existing = group == "Mountains"
    for role, ids in roles.items():
        sets = ([None] if existing else []) + ids          # None = the existing v1 (Mountains)
        for n, sid in enumerate(sets, start=1):
            suffix = "" if n == 1 else f"_v{n}"
            if sid is None: continue
            for m, p in set_files(sid).items():
                shutil.copyfile(p, g/f"{role}_{m}{suffix}{p.suffix}")
# existing Mountains arrays (RGBA8, V=1, slice = role) -> v1 PNGs; Mask = R AO, G roughness, B height
alb = H.read_hasset(cache/"T_Landscape_Albedo_Array.hasset")
nrm = H.read_hasset(cache/"T_Landscape_Normal_Array.hasset")
msk = H.read_hasset(cache/"T_Landscape_Mask_Array.hasset")
g = out/"Mountains"
for s, role in enumerate(["Grass","Dirt","Rock","Snow"]):
    H.view(alb, s, 0).convert("RGB").save(g/f"{role}_Albedo.png")
    H.view(nrm, s, 0).convert("RGB").save(g/f"{role}_Normal.png")
    mk = H.view(msk, s, 0)
    for name, i in (("AO",0),("Roughness",1),("Height",2)): mk.getchannel(i).save(g/f"{role}_{name}.png")
for gp in sorted(out.iterdir()): print(gp.name, len(list(gp.iterdir())), "files")
