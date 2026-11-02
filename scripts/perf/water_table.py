#!/usr/bin/env python3
"""Tables for the water audit (Thema 235): median over the repetitions of every cell, spread next to it.

  python3 scripts/perf/water_table.py /tmp/he235/raw [prefix]
  python3 scripts/perf/water_table.py /tmp/he235/raw --pack docs/perf-audit/raw-water-235

Whole-frame runs (--no-counters): GPU ms (p50 of the frames' gpuMs), CPU ms p50, FPS avg (locked screens throttle it:
read GPU/CPU, not FPS). Detailed runs (<prefix>-det-...): exclusive GPU ms per pass (p50 over frames), then the median
over the repetitions. 'Δ' columns are against the cell named by --base (default: the same cell with 'nowater').
"""
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def load(d, prefix):
    cells = defaultdict(list)
    for f in sorted(Path(d).glob(f"{prefix}*.summary.json")):
        m = re.match(r"(.*)-r(\d+)\.summary\.json", f.name)
        if m:
            cells[m.group(1)].append(json.loads(f.read_text()))
    return cells


def med_spread(v):
    return statistics.median(v), min(v), max(v)


def pack(d, out):
    """One slim record per run (the profile dumps and logs stay out of the repo) + all conditions files in one text."""
    runs, conds = {}, []
    for f in sorted(Path(d).glob("*.summary.json")):
        r = json.loads(f.read_text())
        label = f.name[:-len(".summary.json")]
        rn = r["runner"]
        runs[label] = {
            "size": f"{r['session'].get('width')}x{r['session'].get('height')}", "fps": (r.get("fps") or {}).get("avg"),
            "cpuMsP50": (r.get("cpuMs") or {}).get("p50"), "gpuMsP50": (r.get("gpuMs") or {}).get("p50"),
            "passP50": {k: round(v["p50"], 3) for k, v in (r.get("gpuPassPercentiles") or {}).items()},
            "counters": {k: v for k, v in (r.get("counters_midframe") or {}).items() if k in ("draws", "tris", "visible", "lights")},
            "scene": Path(rn["scene"] or "").name, "cam": rn["cam"], "sets": rn["sets"], "env": rn["env"],
            "counters_on": rn["counters"], "frames": rn["frames"]}
        c = f.with_name(label + ".conditions.txt")
        if c.exists():
            conds.append(c.read_text())
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / "runs.json").write_text(json.dumps(runs, indent=0))
    (Path(out) / "conditions.txt").write_text("\n".join(conds))
    print(f"packed {len(runs)} runs into {out}")


def main():
    if len(sys.argv) > 3 and sys.argv[2] == "--pack":
        return pack(sys.argv[1], sys.argv[3])
    d = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else ""
    cells = load(d, prefix)
    whole = {k: v for k, v in cells.items() if v[0]["runner"]["counters"] is False}
    det = {k: v for k, v in cells.items() if v[0]["runner"]["counters"]}
    if whole:
        print("| cell | n | GPU ms p50 (min..max) | CPU ms p50 (min..max) | FPS avg | draws | tris |")
        print("|---|---|---|---|---|---|---|")
        for k, runs in whole.items():
            g = [r["gpuMs"]["p50"] for r in runs]
            c = [r["cpuMs"]["p50"] for r in runs]
            fps = [r["fps"]["avg"] for r in runs]
            cm = runs[0].get("counters_midframe", {})
            gm, gl, gh = med_spread(g)
            cmed, cl, ch = med_spread(c)
            print(f"| {k} | {len(runs)} | {gm:.2f} ({gl:.2f}..{gh:.2f}) | {cmed:.2f} ({cl:.2f}..{ch:.2f}) | "
                  f"{statistics.median(fps):.1f} | {cm.get('draws')} | {cm.get('tris')} |")
        print()
    if det:
        passes = []
        for runs in det.values():
            for r in runs:
                for p in r["gpuPassPercentiles"]:
                    if p not in passes:
                        passes.append(p)
        print("| cell | n | " + " | ".join(passes) + " | Σ passes | gpuMs p50 |")
        print("|---|---|" + "---|" * (len(passes) + 2))
        for k, runs in det.items():
            row, tot = [], 0.0
            for p in passes:
                v = [r["gpuPassPercentiles"][p]["p50"] for r in runs if p in r["gpuPassPercentiles"]]
                m = statistics.median(v) if v else 0.0
                tot += m
                row.append(f"{m:.2f}")
            g = statistics.median(r["gpuMs"]["p50"] for r in runs)
            print(f"| {k} | {len(runs)} | " + " | ".join(row) + f" | {tot:.2f} | {g:.2f} |")


if __name__ == "__main__":
    main()
