#!/usr/bin/env python3
"""Per-frame CPU analysis of profiler dumps (perf audit Thema 99, Schritt 3).

For each dump:
  * main-thread CPU frame minus Metal::NextDrawable (the part that is engine
    work), as a distribution, plus the frames where it exceeds --stall ms and
    the scopes that grew in them compared with the median frame;
  * job-system fan-out: jobs per frame, worker busy time per frame, and the
    main-thread time of the scopes that dispatch them (extract/cull);
  * allocation counters per frame when the dump carries them (alloc_probe).

  python3 scripts/perf/step3_frames.py docs/perf-audit/raw-step3/C1-*.profile.json.gz
"""
import argparse
import gzip
import json
import statistics
import sys
from collections import defaultdict


def load(path):
    op = gzip.open if path.endswith(".gz") else open
    with op(path, "rt") as f:
        return json.load(f)


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def fmt(v):
    return f"{pct(v, 50):.3f} / {pct(v, 90):.3f} / {pct(v, 99):.3f} / {max(v):.3f}"


def analyse(path, stall_ms):
    d = load(path)
    frames = d["frames"]
    marks = {m["i"]: (m["s"], m["e"]) for m in d.get("frameMarks", [])}
    out = {"dump": path, "frames": len(frames), "note": d["session"].get("note")}

    # Per-frame scope sums (inclusive time of each named scope, summed per frame).
    per = []
    for f in frames:
        acc = defaultdict(float)
        top = 0.0
        for s in f.get("cpu", []):
            acc[s["n"]] += s["ms"]
            if s["d"] == 0:
                top += s["ms"]
        per.append((f, acc, top))

    nd = [a.get("Metal::NextDrawable", 0.0) for _, a, _ in per]
    cpu = [f["cpuMs"] for f, _, _ in per]
    work = [c - n for c, n in zip(cpu, nd)]
    untracked = [f["cpuMs"] - t for f, _, t in per]  # frame time outside any top-level scope
    out["cpuMs p50/p90/p99/max"] = fmt(cpu)
    out["NextDrawable p50/p90/p99/max"] = fmt(nd)
    out["cpu-minus-ND p50/p90/p99/max"] = fmt(work)
    out["outside top-level scopes p50/p90/p99/max"] = fmt(untracked)

    names = set()
    for _, a, _ in per:
        names.update(a)
    med = {n: statistics.median([a.get(n, 0.0) for _, a, _ in per]) for n in names}
    out["scope p50 ms (per frame)"] = {n: round(v, 4) for n, v in sorted(med.items(), key=lambda kv: -kv[1])[:25]}

    stalls = []
    for (f, a, t), w in zip(per, work):
        if w > stall_ms:
            grew = sorted(((a.get(n, 0.0) - med[n], n) for n in names if n != "Metal::NextDrawable"),
                          reverse=True)[:4]
            stalls.append({"frame": f["i"], "cpuMinusND": round(w, 3),
                           "outsideScopes": round(f["cpuMs"] - t, 3),
                           "grew": [(n, round(g, 3)) for g, n in grew if g > 0.05]})
    out[f"frames with cpu-minus-ND > {stall_ms} ms"] = len(stalls)
    out["stall frames"] = stalls[:15]

    # Job system: spans on worker threads falling inside each frame window.
    workers = [t for t in d.get("threads", []) if not t.get("main")]
    mainT = next((t for t in d.get("threads", []) if t.get("main")), None)
    if workers and marks:
        jobs_pf, busy_pf, active_pf = [], [], []
        starts = sorted((s, e, i) for i, (s, e) in marks.items())
        import bisect
        keys = [s for s, _, _ in starts]
        cnt = defaultdict(int)
        busy = defaultdict(float)
        act = defaultdict(set)
        names_w = defaultdict(int)
        for wi, t in enumerate(workers):
            for s in t["spans"]:
                k = bisect.bisect_right(keys, s["s"]) - 1
                if k < 0:
                    continue
                fs, fe, fi = starts[k]
                if s["s"] > fe:
                    continue
                cnt[fi] += 1
                busy[fi] += (s["e"] - s["s"]) / 1e6  # timestamps are ns
                act[fi].add(wi)
                names_w[s["n"]] += 1
        for i in marks:
            jobs_pf.append(cnt[i]); busy_pf.append(busy[i]); active_pf.append(len(act[i]))
        disp = [a.get("RenderExtractor::extract", 0.0) + a.get("FrustumCuller::cull", 0.0) for _, a, _ in per]
        out["workers"] = len(workers)
        out["jobs/frame p50 (max)"] = f"{pct(jobs_pf, 50):.0f} ({max(jobs_pf)})"
        out["job names (total)"] = dict(names_w)
        out["worker busy ms/frame (sum over workers) p50/p90"] = f"{pct(busy_pf, 50):.4f} / {pct(busy_pf, 90):.4f}"
        out["workers touched/frame p50"] = pct(active_pf, 50)
        out["main-thread extract+cull ms/frame p50/p90"] = f"{pct(disp, 50):.4f} / {pct(disp, 90):.4f}"
        durs = [(s["e"] - s["s"]) / 1e6 for t in workers for s in t["spans"]]
        out["job duration ms p50/p90/max"] = f"{pct(durs, 50):.4f} / {pct(durs, 90):.4f} / {max(durs):.4f}"

    # Allocation counters (only present with the alloc_probe build hook).
    ac = [f["stats"].get("allocsMain") for f in frames if "allocsMain" in f.get("stats", {})]
    if ac:
        for key in ("allocsMain", "allocBytesMain", "allocsOther", "freesMain", "fileOpsMain", "fileOpsOther"):
            v = [f["stats"].get(key, 0) for f in frames]
            out[f"{key}/frame p50/p90/max"] = f"{pct(v, 50):.0f} / {pct(v, 90):.0f} / {max(v):.0f}"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--stall", type=float, default=5.0)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    res = [analyse(p, a.stall) for p in a.dumps]
    if a.json:
        json.dump(res, sys.stdout, indent=1)
        return
    for r in res:
        print(f"== {r['dump']}")
        for k, v in r.items():
            if k == "dump":
                continue
            if isinstance(v, (dict, list)):
                print(f"  {k}:")
                for x in (v.items() if isinstance(v, dict) else v):
                    print(f"     {x}")
            else:
                print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
