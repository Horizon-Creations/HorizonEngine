#!/usr/bin/env python3
"""Old (texture snow, pre Thema 329 step 2) against new (MF_ProceduralSnow / Sand) landscape material, per group.

  python3 scripts/perf/snow_sand_cost_ab.py run   --work /tmp/he328 [--reps 3] [--rhi Metal] [--poses top,low]
  python3 scripts/perf/snow_sand_cost_ab.py table --work /tmp/he328

Needs <work>/new and <work>/old (copied editor deploys, see scripts/perf/selfcontain_deploy_copy.sh; `old` has the four
M_AutoLandscape*.hasset of the commit before step 2 swapped in) and <work>/proj_<Group> (snow_sand_setup.py).
Each cell is landscape_regression.py --detailed --scale 2 (4x the pixels so the GPU never idles, see
docs/perf-audit/auto-landscape-material-2-audit-2026-10-21.md, 'Die Falle bei der GPU-Zeit'); the order of old/new
alternates every repetition. The table reads the per-pass GPU p50 (Scene) and the Tonemap pass as the clock witness.
"""
import argparse, json, statistics, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GROUPS = ["Mountains", "Forest", "Grassland", "Desert"]
POSES = {"top": "0,150,0,0,-1.4", "low": "0,15,0,0,-0.2"}   # x,y,z,yaw,pitch: terrain fills the frame / gameplay height


def run(a):
    raw = Path(a.work) / "raw"
    raw.mkdir(exist_ok=True)
    for rep in range(1, a.reps + 1):
        for pose in a.poses.split(","):
            for g in GROUPS:
                for side in (["old", "new"] if rep % 2 else ["new", "old"]):
                    label = f"{g}-{side}-{pose}-{a.rhi}-r{rep}"
                    if (raw / f"{label}.summary.json").exists():
                        continue
                    subprocess.run([sys.executable, str(REPO / "scripts/perf/landscape_regression.py"), "--label", label,
                                    "--editor", f"{a.work}/{side}/HorizonEditor", "--project", f"{a.work}/proj_{g}/{g}.heproj",
                                    "--cam", POSES[pose], "--rhi", a.rhi, "--detailed", "--scale", "2", "--out", str(raw),
                                    "--timeout", "300"], check=False)


def table(a):
    raw = Path(a.work) / "raw"
    rows = {}
    for f in sorted(raw.glob("*-r*.summary.json")):
        g, side, pose, rhi, rep = f.name.replace(".summary.json", "").split("-")
        s = json.loads(f.read_text())
        p = s["gpuPassPercentiles"]
        rows.setdefault((rhi, pose, g, side), []).append(
            (p["Scene"]["p50"], p["Tonemap"]["p50"], sum(v["p50"] for v in p.values()), s["fps"]["avg"], s["cpuMs"]["p50"]))
    print("| RHI | pose | group | Scene p50 old | Scene p50 new | delta ms | delta % | sum passes old | sum passes new | Tonemap old/new (clock) | reps | Scene min..max old / new |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for (rhi, pose, g) in sorted({(k[0], k[1], k[2]) for k in rows}):
        o, n = rows.get((rhi, pose, g, "old")), rows.get((rhi, pose, g, "new"))
        if not o or not n:
            continue
        m = lambda v, i: statistics.median(x[i] for x in v)
        rg = lambda v: f"{min(x[0] for x in v):.2f}..{max(x[0] for x in v):.2f}"
        print(f"| {rhi} | {pose} | {g} | {m(o,0):.2f} | {m(n,0):.2f} | {m(n,0)-m(o,0):+.2f} | {100*(m(n,0)/m(o,0)-1):+.1f} % | "
              f"{m(o,2):.2f} | {m(n,2):.2f} | {m(o,1):.2f} / {m(n,1):.2f} | {len(o)}/{len(n)} | {rg(o)} / {rg(n)} |")


ap = argparse.ArgumentParser()
ap.add_argument("cmd", choices=["run", "table"])
ap.add_argument("--work", required=True)
ap.add_argument("--reps", type=int, default=3)
ap.add_argument("--rhi", default="Metal")
ap.add_argument("--poses", default="top,low")
a = ap.parse_args()
(run if a.cmd == "run" else table)(a)
