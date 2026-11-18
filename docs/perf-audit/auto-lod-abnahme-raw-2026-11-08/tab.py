import json, sys, glob, re, os
raw = "/private/tmp/auto-lod-abnahme/raw"
print(f"{'run':16s} {'fps':>5s} {'p50dt':>6s} {'low1%':>5s} {'gpu':>6s} {'Scene':>6s} {'Shadow':>6s} {'SSAO':>5s} {'Tonem':>5s} {'tris(M)':>8s} {'draws':>5s} {'cpuMs':>6s} {'casters':>7s} {'ao':>4s}")
for f in sorted(glob.glob(f"{raw}/*.summary.json")):
    s = json.load(open(f)); lab = s["runner"]["label"]
    pp = s.get("gpuPassPercentiles") or {}
    g = lambda k: pp.get(k, {}).get("p50", float("nan"))
    c = s["counters_midframe"]
    try: log = open(f"{raw}/{lab}.log", errors="replace").read()
    except: log = ""
    m = re.search(r"shadow pass instanced \(first run: (\d+) casters", log)
    a = re.search(r"SSAO pre-pass instanced \(first run: (\d+) objects", log)
    print(f"{lab:16s} {s['fps']['avg']:5.2f} {s['deltaMs']['p50']:6.1f} {s['fps']['low1Percent']:5.1f} {s['gpuMs']['p50']:6.1f} {g('Scene'):6.1f} {g('Shadow'):6.1f} {g('SSAO'):5.1f} {g('Tonemap'):5.3f} "
          f"{c['tris']/1e6:8.2f} {c['draws']:5d} {s['cpuMs']['p50']:6.1f} {m.group(1) if m else '-':>7s} {a.group(1) if a else '-':>4s}")
