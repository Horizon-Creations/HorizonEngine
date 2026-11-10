#!/usr/bin/env python3
"""Measurement adapter for the Hive (`.hive/perf.json`): one capture, one JSON line.

The Hive measures a commit by checking it out in a worktree of its own and calling, once, the profile's `setup`
(`--build`: build the editor of that tree) and then its `capture` as often as `runs` says:

  python3 scripts/perf/hive_capture.py --project Catania --scene default
  python3 scripts/perf/hive_capture.py --project generated --scene entities_stress

The LAST line printed is what the Hive reads:

  {"scopes": {"<scope>": {"p50": ms, "p90": ms}, ...}, "allocations": n}

Everything else goes to stderr. It runs `scripts/he_perf_capture.py` (the real editor loop, private config dir, one
project, scripted capture) and takes the per-frame p50/p90 of every CPU scope and of the frame's CPU and GPU time
from its summary. Allocations are left out: they need the DYLD-inserted alloc_probe on a Mac and are not part of
this adapter (the Hive then compares scopes only).

--project: a path to a `.heproj`, or a name looked up in $HE_PERF_PROJECTS and ~/HorizonEngineProjects. The name
`generated` takes the project named by $HE_PERF_BASE_PROJECT (default Catania) as the host of a generated scene:
a copy of that project gets the scene (SCENES below, made by scripts/perf/gen_reference_world.py), the original is
never touched.
--scene: `default` keeps the project's startup scene.
"""
import argparse
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
WINDOWS = sys.platform == "win32"
EDITOR = REPO / "out" / "deploy" / "Editor" / ("HorizonEditor.exe" if WINDOWS else "HorizonEditor")
TEMPLATE = REPO / "docs" / "perf-audit" / "scenes" / "landscape_noclouds.hescene"

# One scene per feature, loading the feature as hard as a scene can (the engine's generator is the same one the
# perf audits use). The numbers are a first stress level; raise them when a feature stops showing up in the scopes.
SCENES = {
    "entities_stress": ["--count", "20000", "--extent", "1500"],
    "lights_stress": ["--count", "2000", "--extent", "300", "--lights", "256"],
    "physics_stress": ["--count", "3000", "--extent", "200", "--physics", "0.5"],
    "foliage_stress": ["--count", "0", "--foliage", "200000"],
}


def log(*parts):
    print(*parts, file=sys.stderr, flush=True)


def run(command, **kwargs):
    log("+", " ".join(str(c) for c in command))
    done = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kwargs)
    sys.stderr.write(done.stdout[-4000:] if done.stdout else "")
    if done.returncode != 0:
        sys.exit(f"command failed ({done.returncode}): {' '.join(str(c) for c in command)}")
    return done.stdout


def build():
    """Build and deploy the editor of this tree (what POST_BUILD copies to out/deploy). Incremental: a tree that
    is built already costs seconds."""
    if WINDOWS:
        if not (REPO / "out" / "build" / "x64-release").exists():
            run(["cmd", "/c", str(REPO / "_build_configure.bat")], cwd=REPO)
        run(["cmd", "/c", str(REPO / "_build_editor.bat")], cwd=REPO)
        return
    build_dir = REPO / "cmake-build-release"
    env = dict(os.environ, CCACHE_BASEDIR=str(REPO), CCACHE_NOHASHDIR="1")
    if not (build_dir / "CMakeCache.txt").exists():
        launcher = ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"] if shutil.which("ccache") else []
        run(["cmake", "-S", str(REPO), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", *launcher], cwd=REPO, env=env)
    run(["cmake", "--build", str(build_dir), "--target", "HorizonEditor", "-j", str(multiprocessing.cpu_count())],
        cwd=REPO, env=env)


def find_project(name):
    path = Path(name).expanduser()
    if path.suffix == ".heproj" and path.is_file():
        return path.resolve()
    roots = [Path(p) for p in os.environ.get("HE_PERF_PROJECTS", "").split(os.pathsep) if p]
    roots.append(Path.home() / "HorizonEngineProjects")
    for root in roots:
        folder = root / name
        found = sorted(folder.glob("*.heproj")) if folder.is_dir() else []
        if found:
            return found[0].resolve()
    sys.exit(f"no project {name!r}: give a .heproj path, or put it in $HE_PERF_PROJECTS or ~/HorizonEngineProjects")


def host_copy(base, work):
    """A copy of the project to put a generated scene into (never the original; no saved state, no exports)."""
    target = work / "project"
    shutil.copytree(base.parent, target, ignore=shutil.ignore_patterns("Saved", "Export", "Exports", ".git", "Intermediate"))
    return target / base.name


def generated_scene(scene, work):
    if scene not in SCENES:
        sys.exit(f"no generated scene {scene!r}: one of {', '.join(sorted(SCENES))}")
    out = work / f"{scene}.hescene"
    run([sys.executable, str(REPO / "scripts" / "perf" / "gen_reference_world.py"), *SCENES[scene],
         "--template", str(TEMPLATE), "--out", str(out), "--seed", "1"], cwd=REPO)
    return out


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def result_of(summary, dump):
    """The Hive's JSON from he_perf_capture's summary and its dump: p50/p90 per frame of every CPU scope, and of
    the frame's CPU and GPU time (the dump's own frames: the summary holds only the p50 of those)."""
    scopes = {}
    for name, v in (summary.get("cpuScopePerFrame") or {}).items():
        scopes[name] = {"p50": float(v["p50"]), "p90": float(v["p90"])}
    frames = dump.get("frames") or []
    for label, key in (("Frame CPU", "cpuMs"), ("Frame GPU", "gpuMs")):
        values = [float(f[key]) for f in frames if f.get(key) is not None]
        if values:
            scopes[label] = {"p50": pct(values, 50), "p90": pct(values, 90)}
    for name, v in (summary.get("gpuPassPercentiles") or {}).items():
        scopes["GPU " + name] = {"p50": float(v["p50"]), "p90": float(v["p90"])}
    return {"scopes": scopes}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true", help="only build and deploy the editor of this tree")
    ap.add_argument("--project", default="Catania")
    ap.add_argument("--scene", default="default")
    ap.add_argument("--rhi", default=os.environ.get("HE_PERF_RHI") or ("D3D12" if WINDOWS else "Metal"))
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--timeout", type=float, default=900)
    a = ap.parse_args()
    if a.build:
        build()
        return
    if not EDITOR.exists():
        build()
    with tempfile.TemporaryDirectory(prefix="hive_perf_") as tmp:
        work = Path(tmp)
        if a.project == "generated":
            base = find_project(os.environ.get("HE_PERF_BASE_PROJECT", "Catania"))
            project, scene = host_copy(base, work), generated_scene(a.scene, work)
        else:
            project, scene = find_project(a.project), None
            if a.scene not in ("", "default"):
                sys.exit(f"{a.project!r} has only its startup scene ('default'); scenes are generated with --project generated")
        command = [sys.executable, str(REPO / "scripts" / "he_perf_capture.py"), "--project", str(project),
                   "--label", "hive", "--out", str(work / "out"), "--cfgdir", str(work / "cfg"), "--rhi", a.rhi,
                   "--warmup", str(a.warmup), "--frames", str(a.frames), "--timeout", str(a.timeout), "--no-counters"]
        if scene is not None:
            command += ["--scene", str(scene)]
        run(command, cwd=REPO)
        summary = json.loads((work / "out" / "hive.summary.json").read_text())
        dump = json.loads((work / "out" / "hive.profile.json").read_text())
    result = result_of(summary, dump)
    if not result["scopes"]:
        sys.exit("the capture holds no scopes")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
