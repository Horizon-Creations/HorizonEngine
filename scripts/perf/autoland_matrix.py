#!/usr/bin/env python3
"""Auto Landscape 2.0 audit matrix (Thema 180, Schritt 6): layer meshes and texture variants on/off.

  python3 scripts/perf/autoland_matrix.py mesh   [--reps 3]   # layer meshes off / x1 / x4 / x16, two extents
  python3 scripts/perf/autoland_matrix.py var    [--reps 3]   # V=1 arrays against V=3 arrays (project override)
  python3 scripts/perf/autoland_matrix.py detail [--reps 3]  # --detailed runs per cell (exclusive GPU time per pass)

Needs (the audit doc says how they were made):
  /tmp/al2/projA/Test/Test.heproj   a copy of a small project; its arrays are the deploy's (V = 1)
  /tmp/al2/projB/Test/Test.heproj   the same with Content/Engine/Textures/Landscape/T_Landscape_*_Array.hasset
                                    of 13 slices (V = 3, landscape_tex_gen --arrays-only --variants 3)
  scenes from scripts/perf/gen_autoland_scene.py (generated here when missing)

Every run goes through scripts/he_perf_capture.py (the real editor loop, Metal, vsync off, fixed camera,
--no-counters unless --detailed). Repetitions alternate their order (rep 1 forward, rep 2 backward ...), so a
slow drift of the machine does not sit on one side. Each run writes
  <label>.summary.json   (he_perf_capture) and <label>.scopes.json (per-frame p50/p90/mean of EVERY CPU scope)
  <label>.conditions.txt (load, screen lock, power mode, GPU use, busiest processes before and after)
A run that overlapped a foreign compile (cc1, clang, ninja, make, another editor, he_tests) is moved to
discarded-under-load/ and repeated, the way the foliage audit did; the driver also waits for a quiet machine
before it starts one.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CAPTURE = REPO / "scripts" / "he_perf_capture.py"
GEN = REPO / "scripts" / "perf" / "gen_autoland_scene.py"
SCENES = Path("/tmp/al2/scenes")
PROJ = {"A": "/tmp/al2/projA/Test/Test.heproj", "B": "/tmp/al2/projB/Test/Test.heproj"}
CAM = "0,10,-20,0,-0.2"            # 15 m over the ground, looking along +Z with the hills ahead

# name -> (extent, density | None for "no layer meshes", extra generator flags)
SCENE_SPEC = {
    "s128-off":  (128, None, []),  "s128-x1": (128, 1, []), "s128-x4": (128, 4, []), "s128-x16": (128, 16, []),
    "s128-x4ns": (128, 4, ["--no-shadows"]),
    "s512-off":  (512, None, []),  "s512-x1": (512, 1, []), "s512-x4": (512, 4, []),
    "s512-x4ns": (512, 4, ["--no-shadows"]),
}
# group -> [(cell name, scene, project)]
CELLS = {
    "mesh": [(s, s, "A") for s in ("s128-off", "s128-x1", "s128-x4", "s128-x16", "s128-x4ns",
                                   "s512-off", "s512-x1", "s512-x4", "s512-x4ns")],
    "var": [(f"{s}-{v}", s, p) for s in ("s128-off", "s512-off", "s128-x4") for v, p in (("V1", "A"), ("V3", "B"))],
    # --detailed (per-pass exclusive GPU time, counters on) of the cells the audit tables compare
    "detail": [("s128-off", "s128-off", "A"), ("s128-x1", "s128-x1", "A"), ("s128-x4", "s128-x4", "A"),
               ("s128-x16", "s128-x16", "A"), ("s128-x4ns", "s128-x4ns", "A"),
               ("s512-off", "s512-off", "A"), ("s512-x1", "s512-x1", "A"), ("s512-x4", "s512-x4", "A"),
               ("s512-x4ns", "s512-x4ns", "A"),
               ("s128-off-V3", "s128-off", "B"), ("s512-off-V3", "s512-off", "B"), ("s128-x4-V3", "s128-x4", "B")],
}
FOREIGN = re.compile(r"\b(cc1|clang|clang\+\+|ninja|make|cmake|ld|HorizonEditor|HorizonGame|he_tests|rustc|swift-frontend)\b")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def foreign_procs():
    """Compiler / linker / editor processes other than the one this driver starts (clangd is an index, not a build)."""
    seen = []
    for line in run(["ps", "-Ao", "pid,pcpu,comm"]).splitlines()[1:]:
        pid, cpu, comm = line.split(None, 2)
        base = os.path.basename(comm)
        if base == "HorizonEditor":          # ours only exists while a capture runs
            continue
        if FOREIGN.fullmatch(base) and "clangd" not in comm:
            seen.append(f"{base}({cpu}%)")
    return seen


def conditions():
    ioreg = run(["/usr/sbin/ioreg", "-n", "Root", "-d1"])
    lock = re.search(r'"CGSSessionScreenIsLocked"=(\w+)', ioreg)
    pm = run(["/usr/bin/pmset", "-g"])
    low = re.search(r"lowpowermode\s+(\d)", pm)
    gpu = re.search(r'"Device Utilization %"=(\d+)', run(["/usr/sbin/ioreg", "-r", "-c", "IOAccelerator", "-d", "1"]))
    top = run(["ps", "-Ao", "pcpu,comm"]).splitlines()[1:]
    top = sorted(top, key=lambda l: -float(l.split(None, 1)[0]))[:4]
    return {
        "uptime": re.sub(r".*load", "load", run(["uptime"]).strip()),
        "locked": lock.group(1) if lock else "none",
        "lowpower": low.group(1) if low else "?",
        "gpu": gpu.group(1) if gpu else "?",
        "power": run(["/usr/bin/pmset", "-g", "batt"]).splitlines()[0],
        "busy": "; ".join(f"{l.split(None, 1)[0]}% {os.path.basename(l.split(None, 1)[1])}" for l in top),
        "foreign": ",".join(foreign_procs()) or "none",
    }


def fmt(c):
    return "".join(f"  {k + ':':10} {v}\n" for k, v in c.items())


def wait_quiet(limit_s=1800):
    t0 = time.time()
    while foreign_procs() and time.time() - t0 < limit_s:
        print(f"  waiting for a quiet machine: {foreign_procs()}", flush=True)
        time.sleep(30)
    return not foreign_procs()


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def scope_sidecar(profile: Path, dst: Path):
    d = json.loads(profile.read_text())
    per = {}
    for f in d.get("frames", []):
        tot = {}
        for c in f.get("cpu", []):
            tot[c["n"]] = tot.get(c["n"], 0.0) + c["ms"]
        for n, ms in tot.items():
            per.setdefault(n, []).append(ms)
    nframes = len(d.get("frames", []))
    out = {"frames": nframes, "scopes": {
        n: {"p50": pct(v, 50), "p90": pct(v, 90), "mean": sum(v) / nframes, "frames": len(v)} for n, v in per.items()}}
    dst.write_text(json.dumps(out, indent=1))


def ensure_scene(name):
    path = SCENES / f"{name.replace('-', '_')}.hescene"
    if path.exists():
        return path
    extent, dens, extra = SCENE_SPEC[name]
    cmd = [sys.executable, "-I", str(GEN), "--extent", str(extent), "--out", str(path)] + extra
    cmd += ["--meshes", "none"] if dens is None else ["--density", str(dens)]
    print(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip())
    return path


def one_run(label, scene, proj, out, detailed, warmup, frames, env, scale=1.0, retries=2):
    cond = out / f"{label}.conditions.txt"
    for attempt in range(retries + 1):
        if not wait_quiet():
            print(f"  [{label}] machine never went quiet, running anyway (flagged)", flush=True)
        before = conditions()
        cmd = [sys.executable, "-I", str(CAPTURE), "--project", PROJ[proj], "--label", label, "--out", str(out),
               "--scene", str(scene), "--warmup", str(warmup), "--frames", str(frames), "--cam", CAM,
               "--set", "ViewportGroundGrid=false", "--timeout", "400"]
        if scale != 1.0:
            cmd += ["--set", f"RenderScale={scale}"]
        if not detailed:
            cmd.append("--no-counters")
        else:
            cmd.append("--detailed")
        for kv in env:
            cmd += ["--env", kv]
        r = subprocess.run(cmd, capture_output=True, text=True)
        after = conditions()
        line = (r.stdout.strip().splitlines() or [r.stderr.strip()])[-1]
        loaded = before["foreign"] != "none" or after["foreign"] != "none"
        cond.write_text(f"== {label}  {time.strftime('%Y-%m-%d %H:%M:%S')}  attempt {attempt + 1}\nbefore:\n{fmt(before)}"
                        f"after (rc={r.returncode}):\n{fmt(after)}")
        print(f"  {line}{'   LOADED' if loaded else ''}", flush=True)
        if r.returncode != 0:
            print(r.stderr[-400:], flush=True)
        profile = out / f"{label}.profile.json"
        if r.returncode == 0 and profile.exists():
            scope_sidecar(profile, out / f"{label}.scopes.json")
        if loaded and r.returncode == 0:
            bad = out / "discarded-under-load"
            bad.mkdir(exist_ok=True)
            for ext in ("summary.json", "scopes.json", "conditions.txt", "log", "profile.json"):
                src = out / f"{label}.{ext}"
                if src.exists():
                    shutil.move(str(src), str(bad / f"{label}-try{attempt + 1}.{ext}"))
            continue
        if profile.exists():
            keep = Path("/tmp/al2/profiles")
            keep.mkdir(exist_ok=True)
            shutil.move(str(profile), str(keep / profile.name))
        return r.returncode == 0
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("group", choices=sorted(CELLS))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--out", default=str(REPO / "docs" / "perf-audit" / "raw-auto-landscape-2"))
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--only", help="comma list of cell names")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="RenderScale. 1 = the editor's default: the locked screen throttles presents, the GPU idles between "
                         "frames and its clock drops, so whole-frame GPU time of a cheap frame is INFLATED (an empty terrain "
                         "measured 17 ms, with 1M more triangles 7.7 ms). 2 = a GPU-bound frame (4x the pixels): the GPU never "
                         "idles and GPU p50 is the cost. Labels of scale != 1 get the suffix '<scale>x' on the group.")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cells = CELLS[a.group]
    if a.only:
        keep = set(a.only.split(","))
        cells = [c for c in cells if c[0] in keep]
    detailed = a.group == "detail"
    reps = a.reps
    env = ["HE_FOLIAGE_STATS=1"] if detailed else []
    for rep in range(1, reps + 1):
        order = cells if rep % 2 == 1 else cells[::-1]
        for name, scene, proj in order:
            grp = a.group if a.scale == 1.0 else f"{a.group}{a.scale:g}x"
            label = f"{grp}-{name}-r{rep}"
            if (out / f"{label}.summary.json").exists():
                continue
            print(f"[{time.strftime('%H:%M:%S')}] {label}", flush=True)
            one_run(label, ensure_scene(scene), proj, out, detailed, a.warmup, a.frames, env, a.scale)


if __name__ == "__main__":
    main()
