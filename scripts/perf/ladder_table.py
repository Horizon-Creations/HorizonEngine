#!/usr/bin/env python3
"""One table row per world_streaming_ladder.sh run (Thema 153, Schritt 5).

For each <label>.profile.json next to its <label>.log: load timings from the log
(SceneLoadTiming / SceneOpenTiming), CPU per frame (p50, p99 without frames 0/1),
the post-load hitch (frames 0 and 1, and Metal::EncodeScene in them), frames
from 2 on that cost more than twice the median, RSS max and the per-frame sums
of the scopes the baseline document compares.

  python3 scripts/perf/ladder_table.py /tmp/ws5/raw/s5-*.profile.json
"""
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SCOPES = ["RenderExtractor::extract", "Metal::EncodeScene", "Metal::EncodeSSAO",
          "Metal::EncodeShadowMap", "Metal::Overlay", "FrustumCull", "OnRender"]


def pct(v, p):
    v = sorted(v)
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def timings(log):
    out = {}
    if not log.exists():
        return out
    for line in log.read_text(errors="replace").splitlines():
        if "SceneLoadTiming" in line or "SceneOpenTiming" in line:
            for k, v in re.findall(r"(\w+)=([\d.]+)", line):
                out[k] = float(v)
    return out


def scope_sum(frame, name):
    return sum(s["ms"] for s in frame["cpu"] if s["n"] == name)


def row(path):
    p = json.loads(Path(path).read_text())
    frames = p["frames"]
    label = Path(path).name.replace(".profile.json", "")
    t = timings(Path(path).with_name(label + ".log"))
    summ = json.loads(Path(path).with_name(label + ".summary.json").read_text())
    cpu = [f["cpuMs"] for f in frames]
    steady = cpu[2:] or cpu
    med = statistics.median(steady)
    stalls = sum(1 for c in steady if c > 2 * med)
    scopes = {n: pct([scope_sum(f, n) for f in frames[2:]], 50) for n in SCOPES}
    return {
        "label": label,
        "entities": frames[0]["stats"].get("entities"),
        "parseMs": t.get("parseMs"), "buildMs": t.get("buildMs"), "loadMs": t.get("loadMs"),
        "warmupMs": t.get("warmupMs"), "totalMs": t.get("totalMs"),
        "cpuP50": pct(steady, 50), "cpuP99": pct(steady, 99),
        "f0": cpu[0], "f1": cpu[1] if len(cpu) > 1 else None,
        "f0Scene": scope_sum(frames[0], "Metal::EncodeScene"),
        "f1Scene": scope_sum(frames[1], "Metal::EncodeScene") if len(frames) > 1 else None,
        "stallsAfter1": stalls, "rssMax": summ["rssMB"]["max"],
        "scopesP50": scopes,
    }


def main():
    rows = [row(a) for a in sys.argv[1:]]
    rows.sort(key=lambda r: (r["label"].rsplit("-", 1)[0], r["entities"] or 0))
    fmt = lambda v: "-" if v is None else f"{v:.1f}"
    print("| run | entities | parse | build | load | warmup | total | CPU p50 | p99 (ab F2) "
          "| F0 / F1 | EncodeScene F0 / F1 | >2×Median ab F2 | RSS max |")
    print("|" + "---|" * 13)
    for r in rows:
        print(f"| {r['label']} | {r['entities']} | {fmt(r['parseMs'])} | {fmt(r['buildMs'])} | "
              f"{fmt(r['loadMs'])} | {fmt(r['warmupMs'])} | {fmt(r['totalMs'])} | {fmt(r['cpuP50'])} | "
              f"{fmt(r['cpuP99'])} | {fmt(r['f0'])} / {fmt(r['f1'])} | "
              f"{fmt(r['f0Scene'])} / {fmt(r['f1Scene'])} | {r['stallsAfter1']} | {fmt(r['rssMax'])} |")
    print()
    print("| run | " + " | ".join(SCOPES) + " |")
    print("|" + "---|" * (len(SCOPES) + 1))
    for r in rows:
        print(f"| {r['label']} | " + " | ".join(fmt(r["scopesP50"][n]) for n in SCOPES) + " |")


if __name__ == "__main__":
    main()
