#!/usr/bin/env python3
"""Per-group projects and scenes for the procedural snow/sand cost A/B (Thema 328, step 1).

  python3 scripts/perf/snow_sand_setup.py --work /tmp/he328 --ph /tmp/he328/ph

For every Landscape 3.0 group (AutoLandscapeMaterial.h, kAutoLandscapeGroups) it writes
<work>/proj_<G>/<G>.heproj plus a startup scene: the gen_autoland_scene.py landscape (no layer meshes) with that
group's material id. Forest / Grassland / Desert have no real arrays on a dev machine, so their placeholder arrays
(landscape_tex_gen <dir> --group G, 128 px) go in as a project override; Mountains reads the EngineContentCache.
"""
import argparse, json, shutil, subprocess, sys, uuid
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GROUP_MATERIAL = {"Mountains": [0x412, 1], "Forest": [0x415, 1], "Grassland": [0x419, 1], "Desert": [0x4A0, 1]}

ap = argparse.ArgumentParser()
ap.add_argument("--work", required=True)
ap.add_argument("--ph", required=True, help="dir with <G>/T_Landscape_<G>_*_Array.hasset placeholders")
ap.add_argument("--extent", type=float, default=256.0)
a = ap.parse_args()
work, ph = Path(a.work), Path(a.ph)

base = work / "scenes" / "base.hescene"
base.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(REPO / "scripts/perf/gen_autoland_scene.py"), "--extent", str(a.extent),
                "--meshes", "none", "--out", str(base)], check=True)
doc = json.loads(base.read_text())
terrains = [e for e in doc["entities"] if "terrain" in e.get("components", {})]
assert len(terrains) == 1
for g, mat in GROUP_MATERIAL.items():
    terrains[0]["components"]["material"] = {"asset": mat}
    proj = work / f"proj_{g}"
    shutil.rmtree(proj, ignore_errors=True)
    (proj / "Content").mkdir(parents=True)
    (proj / "Config").mkdir()
    (proj / "Content" / "StartupScene.hescene").write_text(json.dumps(doc))
    if g != "Mountains":
        dst = proj / "Content" / "Engine" / "Textures" / "Landscape"
        dst.mkdir(parents=True)
        for f in (ph / g).glob("T_Landscape_*_Array.hasset"):
            shutil.copy(f, dst / f.name)
    (proj / f"{g}.heproj").write_text(json.dumps({
        "name": f"SnowSand{g}", "id": uuid.uuid4().hex, "version": "1.0", "preset": 0,
        "startupScene": "Content/StartupScene.hescene", "scriptLanguage": "None", "appProject": False,
        "advancedShaderEffects": False, "allowFiles": False, "allowProcesses": False, "allowNetwork": False,
        "fontScripts": 0}, indent=2))
    print(proj / f"{g}.heproj")
