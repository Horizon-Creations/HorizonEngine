#!/usr/bin/env python3
"""A loose HorizonGame directory that streams a split reference world (Thema 164, step 5).

  python3 scripts/perf/make_cell_game_dir.py --split /tmp/ws_cellbench/d512_l1100 --out /tmp/ws_game \
      [--runtime out/deploy/Game] [--cam 0,25,90] [--pitch -14]

--split is a HE_CELL_BENCH_DIR of tests/test_world_scale.cpp's 'Cell streaming bench: extract*'
(Bench.hescene = the base with its cell manifest, Bench.cells/ = the cells). The game gets a copy
of the runtime, a project.hcfg (version 2, no pak: the scene is loaded loose, like a WIP run) and
the split with one camera added to the base. Run it with

  HE_CAPTURE_FRAME=400 HE_CAPTURE_PATH=/tmp/a.ppm  /tmp/ws_game/HorizonGame     (a picture)
  HE_PROFILE_CAPTURE=300 HE_PROFILE_WARMUP=300 HE_PROFILE_COUNTERS=0 /tmp/ws_game/HorizonGame  (the profiler dump)

Application::Run implements both, so the game takes them as the editor does.
"""
import argparse
import json
import shutil
import struct
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent


def pstr(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<I", len(b)) + b


def write_hcfg(dest: Path, scene: str):
    # ProjectConfigLoader::save, version 2: magic, version, reserved, three strings, the legacy
    # project uuid, the flags word, the key and the packed-scene uuid. No flag: not encrypted, no
    # packed scene, not an application, shaders as usual.
    buf = b"HCFG" + struct.pack("<HH", 2, 0)
    buf += pstr("BenchGame") + pstr("none.hpak") + pstr(scene)
    buf += bytes(16) + struct.pack("<I", 0) + bytes(32) + bytes(16)
    (dest / "project.hcfg").write_bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--runtime", type=Path, default=REPO / "out" / "deploy" / "Game")
    ap.add_argument("--cam", default="0,25,90")
    ap.add_argument("--pitch", type=float, default=-14.0, help="degrees")
    a = ap.parse_args()

    if a.out.exists():
        shutil.rmtree(a.out)
    # -c: APFS clone, instant and no extra space.
    subprocess.run(["cp", "-cR", str(a.runtime), str(a.out)], check=True)
    shutil.copytree(a.split / "Bench.cells", a.out / "Bench.cells")

    scene = json.loads((a.split / "Bench.hescene").read_text())
    world = next(e for e in scene["entities"] if e.get("parent") is None)
    x, y, z = (float(v) for v in a.cam.split(","))
    cam = {"children": [], "name": "Bench camera", "parent": world["uuid"], "uuid": [0x1234567, 0x7654321],
           "components": {
               "camera": {"fovDegrees": 60.0, "nearPlane": 0.1, "farPlane": 4000.0, "isMain": True,
                          "orthographic": False},
               "transform": {"position": [x, y, z], "rotation": [a.pitch, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}}}
    world.setdefault("children", []).append(cam["uuid"])
    scene["entities"].append(cam)
    (a.out / "Bench.hescene").write_text(json.dumps(scene))
    write_hcfg(a.out, "Bench.hescene")
    print(f"{a.out}: {len(scene['entities'])} base entities, cells {len(list((a.out / 'Bench.cells').iterdir()))}")


if __name__ == "__main__":
    main()
