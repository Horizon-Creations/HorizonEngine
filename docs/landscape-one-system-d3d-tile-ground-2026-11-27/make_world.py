"""HEProject's StartupScene -> a scene with only its Terrain (stretched to SIZE m, far_testworld's mountain/F/rock/snow), Sky, Weather.
usage: make_world.py in.hescene out.hescene SIZE"""
import json, runpy, sys

src, dst, size = sys.argv[1], sys.argv[2], float(sys.argv[3])
d = json.load(open(src))
keep = [e for e in d["entities"] if "components" in e and ({"terrain", "environment", "weather"} & set(e["components"]))]
keep.sort(key=lambda e: "terrain" not in e["components"])   # terrain first: far_testworld reads entities[0]
tc = keep[0]["components"]["terrain"]
for k in ("waterBodies", "layerWeights2B64", "layerWeights3B64"):
    tc.pop(k, None)
keep[0]["components"].pop("foliage", None)
d["entities"] = keep
tmp = dst + ".tmp.json"
json.dump(d, open(tmp, "w"))
# far_testworld.py with its 40 km scaled to SIZE (its feature positions are in metres of a 40 km world)
code = open(sys.argv[4]).read()
k = size / 40000.0
code = code.replace("SIZE, RES, WRES = 40000.0, 1025, 512", f"SIZE, RES, WRES = {size}, 1025, 512")
code = code.replace("def height(x, z):", f"def height(x, z):\n    x, z = x / {k}, z / {k}")
code = code.replace("def in_f(x, z):", f"def in_f(x, z):\n    x, z = x / {k}, z / {k}")
code = code.replace("r = math.hypot(x - 30000, z - 10000)", f"r = math.hypot(x / {k} - 30000, z / {k} - 10000)")
sys.argv = ["far_testworld.py", tmp, dst]
exec(compile(code, "far_testworld.py", "exec"), {"__name__": "__main__"})
