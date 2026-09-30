#!/usr/bin/env python3
"""table.py DIR PREFIX : per-run pass p50 table + pair differences (after minus mean of neighbours)."""
import json, sys, glob, os, statistics

d, pfx = sys.argv[1], sys.argv[2]
order = sys.argv[3].split(",")
rows = []
for lab in order:
    f = os.path.join(d, f"{pfx}-{lab}.summary.json")
    s = json.load(open(f))
    pp = s.get("gpuPassPercentiles", {})
    g = lambda n, k="p50": pp.get(n, {}).get(k, float("nan"))
    cs = s.get("cpuScopePerFrame", {})
    lut = cs.get("Metal::EncodeSkyViewLut", {}).get("p50", float("nan"))
    rows.append((lab, g("Scene", "min"), g("Scene", "p10"), g("Scene"), g("Shadow"),
                 s["gpuMs"]["p50"], s["deltaMs"]["p50"], s.get("hitchCount", 0), lut,
                 s.get("rssMB", {}).get("max", float("nan"))))
print("| Lauf | Scene min | p10 | p50 | Shadow p50 | gpuMs p50 | delta p50 | Hänger | EncodeSkyViewLut p50 | RSS max MB |")
print("|---|---|---|---|---|---|---|---|---|---|")
for r in rows:
    print(f"| {pfx}-{r[0]} | {r[1]:.2f} | {r[2]:.2f} | {r[3]:.2f} | {r[4]:.3f} | {r[5]:.2f} | {r[6]:.2f} | {r[7]} | {r[8]:.4f} | {r[9]:.0f} |")
# pair differences: each run vs mean of neighbours of the other kind
for idx, name in ((3, "Scene p50"), (4, "Shadow p50"), (5, "gpuMs p50")):
    diffs = []
    for i in range(1, len(rows) - 1):
        a, b, c = rows[i - 1], rows[i], rows[i + 1]
        ka, kb, kc = (x[0].rstrip("0123456789b") for x in (a, b, c))
        if ka == kc != kb:
            sign = 1 if kb in ("A",) else -1
            diffs.append(sign * (b[idx] - (a[idx] + c[idx]) / 2))
    print(f"{name}: nachher - vorher, Nachbarmittel: " + " / ".join(f"{x:+.2f}" for x in diffs)
          + (f"  Median {statistics.median(diffs):+.2f}" if diffs else ""))
