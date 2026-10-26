#!/usr/bin/env python3
"""Water close-up audit matrix (Thema 235, Schritt 1): lake scene, camera distance and toggles, ABAB.

  python3 scripts/perf/water_matrix.py base   [--reps 3] [--detailed]
  python3 scripts/perf/water_matrix.py <group> --only cell1,cell2

A cell is (scene, camera, CustomConfig sets, env, Water.hasset variant). Every run goes through
scripts/he_perf_capture.py (the real editor loop, Metal, vsync off) on this tree's deploy, so the
numbers are the editor's. The method is scripts/perf/autoland_matrix.py's: repetitions alternate their
order (forward, backward, forward), a run that overlapped a foreign compile is discarded and repeated,
and every run leaves <label>.summary.json, <label>.scopes.json and <label>.conditions.txt.

A "Water.hasset variant" is a file in --variants (default /tmp/he235/variants/<name>.hasset, written by
a temporary local patch of mat_gen, never committed): it is copied over the deploy's
EngineContent/Materials/Water.hasset before the run and the original is put back at the end.
The driver prints the md5 of the editor binary and of the water asset per run into the conditions file.

The scenes come from scripts/perf/gen_water_scene.py (generated here when missing).
"""
import argparse
import os
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import autoland_matrix as am   # conditions(), fmt(), wait_quiet(), scope_sidecar()

REPO = Path(__file__).resolve().parent.parent.parent
CAPTURE = REPO / "scripts" / "he_perf_capture.py"
GEN = REPO / "scripts" / "perf" / "gen_water_scene.py"
EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"
WATER = EDITOR.parent / "EngineContent" / "Materials" / "Water.hasset"
# HE235_ROOT moves the work directory, HE235_PROJ names the project (the Test project of step 1 is gone).
ROOT = Path(os.environ.get("HE235_ROOT") or "/tmp/he235")
PROJ = os.environ.get("HE235_PROJ") or str(ROOT / "proj" / "Test" / "Test.heproj")
SCENES = ROOT / "scenes"
VARIANTS = ROOT / "variants"
WATER_ORIG = ROOT / "Water.hasset.orig"   # the shipped asset, copied before the first run

# x,y,z,yaw,pitch; yaw 0 looks along -Z (EditorCamera::forward), so yaw pi looks across the lake along +Z; the lake is a basin of radius 120 m around the origin, level 0.
CAMS = {
    "far":    "0,10,-250,3.14159,-0.05",     # 130 m to the near shore, 370 m to the far one
    "mid":    "0,10,-150,3.14159,-0.08",     # 30 m to the near shore
    "near3":  "0,3,-60,3.14159,-0.15",       # 3 m over the water, the lake fills the lower two thirds
    "near1":  "0,1.2,-60,3.14159,-0.08",     # 1.2 m over the water, grazing
    "down3":  "0,3,-60,3.14159,-0.9",        # 3 m over the water, looking steeply down: water on every pixel
    "under":  "0,-1.5,-60,3.14159,0.05",     # 1.5 m under the surface
}
# name -> dict(scene, cam, sets [K=V], env [K=V], variant | None)
CELLS = {
    "base": {
        "far-nowater":   dict(scene="lake_nowater", cam="far"),
        "far-water":     dict(scene="lake", cam="far"),
        "mid-nowater":   dict(scene="lake_nowater", cam="mid"),
        "mid-water":     dict(scene="lake", cam="mid"),
        "near3-nowater": dict(scene="lake_nowater", cam="near3"),
        "near3-water":   dict(scene="lake", cam="near3"),
        "near1-water":   dict(scene="lake", cam="near1"),
        "down3-nowater": dict(scene="lake_nowater", cam="down3"),
        "down3-water":   dict(scene="lake", cam="down3"),
        "under-water":   dict(scene="lake", cam="under"),
    },
}

# What the player's exported game runs (Catania config.json): deferred path, GI diffuse on, occlusion culling on.
PLAYER = ["RenderPath=1", "GlobalIlluminationEnabled=true", "OcclusionCulling=true"]
CELLS["player"] = {f"{cam}-{w}": dict(scene="lake" if w == "water" else "lake_nowater", cam=cam, sets=PLAYER)
                   for cam in ("far", "near3", "down3") for w in ("nowater", "water")}
