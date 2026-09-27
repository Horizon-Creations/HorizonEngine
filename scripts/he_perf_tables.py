#!/usr/bin/env python3
"""Markdown tables from the *.summary.json files he_perf_capture.py writes.

  python3 scripts/he_perf_tables.py docs/perf-audit/raw > tables.md

Runs whose label starts with c0..c4 were taken with the display asleep (see
docs/perf-audit/baseline-2026-09-27.md); everything else with it awake.
"""
import glob
import json
import os
import sys

R = sys.argv[1] if len(sys.argv) > 1 else "docs/perf-audit/raw"
PASSES = ["Shadow", "GIAccel", "GIShadow", "GIProbes", "SSAO", "Scene", "Bloom", "Tonemap", "Present"]


def load(label):
    with open(f"{R}/{label}.summary.json") as fh:
        return json.load(fh)


def f(x, n=1):
    return "–" if x is None else f"{x:.{n}f}"


runs = sorted(os.path.basename(p)[: -len(".summary.json")] for p in glob.glob(f"{R}/*.summary.json"))
detailed = [L for L in runs if (load(L).get("gpuTimingModes") or {}).get("detailed")]
normal = [L for L in runs if L not in detailed]

print("### Tabelle A: Frame-Takt (Normalmodus, 600 Frames nach 300 Frames Warmup)\n")
print("| Lauf | Display | vsync | FPS Ø | 1%-Low FPS | delta p50 / p95 ms | CPU-Frame p50 / p95 ms "
      "| davon Metal::NextDrawable p50 / p90 ms | CPU-Frame ohne NextDrawable p50 / p90 ms "
      "| GPU-Spanne p50 ms ¹ | RSS max MB |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for L in normal:
    s = load(L)
    nd = (s.get("cpuScopePerFrame") or {}).get("Metal::NextDrawable")
    rest = s.get("cpuMinusNextDrawable")
    disp = "schläft" if L[:2] in ("c0", "c1", "c2", "c3", "c4") else "wach"
    rss = (s.get("rssMB") or {}).get("max")
    print(f"| {L} | {disp} | {'an' if s['session']['vsync'] else 'aus'} | {f(s['fps']['avg'])} "
          f"| {f(s['fps']['low1Percent'])} | {f(s['deltaMs']['p50'], 2)} / {f(s['deltaMs']['p95'], 2)} "
          f"| {f(s['cpuMs']['p50'], 2)} / {f(s['cpuMs']['p95'], 2)} "
          f"| {(f(nd['p50'], 2) + ' / ' + f(nd['p90'], 2)) if nd else 'n/a (vor dem Scope-Split)'} "
          f"| {(f(rest['p50'], 2) + ' / ' + f(rest['p90'], 2)) if (rest and nd) else '–'} "
          f"| {f((s.get('gpuMs') or {}).get('p50'), 2)} | {f(rss, 0)} |")
print("\n¹ Whole-Frame-Spanne GPUStartTime→GPUEndTime des einen Command-Buffers im Normalmodus. "
      "Gemessen unter Fremdlast (pid 72986), enthält also auch Zeit, in der die GPU fremde Arbeit ausführt.\n")

print("### Tabelle B: Exklusive GPU-Zeit pro Pass (Detailed-Capture), ms als min / p10 / p50 über 600 Frames\n")
print("| Lauf | " + " | ".join(PASSES) + " | gpuMs p50 | Σ Pässe / gpuMs |")
print("|---|" + "---|" * (len(PASSES) + 2))
for L in detailed:
    s = load(L)
    pp = s.get("gpuPassPercentiles") or {}
    cells = []
    for p in PASSES:
        v = pp.get(p)
        cells.append("–" if not v else f"{v['min']:.2f} / {v['p10']:.2f} / {v['p50']:.2f}")
    print(f"| {L} | " + " | ".join(cells)
          + f" | {f(s['gpuMs']['p50'], 2)} | {f(s.get('detailedSumOverGpu_median'), 3)} |")

print("\n### Tabelle C: CPU-Scopes pro Frame (Summe je Frame; p50 / p90 ms)\n")
wanted = ["Render", "Metal::NextDrawable", "Metal::EncodeShadowMap", "Metal::EncodeCloudShadow",
          "Metal::EncodeSSAO", "Metal::EncodeScene", "Metal::Overlay", "Metal::Commit", "OnRender",
          "PollEvents", "SceneSystemsTick", "RenderExtractor::extract", "FrustumCuller::cull",
          "Terrain", "LOD", "Weather", "EnvironmentPush", "GameLogicTick", "SwapBuffers"]
cols = [L for L in normal if L.startswith(("L1", "L2", "L4", "L6", "e1"))][:6]
print("| Scope | " + " | ".join(cols) + " |")
print("|---|" + "---|" * len(cols))
for sc in wanted:
    row = []
    for L in cols:
        v = (load(L).get("cpuScopePerFrame") or {}).get(sc)
        row.append("–" if not v else f"{v['p50']:.3f} / {v['p90']:.3f}")
    print(f"| {sc} | " + " | ".join(row) + " |")

print("\n### Tabelle D: Zähler (Frame aus der Mitte der Aufnahme)\n")
print("| Lauf | Draws | Dreiecke | sichtbar/gesamt | Entities | Lichter | Partikel | GPU-Partikel |")
print("|---|---|---|---|---|---|---|---|")
for L in ("c1-base-vsyncoff-run1", "L1-landscape-vsyncoff-run1", "L4-skyonly-vsyncoff",
          "L6-landscape-noclouds-vsyncoff"):
    if L not in runs:
        continue
    c = load(L)["counters_midframe"]
    print(f"| {L} | {c['draws']} | {c['tris']} | {c['visible']}/{c['total']} | {c['entities']} "
          f"| {c['lights']} | {c['particles']} | {c['gpuParticles']} |")
