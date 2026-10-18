#!/usr/bin/env python3
"""Run physics_load_bench over the matrix behind docs/physics-threading-baseline.md and print markdown tables.

  scripts/perf/physics_load_bench.sh build            # once
  python3 scripts/perf/physics_load_sweep.py [--bench /tmp/he178/physics_load_bench] [--only bodies|workers|listener|chars|rays]

Every row is one process (fresh Jolt, fresh allocator). Numbers are per fixed step (dt = 1/60), measured after a
warm-up, so "p50 / p95 / max" are milliseconds of one PhysicsSystem::Update (or of the whole character / ray batch).
Check `uptime` and the power state before you trust a run: the Hive shares the machine.
"""
import argparse
import re
import subprocess
import sys

def run(bench, **kw):
    args = [bench]
    for k, v in kw.items():
        k = "--" + k.replace("_", "-")
        if v is True:
            args.append(k)
        elif v is not False and v is not None:
            args += [k, str(v)]
    out = subprocess.run(args, capture_output=True, text=True, check=False).stdout
    r = {"raw": out}
    m = re.search(r"Update ms\s+p50 ([\d.]+)\s+p95 ([\d.]+)\s+max ([\d.]+)", out)
    r["upd"] = tuple(float(x) for x in m.groups()) if m else None
    m = re.search(r"ExtendedUpdate ms\(all (\d+) chars\) p50 ([\d.]+)\s+p95 ([\d.]+)\s+max ([\d.]+)\s+-> ([\d.]+) us/char", out)
    r["chars"] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))) if m else None
    m = re.search(r"CastRay ms\((\d+) rays\) p50 ([\d.]+)\s+p95 ([\d.]+)\s+max ([\d.]+)\s+-> ([\d.]+) us/ray", out)
    r["rays"] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))) if m else None
    m = re.search(r"total physics ms p50 ([\d.]+) p95 ([\d.]+) max ([\d.]+)", out)
    r["total"] = tuple(float(x) for x in m.groups()) if m else None
    m = re.search(r"temp peak ([\d.]+) MB", out)
    r["temp"] = float(m.group(1)) if m else None
    m = re.search(r"stepsWithErrors=(\d+) flags=(.*)", out)
    r["err"] = (int(m.group(1)), m.group(2).strip()) if m else (0, "")
    m = re.search(r"all threads: \d+ ms cpu = ([\d.]+) cores", out)
    r["cores"] = float(m.group(1)) if m else None
    m = re.search(r"active bodies at end (\d+)", out)
    r["activeEnd"] = int(m.group(1)) if m else None
    m = re.search(r"active bodies max (\d+)", out)
    r["active"] = int(m.group(1)) if m else None
    m = re.search(r"contacts added (\d+) removed (\d+) active-now (\d+)", out)
    r["contacts"] = tuple(int(x) for x in m.groups()) if m else None
    return r

def f3(t, i=None):
    return "-" if t is None else ("%.2f / %.2f / %.2f" % t[:3] if i is None else "%.3f" % t[i])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default="/tmp/he178/physics_load_bench")
    ap.add_argument("--only", default="")
    ap.add_argument("--steps", type=int, default=240)
    ap.add_argument("--warmup", type=int, default=90)
    a = ap.parse_args()
    want = lambda k: not a.only or a.only == k
    common = dict(steps=a.steps, warmup=a.warmup)

    if want("bodies"):
        print("\n### Update time by body count (4 workers = the engine default on 10 cores, listener = HE's)\n")
        print("| scene | bodies | Update ms p50 / p95 / max | cores busy | temp peak MB | Jolt update errors |")
        print("|---|---:|---|---:|---:|---|")
        for scene, ns, extra in (("bounce", (1000, 5000, 20000, 50000), {}), ("pile", (1000, 2000, 5000, 10000, 20000), {})):
            for n in ns:
                r = run(a.bench, scene=scene, bodies=n, **common, **extra)
                e = r["err"]
                print("| %s | %d | %s | %s | %s | %s |" % (scene, n, f3(r["upd"]), r["cores"], r["temp"],
                      "none" if e[0] == 0 else "%d/%d steps: %s" % (e[0], a.steps, e[1])))
        r = run(a.bench, scene="pile", bodies=5000, sleeping=True, steps=a.steps, warmup=900)
        print("| pile, sleeping allowed (15 s settle) | 5000 | %s | %s | %s | none; %s of 5000 still active |" % (f3(r["upd"]), r["cores"], r["temp"], r["activeEnd"]))

    if want("workers"):
        print("\n### Worker sweep (Jolt worker threads besides the calling thread; engine default is clamp(hw/2-1, 0, 4) = 4)\n")
        print("| scene | bodies | workers | Update ms p50 / p95 / max | cores busy |")
        print("|---|---:|---:|---|---:|")
        for scene, n in (("bounce", 20000), ("pile", 5000)):
            for w in (0, 1, 2, 3, 4, 6, 9):
                r = run(a.bench, scene=scene, bodies=n, workers=w, **common)
                print("| %s | %d | %d | %s | %s |" % (scene, n, w, f3(r["upd"]), r["cores"]))

    if want("listener"):
        print("\n### Contact listener cost (HE's mutex-guarded listener vs none), 4 workers\n")
        print("| scene | bodies | listener | Update ms p50 / p95 / max | contact events added / removed |")
        print("|---|---:|---|---|---|")
        for scene, n in (("bounce", 20000), ("pile", 5000)):
            for lis in ("he", "none"):
                r = run(a.bench, scene=scene, bodies=n, listener=lis, **common)
                c = r["contacts"]
                print("| %s | %d | %s | %s | %s |" % (scene, n, lis, f3(r["upd"]), "-" if not c else "%d / %d" % c[:2]))

    if want("contend"):
        print("\n### Jolt next to busy engine-pool threads (K spinning threads = a globalPool with every job running), 20000 bouncing bodies\n")
        print("| workers | K busy threads | Update ms p50 / p95 / max |")
        print("|---:|---:|---|")
        for w in (4, 2):
            for k in (0, 2, 5, 10):
                r = run(a.bench, scene="bounce", bodies=20000, workers=w, contend=k, **common)
                print("| %d | %d | %s |" % (w, k, f3(r["upd"])))

    if want("chars"):
        print("\n### Character ExtendedUpdate (serial, after Update), 5000 bouncing bodies in the world\n")
        print("| characters | ExtendedUpdate ms p50 / p95 / max | us per character | Update ms p50 |")
        print("|---:|---|---:|---:|")
        for c in (0, 10, 100, 500, 1000):
            r = run(a.bench, scene="bounce", bodies=5000, characters=c, **common)
            print("| %d | %s | %s | %.2f |" % (c, f3(r["chars"]) if r["chars"] else "-", "%.1f" % r["chars"][3] if r["chars"] else "-", r["upd"][0]))

    if want("rays"):
        print("\n### Raycasts (CastRay closest hit, from the calling thread, 40 m down through the field), 20000 bouncing bodies\n")
        print("| rays per step | batch ms p50 / p95 / max | us per ray |")
        print("|---:|---|---:|")
        for n in (100, 1000, 5000):
            r = run(a.bench, scene="bounce", bodies=20000, rays=n, steps=120, warmup=60)
            print("| %d | %s | %.2f |" % (n, f3(r["rays"]), r["rays"][3]))

if __name__ == "__main__":
    sys.exit(main())
