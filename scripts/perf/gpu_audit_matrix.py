#!/usr/bin/env python3
"""GPU audit matrix (Thema 239, Schritt 1): per-pass GPU cost of typical scenes on Metal, ABBA.

  python3 scripts/perf/gpu_audit_matrix.py run   --proj /tmp/gpuaudit/proj/Catania/HEProject.heproj \
          --scenes /tmp/gpuaudit/scenes --out docs/perf-audit/raw-gpu-audit [--reps 2]
  python3 scripts/perf/gpu_audit_matrix.py table --out docs/perf-audit/raw-gpu-audit

Every cell is ONE editor process through scripts/he_perf_capture.py (real editor loop, Metal, vsync off,
fixed camera) with --detailed --set RenderScale=2: the run is GPU-bound, the GPU clock stays up and
the per-pass times are exclusive. Compare Pass-p50 only; Tonemap (a fixed-cost full-screen pass) is the
clock witness, `table` prints it per row and flags a row that differs from the median by more than
--tol (default 10 %). Repetitions alternate their order (rep 1 forward, rep 2 backward).

Scenes (scripts/perf/gen_autoland_scene.py, gen_water_scene.py; written here when missing):
  base128 | land512 | land512_foliage | lake512_nowater | lake512 | catania (the project's own startup scene)
The generated foliage layers set castsShadow/contributesAO explicitly (see the generator), the log line
'shadow pass instanced (first run: N casters)' is kept in <label>.log as the witness.

--scene overwrites the project's startup scene: the driver keeps the original under $TMPDIR/gpu_audit_startup_original.hescene
and puts it back at the end. Use a private project copy, never the human's.
"""
import argparse
import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CAPTURE = REPO / "scripts" / "he_perf_capture.py"
GEN_LAND = REPO / "scripts" / "perf" / "gen_autoland_scene.py"
GEN_WATER = REPO / "scripts" / "perf" / "gen_water_scene.py"
CAM = "0,25,90,0,-0.25"
CELLS = [  # label, scene file (None = the project's own startup scene), generator args
    ("catania", None, None),
    ("base128", "base128.hescene", [GEN_LAND, "--extent", "128", "--meshes", "none"]),
    ("land512", "land512_nofoliage.hescene", [GEN_LAND, "--extent", "512", "--meshes", "none"]),
    ("land512_foliage", "land512_foliage.hescene", [GEN_LAND, "--extent", "512"]),
    ("lake512_nowater", "lake512_nowater.hescene", [GEN_WATER, "--no-water"]),
    ("lake512", "lake512.hescene", [GEN_WATER]),
]
WITNESS = "Tonemap"


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def conditions(label):
    return "\n".join([
        f"== {label} {time.strftime('%H:%M:%S')}",
        sh("uptime"),
        sh("/usr/bin/pmset -g batt | head -1"),
        "lowpowermode " + sh("/usr/bin/pmset -g | grep lowpowermode | awk '{print $2}'"),
        sh("/usr/sbin/ioreg -r -c IOAccelerator -d 1 | grep -o '\"Device Utilization %\"=[0-9]*'"),
        "locked: " + sh("/usr/sbin/ioreg -n Root -d1 -a 2>/dev/null | grep -c 'CGSSessionScreenIsLocked' || true"),
        "foreign: " + sh("ps -axo comm | grep -E 'cc1|clang|ninja|he_tests|HorizonEditor' | grep -v grep | sort | uniq -c | tr '\\n' ' '"),
    ])


def wait_quiet(limit=8.0, tries=40):
    for _ in range(tries):
        load = float(sh("/usr/sbin/sysctl -n vm.loadavg").strip("{} ").split()[0])
        if load < limit:
            return load
        time.sleep(15)
    return load


