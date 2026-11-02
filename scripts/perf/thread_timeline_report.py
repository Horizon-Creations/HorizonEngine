#!/usr/bin/env python3
"""Thread-timeline report for a profiler capture dump (HE_PROFILE_CAPTURE, v3 `threads` lanes).

  python3 scripts/perf/thread_timeline_report.py <dump.profile.json> [--top 14] [--hitch-ms 25] [--json out.json]

Answers "how well are the cores used" from the dump alone, no xctrace:
  * per lane: busy time (union of depth-0 spans) as a share of the capture window, spans, the work names it ran
  * the main thread's frame split into WAIT (WaitForFrame = vsync / NextDrawable) and WORK, and the work by depth-0/1 scope
  * frames whose WORK exceeds --hitch-ms, with the scope that ate it, and what the pool lanes did in the same window
  * pool occupancy: for every 1 ms bin of the capture, how many lanes (other than main) had a span open

The dump holds at most 120000 spans (the cutoff is a shared timestamp, see EngineProfiler.cpp); the window used
here is the part of the capture that every lane covers.
"""
import argparse
import json
import statistics
from collections import defaultdict


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))] if v else 0.0


def union_ms(spans):
    """Total time covered by [s, e) intervals in ns, as ms."""
    tot, cur_s, cur_e = 0, None, None
    for s, e in sorted(spans):
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                tot += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    if cur_e is not None:
        tot += cur_e - cur_s
    return tot / 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--top", type=int, default=14)
    ap.add_argument("--hitch-ms", type=float, default=25.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    d = json.load(open(a.dump))
    lanes = d["threads"]
    marks = d.get("frameMarks", [])
    main_lane = next(l for l in lanes if l["main"])
    mspans = main_lane["spans"]
    t0 = min(s["s"] for l in lanes for s in l["spans"])
    t1 = max(s["e"] for l in lanes for s in l["spans"])
    # the window every lane covers = the dump's shared cutoff, the main lane's last span is its end
    win = (t1 - t0) / 1e6
    out = {"dump": a.dump, "windowMs": win, "lanes": []}
    print(f"{a.dump}\n  window {win:.0f} ms, {len(lanes)} lane(s), {sum(len(l['spans']) for l in lanes)} spans")

    # ---- lanes -------------------------------------------------------------
    print("\n== lanes (busy = union of depth-0 spans)")
    for l in lanes:
        d0 = [(s["s"], s["e"]) for s in l["spans"] if s["d"] == 0]
        busy = union_ms(d0)
        names = defaultdict(lambda: [0, 0.0])
        for s in l["spans"]:
            if s["d"] == 0:
                n = names[s["n"]]
                n[0] += 1
                n[1] += (s["e"] - s["s"]) / 1e6
        top = sorted(names.items(), key=lambda kv: -kv[1][1])[:6]
        print(f"  {l['thread']:<14} spans={len(l['spans']):>6} busy={busy:9.1f} ms ({100 * busy / win:5.1f}% of window)  "
              + ", ".join(f"{k} x{v[0]} {v[1]:.1f}ms" for k, v in top))
        out["lanes"].append({"thread": l["thread"], "main": l["main"], "busyMs": busy,
                             "busyPct": 100 * busy / win, "spans": len(l["spans"]),
                             "top": [{"n": k, "calls": v[0], "ms": v[1]} for k, v in top]})

    # ---- main thread: wait vs work ----------------------------------------
    # The editor loop waits in a depth-0 WaitForFrame (NextDrawable nested inside it); HorizonGame has no such
    # scope and waits in Metal::NextDrawable inside Render. Either way that is time the thread is blocked.
    wait = [s for s in mspans if s["d"] == 0 and s["n"] == "WaitForFrame"]
    if not wait:
        wait = [s for s in mspans if s["n"] == "Metal::NextDrawable"]
    wait_ms = sum((s["e"] - s["s"]) for s in wait) / 1e6
    d0 = [s for s in mspans if s["d"] == 0]
    busy_ms = union_ms([(s["s"], s["e"]) for s in d0])
    work_ms = busy_ms - wait_ms
    print(f"\n== main thread: busy {busy_ms:.0f} ms of {win:.0f} = {100 * busy_ms / win:.1f}%; "
          f"WaitForFrame {wait_ms:.0f} ms ({100 * wait_ms / win:.1f}%); work {work_ms:.0f} ms ({100 * work_ms / win:.1f}%)")
    per = defaultdict(list)
    for lvl in (0, 1):
        bys = defaultdict(float)
        for s in mspans:
            if s["d"] == lvl:
                bys[s["n"]] += (s["e"] - s["s"]) / 1e6
        per[lvl] = bys
    if not any(x["d"] == 0 and x["n"] == "WaitForFrame" for x in mspans) and "Render" in per[0]:
        per[0]["Render"] -= per[1].get("Metal::NextDrawable", 0.0)   # the game's wait sits inside Render
    print("  depth-0 scopes (total ms, share of work):")
    for n, ms in sorted(per[0].items(), key=lambda kv: -kv[1])[:a.top]:
        if n != "WaitForFrame":
            print(f"    {n:<34} {ms:9.1f} ms  {100 * ms / max(work_ms, 1e-9):5.1f}% of work")
    print("  depth-1 scopes (total ms):")
    for n, ms in sorted(per[1].items(), key=lambda kv: -kv[1])[:a.top]:
        print(f"    {n:<34} {ms:9.1f} ms")
    out["main"] = {"busyMs": busy_ms, "waitMs": wait_ms, "workMs": work_ms,
                   "depth0": dict(per[0]), "depth1": dict(per[1])}

    # ---- per-frame work ----------------------------------------------------
    frames = d["frames"]
    work = []
    for f in frames:
        w = 0.0
        has_wff = any(sc["d"] == 0 and sc["n"] == "WaitForFrame" for sc in f["cpu"])
        for sc in f["cpu"]:
            if sc["d"] == 0 and sc["n"] != "WaitForFrame":
                w += sc["ms"]
            elif sc["n"] == "Metal::NextDrawable" and not has_wff:
                w -= sc["ms"]
        work.append(w)
    print(f"\n== per-frame main-thread WORK (depth-0 scopes minus WaitForFrame / NextDrawable): "
          f"p50 {pct(work, 50):.2f} p90 {pct(work, 90):.2f} p99 {pct(work, 99):.2f} max {max(work):.2f} ms over {len(work)} frames; "
          f"dump cpuMs p50 {pct([f['cpuMs'] for f in frames], 50):.2f}")
    hitches = sorted(((w, i) for i, w in enumerate(work) if w >= a.hitch_ms), reverse=True)[:12]
    out["frameWork"] = {"p50": pct(work, 50), "p90": pct(work, 90), "p99": pct(work, 99), "max": max(work), "n": len(work)}
    out["hitches"] = []
    if hitches:
        print(f"  frames with work >= {a.hitch_ms} ms: {sum(1 for w in work if w >= a.hitch_ms)}; worst:")
    for w, i in hitches:
        f = frames[i]
        top = sorted(((sc["ms"], sc["n"], sc["d"]) for sc in f["cpu"] if sc["d"] <= 1 and sc["n"] not in ("WaitForFrame", "Metal::NextDrawable")), reverse=True)[:4]
        print(f"    frame {f['i']:>4} work {w:7.2f} ms  " + "; ".join(f"{n}(d{dd}) {ms:.1f}" for ms, n, dd in top))
        out["hitches"].append({"frame": f["i"], "workMs": w, "top": [{"n": n, "ms": ms} for ms, n, _ in top]})

    # ---- pool occupancy ----------------------------------------------------
    pool = [l for l in lanes if not l["main"]]
    if pool:
        nb = int(win) + 1
        occ = [0] * nb
        for l in pool:
            seen = set()
            for s in l["spans"]:
                if s["d"] != 0:
                    continue
                for b in range(int((s["s"] - t0) / 1e6), min(nb - 1, int((s["e"] - t0) / 1e6)) + 1):
                    seen.add(b)
            for b in seen:
                occ[b] += 1
        hist = defaultdict(int)
        for o in occ:
            hist[o] += 1
        print("\n== pool occupancy (1 ms bins; how many non-main lanes had work open): "
              + ", ".join(f"{k}:{100 * v / nb:.1f}%" for k, v in sorted(hist.items())))
        out["poolOccupancyPct"] = {str(k): 100 * v / nb for k, v in sorted(hist.items())}
    else:
        print("\n== no worker lane in the dump: no pool task ran a profiler scope during the capture")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