CELLS["player-refl"] = {f"{cam}-{w}": dict(scene="lake" if w == "water" else "lake_nowater", cam=cam,
                                           sets=PLAYER + ["GIReflectionsEnabled=true"])
                        for cam in ("far", "near3", "down3") for w in ("nowater", "water")}
# Is the water material live? Same view with the asset present and with the asset deleted (Thema 230).
CELLS["live"] = {f"{sc}-{cam}-{v}": dict(scene=f"{sc}", cam=cam, variant=None if v == "asset" else "noasset")
                 for sc in ("lake", "plain_lake") for cam in ("near3", "down3") for v in ("asset", "noasset")}
CELLS["live"].update({f"plain_nowater-{cam}": dict(scene="plain_nowater", cam=cam) for cam in ("near3", "down3")})
# Which part of the water graph costs what: Water.hasset variants from a temporary mat_gen patch (HE_MG_VARIANT=...),
# near3 over a plain terrain, empty HOME (--home --plain). "shipped" is the committed asset, "regen" what mat_gen writes
# unpatched (a file in the variants dir), "noasset" the file deleted (fallback material) (the two differ by 74 bytes at the time of writing), "nowater" the scene without the body.
CELLS["variants"] = {"nowater": dict(scene="lake_nowater", cam="near3"), "shipped": dict(scene="lake", cam="near3")}
CELLS["variants"].update({v: dict(scene="lake", cam="near3", variant=v)
                          for v in ("regen", "noasset", "nochop", "nofine", "nofiner", "nocaus", "nofoam", "noshore", "nounder", "trivial")})
# Second set: what is left when the six wave trains go (nowaves), the lighting goes (unlit), or everything but the
# trains and the base colour goes (lean = nowaves + nochop + nocaus + nofoam + nounder; waves-only = the trains alone).
CELLS["variants2"] = {"nowater": dict(scene="lake_nowater", cam="near3"), "shipped": dict(scene="lake", cam="near3")}
CELLS["variants2"].update({v: dict(scene="lake", cam="near3", variant=v)
                           for v in ("nowaves", "waves-only", "lean", "unlit", "trivial", "nochop")})
# A fix against the shipped asset, same binary, ABAB (Thema 235 step 2): "fix" is <root>/variants/fix.hasset,
# what mat_gen writes with the change; "shipped" the asset the deploy had before the first run.
CELLS["fix"] = {f"{cam}-{v}": dict(scene="lake", cam=cam, variant=None if v == "shipped" else "fix")
                for cam in ("down3", "near3", "under", "mid") for v in ("shipped", "fix")}
CELLS["settings"] = {   # one knob at a time, near3 with water (the control is base-near3-water)
    "deferred":   dict(scene="lake", cam="near3", sets=["RenderPath=1"]),
    "gi":         dict(scene="lake", cam="near3", sets=["GlobalIlluminationEnabled=true"]),
    "gi-deferred": dict(scene="lake", cam="near3", sets=["GlobalIlluminationEnabled=true", "RenderPath=1"]),
    "ssr-deferred": dict(scene="lake", cam="near3", sets=["RenderPath=1", "SSREnabled=true"]),
    "taa":        dict(scene="lake", cam="near3", sets=["AntiAliasing=3"]),
    "ssao-off":   dict(scene="lake", cam="near3", sets=["SSAOEnabled=false"]),
}


def md5(p):
    return hashlib.md5(Path(p).read_bytes()).hexdigest()[:12] if Path(p).exists() else "missing"


def ensure_scene(name):
    path = SCENES / f"{name}.hescene"
    if not path.exists():
        cmd = [sys.executable, "-I", str(GEN), "--out", str(path)] + (["--no-water"] if name.endswith("nowater") else []) \
              + (["--plain"] if name.startswith("plain") else [])
        print(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip())
    return path