def run(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    scenes = Path(a.scenes)
    scenes.mkdir(parents=True, exist_ok=True)
    proj = Path(a.proj)
    heproj = json.loads(proj.read_text())
    startup = proj.parent / heproj["startupScene"]
    keep = Path(tempfile.gettempdir()) / "gpu_audit_startup_original.hescene"
    if not keep.exists():
        keep.write_bytes(startup.read_bytes())
    for _, f, gen in CELLS:
        if f and not (scenes / f).exists():
            subprocess.run([sys.executable, *map(str, gen), "--out", str(scenes / f)], check=True)
    only = set(a.only.split(",")) if a.only else None
    cells = [c for c in CELLS if not only or c[0] in only]
    try:
        for rep in range(1, a.reps + 1):
            order = cells if rep % 2 else list(reversed(cells))
            for label, f, _ in order:
                name = f"{label}{a.suffix}-r{rep}"
                if (out / f"{name}.summary.json").exists():
                    continue
                load = wait_quiet()
                cond = conditions(name) + f"\nload before start (1 min): {load}\n"
                scene = str(scenes / f) if f else str(keep)
                cmd = ["caffeinate", "-u", sys.executable, str(CAPTURE), "--project", str(proj), "--label", name,
                       "--out", str(out), "--scene", scene, "--cam", CAM, "--detailed", "--set", f"RenderScale={a.scale}",
                       "--warmup", str(a.warmup), "--frames", str(a.frames), "--timeout", "420"]
                for kv in a.set:
                    cmd += ["--set", kv]
                r = subprocess.run(cmd, capture_output=True, text=True)
                cond += conditions(name + " after").split("\n", 1)[1] + "\n" + r.stdout + r.stderr
                (out / f"{name}.conditions.txt").write_text(cond)
                print(r.stdout.strip() or r.stderr.strip(), flush=True)
    finally:
        startup.write_bytes(keep.read_bytes())


def table(a):
    out = Path(a.out)
    rows = {}
    for p in sorted(out.glob((a.match or "*") + "-r[0-9].summary.json")):
        if not a.match and any(m in p.name for m in ("-rs", "-ab")):   # ablation runs: --match '*-ab-nossao'
            continue
        s = json.loads(p.read_text())
        label = p.name.replace(".summary.json", "")
        rows[label] = s
    passes = []
    for s in rows.values():
        for k in s.get("gpuPassPercentiles", {}):
            if k not in passes:
                passes.append(k)
    wit = {l: s["gpuPassPercentiles"].get(WITNESS, {}).get("p50") for l, s in rows.items()}
    med = statistics.median([v for v in wit.values() if v])
    print(f"| run | " + " | ".join(passes) + " | Σ passes | gpuMs p50 | draws | tris | witness |")
    print("|---|" + "---|" * (len(passes) + 5))
    for l, s in rows.items():
        pp = s["gpuPassPercentiles"]
        tot = sum(v["p50"] for v in pp.values())
        c = s.get("counters_midframe", {})
        w = wit[l]
        flag = "ok" if w and abs(w - med) <= a.tol * med else "**off**"
        cells = [f"{pp[k]['p50']:.2f}" if k in pp else "" for k in passes]
        print(f"| {l} | " + " | ".join(cells) + f" | {tot:.2f} | {s.get('gpuMs', {}).get('p50', 0):.2f} | "
              f"{c.get('drawCalls', c.get('draws', ''))} | {c.get('triangles', c.get('tris', ''))} | {w:.3f} {flag} |")
    print(f"\nTonemap witness median {med:.3f} ms, tolerance {a.tol * 100:.0f} %")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["run", "table"])
    ap.add_argument("--proj")
    ap.add_argument("--scenes", default="/tmp/gpuaudit/scenes")
    ap.add_argument("--out", default=str(REPO / "docs" / "perf-audit" / "raw-gpu-audit"))
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--only")
    ap.add_argument("--match", help="table: glob of run labels without the -rN suffix (ablation runs)")
    ap.add_argument("--suffix", default="", help="label suffix for an ablation run, e.g. -rs1 (table skips them)")
    ap.add_argument("--scale", default="2", help="RenderScale (default 2: GPU-bound)")
    ap.add_argument("--set", action="append", default=[], help="extra CustomConfig KEY=VALUE (ablation)")
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--tol", type=float, default=0.10)
    a = ap.parse_args()
    (run if a.mode == "run" else table)(a)


if __name__ == "__main__":
    main()
