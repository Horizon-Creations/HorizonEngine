#!/usr/bin/env python3
"""Run one physics load scene in the real HorizonGame loop and print what the engine profiler and the thread probe saw.

  clang -O2 -dynamiclib scripts/perf/thread_cpu_probe.c -o /tmp/he178/thread_cpu_probe.dylib
  python3 scripts/perf/physics_load_run.py --runtime /tmp/he178/gamebase --layout dense --bodies 5000 [--characters 100]
          [--frames 600] [--warmup 300] [--keep-vsync] [--workdir /tmp/he178]

Per run: scripts/perf/physics_load_scene.py builds <workdir>/g_<layout><bodies>c<chars>, the game runs hidden with
HE_PROFILE_CAPTURE (scripted F9) and HE_PROFILE_COUNTERS=0 (the --no-counters of he_perf_capture.py), the thread probe
is injected, and the dump is read. Frames are the engine's: with vsync off a hidden window runs at 100+ FPS while physics
steps at a fixed 1/60 s, so some frames carry no step (PhysicsStep absent) and a few carry two. Both views are printed:
per FRAME (what a frame costs) and per frame WITH a step (what a step costs).
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))] if v else float("nan")


def thread_report(tsv, window_ms, end_ms):
    """CPU per thread over [end_ms - window, end_ms] on the probe's clock (the capture is the stretch before the dump)."""
    rows = {}
    last = 0.0
    for line in open(tsv):
        t, tid, tag, name, cpu = line.rstrip("\n").split("\t")
        rows.setdefault(tid, []).append((float(t), tag, name, float(cpu)))
        last = max(last, float(t))
    t1, t0 = min(end_ms, last), min(end_ms, last) - window_ms
    per = []
    for tid, s in rows.items():
        def at(x):
            best = None
            for t, _, _, c in s:
                if t <= x:
                    best = c
            return best
        c1, c0 = at(t1), at(t0)
        if c1 is None:
            continue
        c0 = 0.0 if c0 is None else c0
        per.append((tid, s[-1][1], s[-1][2], c1 - c0))
    return per, window_ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", type=Path, default=Path("/tmp/he178/gamebase"))
    ap.add_argument("--workdir", type=Path, default=Path("/tmp/he178"))
    ap.add_argument("--layout", default="dense")
    ap.add_argument("--bodies", type=int, default=5000)
    ap.add_argument("--characters", type=int, default=0)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--keep-vsync", action="store_true")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--probe", default="/tmp/he178/thread_cpu_probe.dylib")
    ap.add_argument("--label", default="")
    ap.add_argument("--reanalyze", action="store_true", help="skip building and running, read the dump and thread TSV already in the game dir")
    a = ap.parse_args()

    out = a.workdir / f"g_{a.layout}{a.bodies}c{a.characters}"
    load0, wall = os.getloadavg()[0], 0.0
    if not a.reanalyze:
        subprocess.run([sys.executable, str(HERE / "physics_load_scene.py"), "--runtime", str(a.runtime), "--out", str(out),
                        "--layout", a.layout, "--bodies", str(a.bodies), "--characters", str(a.characters)],
                       check=True, stdout=subprocess.DEVNULL)
        env = dict(os.environ, HE_PROFILE_CAPTURE=str(a.frames), HE_PROFILE_WARMUP=str(a.warmup), HE_PROFILE_COUNTERS="0",
                   HE_HIDDEN_WINDOW="1", HE_THREAD_PROBE_OUT=str(out / "threads.tsv"), DYLD_INSERT_LIBRARIES=a.probe,
                   HE_PROFILE_NOTE=a.label or f"{a.layout} {a.bodies} bodies {a.characters} characters")
        if a.keep_vsync:
            env["HE_PROFILE_VSYNC"] = "keep"
        t0 = time.time()
        p = subprocess.Popen([str(out / "HorizonGame")], cwd=out, env=env, stdout=open(out / "run.log", "w"), stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=a.timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            print("TIMEOUT - killed; see", out / "run.log")
        wall = time.time() - t0
    dumps = sorted((out / "dumps").glob("profile_*.json"), key=lambda x: x.stat().st_mtime) if (out / "dumps").exists() else []
    if not dumps:
        print("no dump; tail of the log:\n" + "".join(open(out / "run.log").readlines()[-15:]))
        return 1
    d = json.load(open(dumps[-1]))
    fr = d["frames"]
    cpu = [f["cpuMs"] for f in fr]
    def scope(name):
        return [sum(c["ms"] for c in f["cpu"] if c["n"] == name) for f in fr]
    phys = scope("PhysicsStep")
    disp = scope("CollisionDispatch")
    # How many fixed steps each frame ran: the dump does not say, so replay HE::advanceFixedSteps (FixedStep.h) on the
    # frames' own deltaMs. An inference - the first frame after the warm-up has an unknown accumulator - which is why the
    # step-normalised numbers below use frames inferred to have EXACTLY one step, where an off-by-one cannot hide.
    acc, steps = 0.0, []
    for f in fr:
        acc += f.get("deltaMs", 0.0) / 1000.0
        n = 0
        while acc >= 1.0 / 60.0 and n < 5:
            acc -= 1.0 / 60.0
            n += 1
        if acc > 1.0 / 60.0:
            acc = 0.0
        steps.append(n)
    one = [p_ for p_, n in zip(phys, steps) if n == 1 and p_ > 0]
    per_step = [p_ / n for p_, n in zip(phys, steps) if n >= 1 and p_ > 0]
    print(f"## {a.layout} {a.bodies} bodies, {a.characters} characters - {len(fr)} frames, wall {wall:.0f} s, load {load0:.1f} -> {os.getloadavg()[0]:.1f}")
    print(f"  frame CPU ms      p50 {pct(cpu,50):.2f} p95 {pct(cpu,95):.2f} max {max(cpu):.2f} | frame delta p50 {pct([f['deltaMs'] for f in fr],50):.1f} ms")
    print(f"  PhysicsStep/frame p50 {pct(phys,50):.2f} p95 {pct(phys,95):.2f} max {max(phys):.2f} ms | inferred steps/frame mean {statistics.mean(steps):.2f} (0:{steps.count(0)} 1:{steps.count(1)} 2:{steps.count(2)} 3+:{sum(1 for n in steps if n>=3)})")
    print(f"  per STEP (PhysicsStep / inferred steps) p50 {pct(per_step,50):.2f} p95 {pct(per_step,95):.2f} max {max(per_step) if per_step else 0:.2f} | one-step frames: p50 {pct(one,50):.2f} p95 {pct(one,95):.2f} max {max(one) if one else 0:.2f} ms (n={len(one)})")
    print(f"  CollisionDispatch p50 {pct(disp,50):.3f} max {max(disp):.3f} ms (nested inside PhysicsStep)")
    scopes = {}
    for f in fr:
        for c in f["cpu"]:
            scopes.setdefault(c["n"], []).append(c["ms"])
    tot = sorted(((sum(v) / len(fr), k) for k, v in scopes.items()), reverse=True)[:10]
    print("  top scopes (mean ms per frame, summed over calls): " + ", ".join(f"{k} {v:.2f}" for v, k in tot))
    tsv = out / "threads.tsv"
    if tsv.exists():
        # the capture ends when the dump is written; the probe's clock starts at the TSV's birth
        window = sum(f["deltaMs"] for f in fr)
        end_ms = (dumps[-1].stat().st_mtime - tsv.stat().st_birthtime) * 1000.0
        per, w = thread_report(tsv, window, end_ms)
        print(f"  thread CPU over the last {w/1000:.1f} s before exit (utilisation = cpu / window):")
        agg = {}
        for tid, tag, name, c in per:
            key = (tag, name if name else "")
            agg.setdefault(key, []).append(c)
        for (tag, name), cs in sorted(agg.items(), key=lambda kv: -sum(kv[1])):
            if sum(cs) < 5:
                continue
            print(f"    {tag:<16} {name:<12} threads {len(cs):2d}  sum {sum(cs):8.0f} ms  = {sum(cs)/w:5.2f} cores   per thread: " + " ".join(f"{100*c/w:3.0f}%" for c in sorted(cs, reverse=True)[:8]))
    print("  log:", out / "run.log", "| dump:", dumps[-1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
