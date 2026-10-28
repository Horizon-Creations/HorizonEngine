#!/usr/bin/env python3
"""Summarise one profile_*.json (or a *.profile.json) the way he_perf_capture does: FPS, CPU/GPU p50, per-pass GPU p50, top CPU scopes.
   python3 -I sumdump.py <dump.json> [--top N]"""
import importlib.util, sys
from pathlib import Path
here = Path(__file__).resolve()
repo = Path(sys.argv[0]).resolve()
spec = importlib.util.spec_from_file_location("pc", sys.argv[sys.argv.index("--repo") + 1] + "/scripts/he_perf_capture.py")
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)
dump = Path(sys.argv[1]); top = 12
if "--top" in sys.argv: top = int(sys.argv[sys.argv.index("--top") + 1])
s = pc.summarise(dump)
print(dump.name, s["session"].get("width"), "x", s["session"].get("height"), s["session"].get("backend"))
print("fps %.1f  delta p50 %.2f  cpu p50 %.2f  gpu p50 %s" % (s["fps"]["avg"], s["deltaMs"]["p50"], s["cpuMs"]["p50"], (s.get("gpuMs") or {}).get("p50")))
print("modes", s.get("gpuTimingModes"), "counters", {k: s["counters_midframe"].get(k) for k in ("draws", "tris", "visible", "total", "lights", "entities")})
for k, v in sorted((s.get("gpuPassPercentiles") or {}).items(), key=lambda kv: -kv[1]["p50"]):
    print("  GPU %-28s p50 %.2f p10 %.2f p90 %.2f" % (k, v["p50"], v["p10"], v["p90"]))
for k, v in sorted(s["cpuScopePerFrame"].items(), key=lambda kv: -kv[1]["p50"])[:top]:
    print("  CPU %-44s p50 %.2f p90 %.2f" % (k, v["p50"], v["p90"]))
