"""A loose HorizonGame dir around an editor scene (adds a main camera). Measurement tool only (Thema 238).

  python3 scripts/perf/make_land_game_dir.py <scene.hescene> <runtime Game dir> <out dir>
"""
import json, sys, shutil, random
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_cell_game_dir import write_hcfg
scene, runtime, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
d = json.load(open(scene))
world = next(e for e in d["entities"] if e.get("parent") is None)
rng = random.Random(7)
cam = {"children": [], "name": "Cam", "parent": world["uuid"], "uuid": [rng.getrandbits(64) | 1, rng.getrandbits(64) | 1],
       "components": {"camera": {"fovDegrees": 60.0, "nearPlane": 0.1, "farPlane": 4000.0, "isMain": True, "orthographic": False},
                      "transform": {"position": [0.0, 15.0, 0.0], "rotation": [-10.0, 0.0, 0.0], "scale": [1, 1, 1]}}}
world["children"].append(cam["uuid"]); d["entities"].append(cam)
if out.exists(): shutil.rmtree(out)
shutil.copytree(runtime, out)   # cp -cR is macOS-only (BSD clone flag); this works on every platform
(out / "Bench.hescene").write_text(json.dumps(d)); write_hcfg(out, "Bench.hescene")
print(out)
