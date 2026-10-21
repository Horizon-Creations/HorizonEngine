#!/usr/bin/env python3
"""Tables for the landscape performance investigation (Thema 237): median over the repetitions of every cell.

  python3 scripts/perf/landscape_regression_table.py docs/perf-audit/raw-landscape-regression [--cells a,b,c]

A cell is a label without its trailing -rN (cur-base-det-r1, -r2, -r3 are the cell cur-base-det). Detailed cells
(-det: counters on, each pass its own command buffer) give the per-pass GPU time p50 and their sum; the whole-frame
cells (-fps, -gl) give FPS and CPU p50. FPS of a detailed cell is perturbed by the serialisation: never quote it.
Columns that are not in a run print '-'. min..max is over the repetitions.
"""
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

PASS_GROUPS = [("Scene", ("Scene",)), ("SSAO", ("SSAO",)), ("Shadow", ("Shadow",)),
               ("GI diff", ("GIShadow", "GIAccel", "GIProbes")),
               ("GI refl", ("GIRefl", "GIReflections", "GIReflBlur", "GIReflTemporal", "GIReflMix")),
               ("Bloom", ("Bloom",)), ("Tonemap", ("Tonemap",)), ("Present", ("Present",))]


def load(d: Path):
    cells = defaultdict(list)
    for f in sorted(d.glob("*.summary.json")):
        m = re.match(r"(.*)-r(\d+)\.summary\.json$", f.name)
        if m:
            cells[m.group(1)].append(json.loads(f.read_text()))
    return cells


def med(v):
    return statistics.median(v) if v else None


def rng(v):
    return f"{min(v):.2f}..{max(v):.2f}" if v else "-"


def passes(summ):
    p = summ.get("gpuPassPercentiles") or {}
    out = {}
    for col, names in PASS_GROUPS:
        vals = [p[n]["p50"] for n in p if any(n == k or n.startswith(k) for k in names)]
        out[col] = sum(vals) if vals else None
    known = {n for _, names in PASS_GROUPS for n in names}
    out["other"] = sum(v["p50"] for k, v in p.items() if not any(k == n or k.startswith(n) for n in known)) or None
    return out


def main():
    d = Path(sys.argv[1])
    only = None
    if "--cells" in sys.argv:
        only = sys.argv[sys.argv.index("--cells") + 1].split(",")
    cells = load(d)
    names = [c for c in cells if (only is None or c in only)]
    det = [c for c in names if c.endswith("-det")]
    other = [c for c in names if not c.endswith("-det")]
    cols = [c for c, _ in PASS_GROUPS] + ["other"]
    if det:
        print("### Detailed GPU time per pass (ms, p50 of the pass over 600 frames, median over the repetitions)\n")
        print("| Cell | n | " + " | ".join(cols) + " | Σ passes | Σ min..max |")
        print("|---|---|" + "---|" * (len(cols) + 2))
        for c in det:
            runs = [passes(s) for s in cells[c]]
            row = []
            for col in cols:
                v = [r[col] for r in runs if r[col] is not None]
                row.append(f"{med(v):.2f}" if v else "-")
            sums = [sum(x for x in r.values() if x) for r in runs]
            print(f"| {c} | {len(runs)} | " + " | ".join(row) + f" | {med(sums):.2f} | {rng(sums)} |")
        print()
    if other:
        print("### Whole frame (vsync off, counters off)\n")
        print("| Cell | n | FPS (median) | FPS min..max | CPU p50 ms | draws | tris |")
        print("|---|---|---|---|---|---|---|")
        for c in other:
            ss = cells[c]
            fps = [s["fps"]["avg"] for s in ss]
            cpu = [s["cpuMs"]["p50"] for s in ss]
            mid = ss[0].get("counters_midframe", {})
            print(f"| {c} | {len(ss)} | {med(fps):.1f} | {rng(fps)} | {med(cpu):.2f} | {mid.get('draws')} | {mid.get('tris')} |")


if __name__ == "__main__":
    main()
