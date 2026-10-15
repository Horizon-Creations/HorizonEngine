#!/usr/bin/env python3
"""Tables for foliage_ladder.sh runs (Thema 163, Schritt 2c).

For each <label>.profile.json (next to its <label>.summary.json): the frame
counters of a steady frame (objects, visible, draws, triangles), CPU and GPU
time per frame (p50 / p90), the per-frame sum of the scopes the foliage plan
compares (p50), and resident memory. The first frames of a capture are skipped
(--skip, default 2) because the first draws upload meshes and pipelines.

  python3 scripts/perf/foliage_ladder_table.py docs/perf-audit/raw-foliage/base-*.profile.json
  python3 scripts/perf/foliage_ladder_table.py --list docs/perf-audit/raw-foliage/base-100000.profile.json

--list prints every CPU scope of the first file by its p50, to pick scopes from.
Labels are sorted by instance count (the number after the last dash in the
label's first part, e.g. base-100000-dist100 sorts as 100000).
"""
import argparse
import json
import re
import statistics
from pathlib import Path

# The scopes nest (Render holds the encoders, an encoder holds its culls), so the
# columns do not add up; each one is the per-frame SUM over its calls. "Render" is
# the number to compare: CPU p50 contains the wait for the display (NextDrawable,
# which a locked screen turns into ~11 ms of nothing), so a cheap frame looks slow.
SCOPES = ["Render", "RenderExtractor::extract", "RenderExtractor::reuse", "ExtractFoliage",
          "FrustumCuller::cull", "Metal::EncodeShadowMap", "Metal::EncodeScene", "Metal::EncodeSSAO",
          "Metal::NextDrawable"]


def pct(v, p):
    v = sorted(v)
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def scope_sum(frame, name):
    return sum(s["ms"] for s in frame["cpu"] if s["n"] == name)


def calls(frame, name):
    return sum(1 for s in frame["cpu"] if s["n"] == name)


def instances(label):
    m = re.search(r"-(\d+)(?:-|$)", label)
    return int(m.group(1)) if m else 0


def load(path, skip):
    p = json.loads(Path(path).read_text())
    label = Path(path).name.replace(".profile.json", "")
    frames = p["frames"][skip:] or p["frames"]
    summ_path = Path(path).with_name(label + ".summary.json")
    summ = json.loads(summ_path.read_text()) if summ_path.exists() else {}
    return label, p, frames, summ


def row(path, skip):
    label, p, frames, summ = load(path, skip)
    cpu = [f["cpuMs"] for f in frames]
    gpu = [f["gpuMs"] for f in frames if "gpuMs" in f]
    mid = frames[len(frames) // 2].get("stats", {})
    return {
        "label": label, "n": instances(label), "frames": len(frames),
        "total": mid.get("total"), "visible": mid.get("visible"),
        "draws": mid.get("draws"), "tris": mid.get("tris"),
        "cpu50": pct(cpu, 50), "cpu90": pct(cpu, 90),
        "gpu50": pct(gpu, 50) if gpu else None, "gpu90": pct(gpu, 90) if gpu else None,
        "scopes": {n: pct([scope_sum(f, n) for f in frames], 50) for n in SCOPES},
        "foliageCalls": pct([calls(f, "ExtractFoliage") for f in frames], 50),
        "rss": (summ.get("rssMB") or {}).get("max"),
        "note": (p.get("session") or {}).get("note", ""),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--skip", type=int, default=2)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    f = lambda v, d=2: "-" if v is None else f"{v:.{d}f}"

    if a.list:
        _, _, frames, _ = load(a.files[0], a.skip)
        names = {s["n"] for fr in frames for s in fr["cpu"]}
        rows = sorted(((pct([scope_sum(fr, n) for fr in frames], 50), n) for n in names), reverse=True)
        for ms, n in rows[:40]:
            print(f"{ms:9.3f} ms  {n}")
        return

    rows = sorted((row(x, a.skip) for x in a.files), key=lambda r: (r["label"].split("-")[0], r["n"], r["label"]))
    print("| run | objects | visible | draws | triangles | CPU p50 | CPU p90 | GPU p50 | GPU p90 | RSS max MB |")
    print("|" + "---|" * 10)
    for r in rows:
        print(f"| {r['label']} | {r['total']} | {r['visible']} | {r['draws']} | {r['tris']} | "
              f"{f(r['cpu50'])} | {f(r['cpu90'])} | {f(r['gpu50'])} | {f(r['gpu90'])} | {f(r['rss'], 0)} |")
    print()
    print("| run | " + " | ".join(SCOPES) + " | ExtractFoliage calls/frame |")
    print("|" + "---|" * (len(SCOPES) + 2))
    for r in rows:
        print(f"| {r['label']} | " + " | ".join(f(r["scopes"][n], 2) for n in SCOPES)
              + f" | {f(r['foliageCalls'], 0)} |")


if __name__ == "__main__":
    main()