def one_run(label, cell, out, detailed, warmup, frames, scale, caffeinate=False, retries=2):
    cond = out / f"{label}.conditions.txt"
    variant = cell.get("variant")
    for attempt in range(retries + 1):
        if not am.wait_quiet():
            print(f"  [{label}] machine never went quiet, running anyway (flagged)", flush=True)
        if variant == "noasset":                     # the asset is gone: the water can only draw as the fallback
            WATER.unlink(missing_ok=True)
        else:
            shutil.copyfile(VARIANTS / f"{variant}.hasset" if variant else WATER_ORIG, WATER)
        before = am.conditions()
        cmd = [sys.executable, "-I", str(CAPTURE), "--project", PROJ, "--label", label, "--out", str(out),
               "--scene", str(ensure_scene(cell["scene"])), "--warmup", str(warmup), "--frames", str(frames),
               "--cam", CAMS[cell["cam"]], "--set", "ViewportGroundGrid=false", "--timeout", "400",
               "--editor", str(EDITOR), "--detailed" if detailed else "--no-counters"]
        if scale != 1.0:
            cmd += ["--set", f"RenderScale={scale}"]
        for kv in cell.get("sets", []):
            cmd += ["--set", kv]
        for kv in cell.get("env", []):
            cmd += ["--env", kv]
        if caffeinate:     # keep the display awake: a locked, dark screen throttles the presents (see autoland audit)
            cmd = ["caffeinate", "-d", "-u", "-t", "420"] + cmd
        r = subprocess.run(cmd, capture_output=True, text=True)
        after = am.conditions()
        line = (r.stdout.strip().splitlines() or [r.stderr.strip()])[-1]
        loaded = before["foreign"] != "none" or after["foreign"] != "none"
        cond.write_text(f"== {label}  {time.strftime('%Y-%m-%d %H:%M:%S')}  attempt {attempt + 1}\n"
                        f"editor md5 {md5(EDITOR)}  water md5 {md5(WATER)}  variant {variant}\n"
                        f"before:\n{am.fmt(before)}after (rc={r.returncode}):\n{am.fmt(after)}")
        print(f"  {line}{'   LOADED' if loaded else ''}", flush=True)
        if r.returncode != 0:
            print(r.stderr[-400:], flush=True)
        profile = out / f"{label}.profile.json"
        if r.returncode == 0 and profile.exists():
            am.scope_sidecar(profile, out / f"{label}.scopes.json")
        if loaded and r.returncode == 0:
            bad = out / "discarded-under-load"
            bad.mkdir(exist_ok=True)
            for ext in ("summary.json", "scopes.json", "conditions.txt", "log", "profile.json"):
                src = out / f"{label}.{ext}"
                if src.exists():
                    shutil.move(str(src), str(bad / f"{label}-try{attempt + 1}.{ext}"))
            continue
        if profile.exists():
            keep = ROOT / "profiles"
            keep.mkdir(exist_ok=True)
            shutil.move(str(profile), str(keep / profile.name))
        return r.returncode == 0
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("group", choices=sorted(CELLS))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--out", default=str(ROOT / "raw"))
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--detailed", action="store_true", help="counters on: exclusive GPU time per pass")
    ap.add_argument("--scale", type=float, default=1.0, help="RenderScale (see autoland_matrix.py: the locked screen)")
    ap.add_argument("--only", help="comma list of cell names")
    ap.add_argument("--home", help="HOME for the editor (an EMPTY directory keeps the EngineContentCache out: with a loaded "
                                   "T_Landscape_Grass_Albedo the water draws as the white fallback, Thema 230)")
    ap.add_argument("--plain", action="store_true", help="scenes without the Auto Landscape material (plain_lake / plain_nowater)")
    ap.add_argument("--caffeinate", action="store_true", help="run under caffeinate -d -u (display awake)")
    ap.add_argument("--tag", default="", help="label prefix suffix, e.g. a variant family")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cells = list(CELLS[a.group].items())
    if a.only:
        keep = set(a.only.split(","))
        cells = [c for c in cells if c[0] in keep]
    if a.plain:
        for _, cell in cells:
            cell["scene"] = "plain_nowater" if cell["scene"].endswith("nowater") else "plain_lake"
    if a.home:
        Path(a.home).mkdir(parents=True, exist_ok=True)
        for _, cell in cells:
            cell["env"] = list(cell.get("env", [])) + [f"HOME={a.home}"]
    if not WATER_ORIG.exists():
        shutil.copyfile(WATER, WATER_ORIG)
    try:
        for rep in range(1, a.reps + 1):
            for name, cell in (cells if rep % 2 else cells[::-1]):
                label = f"{a.group}{a.tag}{'-det' if a.detailed else ''}{'-s%g' % a.scale if a.scale != 1.0 else ''}-{name}-r{rep}"
                if (out / f"{label}.summary.json").exists():
                    continue
                print(f"[{time.strftime('%H:%M:%S')}] {label}", flush=True)
                one_run(label, cell, out, a.detailed, a.warmup, a.frames, a.scale, a.caffeinate)
    finally:
        shutil.copyfile(WATER_ORIG, WATER)


if __name__ == "__main__":
    main()
