#!/usr/bin/env python3
"""Markdown tables for the Auto Landscape 2.0 audit from the runs of autoland_matrix.py / he_perf_capture.py.

  python3 scripts/perf/autoland_table.py docs/perf-audit/raw-auto-landscape-2 > tables.md

Reads <group>-<cell>-r<k>.summary.json (+ .scopes.json, the per-frame p50 of every CPU scope) and prints, per
group, one row per cell: the MEDIAN over the repetitions of each number, with the min..max of the GPU time next to
it so a reader sees whether the difference is larger than the spread. Cells are shown against the group's
baseline cell (the first one of the group, "off" / "V1") as a difference in ms and per cent.

Columns: GPU = whole-frame GPU time p50 (Metal command-buffer span, `gpuMs`), Render = CPU time of the scope
`Render` per frame (p50), Extract = `ExtractFoliage` + `Foliage` (the tick) per frame p50, Shadow enc = Metal::EncodeShadowMap p50.
The screen was locked in every run, so frame delta and FPS carry display idle (NextDrawable p90 ~ 60 ms): GPU and
Render are the comparison numbers, not FPS (docs/perf-audit/foliage-baseline-2026-10-10.md section 2).
"""
import glob
import json
import os
import re
import statistics
import sys
from collections import defaultdict

R = sys.argv[1] if len(sys.argv) > 1 else "docs/perf-audit/raw-auto-landscape-2"
ORDER = {"mesh": ["s128-off", "s128-x1", "s128-x4", "s128-x16", "s128-x4ns", "s512-off", "s512-x1", "s512-x4", "s512-x4ns"],
         "var": ["s128-off-V1", "s128-off-V3", "s512-off-V1", "s512-off-V3", "s128-x4-V1", "s128-x4-V3"]}


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def f(x, n=2):
    return "–" if x is None else f"{x:.{n}f}"


def scope(sc, name):
    s = (sc or {}).get("scopes", {}).get(name)
    return s["p50"] if s else None


def load(label):
    with open(f"{R}/{label}.summary.json") as fh:
        s = json.load(fh)
    try:
        with open(f"{R}/{label}.scopes.json") as fh:
            sc = json.load(fh)
    except OSError:
        sc = None
    return s, sc


def metrics(s, sc):
    c = s.get("counters_midframe") or {}
    ext = [scope(sc, "ExtractFoliage"), scope(sc, "Foliage")]
    return {
        "gpu": (s.get("gpuMs") or {}).get("p50"),
        "render": scope(sc, "Render"),
        "extract": sum(x for x in ext if x is not None) if any(x is not None for x in ext) else None,
        "shadow": scope(sc, "Metal::EncodeShadowMap"),
        "scene": scope(sc, "Metal::EncodeScene"),
        "cpu": (s.get("cpuMs") or {}).get("p50"),
        "fps": (s.get("fps") or {}).get("avg"),
        "draws": c.get("draws"), "tris": c.get("tris"), "visible": c.get("visible"),
        "rss_max": (s.get("rssMB") or {}).get("max"), "rss_last": (s.get("rssMB") or {}).get("last"),
    }


runs = defaultdict(lambda: defaultdict(list))   # group -> cell -> [metrics]
for p in sorted(glob.glob(f"{R}/*.summary.json")):
    label = os.path.basename(p)[: -len(".summary.json")]
    m = re.fullmatch(r"(mesh|var|detail|mem|memgl)(2x)?-(.+)-r(\d+)", label)
    if not m:
        continue
    s, sc = load(label)
    runs[m.group(1) + (m.group(2) or '')][m.group(3)].append(metrics(s, sc))


def table(group, title):
    cells = [c for c in ORDER.get(group.rstrip("2x") if group.endswith("2x") else group, []) if c in runs[group]] or sorted(runs[group])
    if not cells:
        return
    print(f"### {title}\n")
    print("| Zelle | Läufe | GPU p50 ms (min..max) | Δ GPU ms / % | Render p50 ms | Δ Render ms | Extract+Tick p50 ms | Shadow enc p50 ms "
          "| Scene enc p50 ms | draws | tris | RSS max / last MB |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    base = None
    for c in cells:
        rs = runs[group][c]
        gpus = [r["gpu"] for r in rs if r["gpu"] is not None]
        mg, mr = med(gpus), med([r["render"] for r in rs])
        if base is None or c.endswith("-off") or c.endswith("-V1"):
            pass
        # the baseline of a row: same extent for "mesh" (sNNN-off), same scene for "var" (…-V1)
        key = c.split("-")[0]
        if group.startswith("mesh"):
            bname = f"{key}-off"
        elif group.startswith("var"):
            bname = c.rsplit("-", 1)[0] + "-V1"
        else:
            bname = None
        brs = runs[group].get(bname) if bname else None
        bg = med([r["gpu"] for r in brs]) if brs else None
        br = med([r["render"] for r in brs]) if brs else None
        dg = f"{mg - bg:+.2f} / {100 * (mg - bg) / bg:+.1f} %" if (mg is not None and bg and bname != c) else "Basis"
        dr = f"{mr - br:+.2f}" if (mr is not None and br is not None and bname != c) else "Basis"
        spread = f"{min(gpus):.2f}..{max(gpus):.2f}" if gpus else "–"
        print(f"| {c} | {len(rs)} | {f(mg)} ({spread}) | {dg} | {f(mr)} | {dr} | {f(med([r['extract'] for r in rs]), 3)} "
              f"| {f(med([r['shadow'] for r in rs]))} | {f(med([r['scene'] for r in rs]))} | {f(med([r['draws'] for r in rs]), 0)} "
              f"| {f(med([r['tris'] for r in rs]), 0)} | {f(med([r['rss_max'] for r in rs]), 0)} / {f(med([r['rss_last'] for r in rs]), 0)} |")
    print()


table("mesh", "Tabelle 1: Layer-Meshes aus / x1 / x4 / x16 (Median über die Wiederholungen)")
table("mesh2x", "Tabelle 1b: wie Tabelle 1, RenderScale 2 (GPU-bound: die GPU geht nie in den Leerlauf)")
table("var", "Tabelle 2: Texturvarianten V = 1 gegen V = 3 (Arrays aus 5 gegen 13 Slices, 2K)")
table("var2x", "Tabelle 2b: wie Tabelle 2, RenderScale 2")
table("mem", "Tabelle 4: Texturformate der Arrays (V = 3)")


# ── Pass breakdown of the --detailed runs (exclusive GPU time per pass) + foliage counters from the log ──
PASS_ROWS = ["Shadow", "SSAO", "Scene", "Bloom", "Tonemap", "Present"]


def foliage_line(label):
    try:
        lines = [l for l in open(f"{R}/{label}.log", errors="replace") if "FoliageExtract:" in l]
    except OSError:
        return None
    if not lines:
        return None
    m = re.search(r"(\d+) cluster\(s\) for (\d+) of (\d+) instance\(s\)", lines[-1])
    return m.groups() if m else None


def tick_ms(label):
    """One-off scatter + terrain cost: the 'tickWorld took X ms' warning of frame 3 (only logged above 16 ms)."""
    try:
        for l in open(f"{R}/{label}.log", errors="replace"):
            m = re.search(r"tickWorld took ([\d.]+) ms", l)
            if m:
                return float(m.group(1))
    except OSError:
        pass
    return None


def pass_table(group, title):
    by = defaultdict(list)
    for p in sorted(glob.glob(f"{R}/{group}-*-r*.summary.json")):
        label = os.path.basename(p)[: -len(".summary.json")]
        m = re.fullmatch(r"%s-(.+)-r(\d+)" % re.escape(group), label)
        if m:
            by[m.group(1)].append(label)
    if not by:
        return
    order = []
    for c in ORDER["mesh"] + ORDER["var"] + ["s128-off-V3", "s512-off-V3", "s128-x4-V3"]:
        if c in by and c not in order:
            order.append(c)
    order += [c for c in sorted(by) if c not in order]
    # baseline of a row, for the difference column of the Scene pass
    def scene_med(cell):
        return med([load(l)[0]["gpuPassPercentiles"]["Scene"]["p50"] for l in by[cell]]) if cell in by else None
    print(f"### {title}\n")
    print("| Zelle | Läufe | " + " | ".join(f"{p} p50 (p10)" for p in PASS_ROWS) + " | Scene p50 min..max | Δ Scene ms | GPU Σ Pässe p50 | Δ GPU ms | Cluster | Instanzen in Reichweite / gespeichert | tickWorld Frame 3 (einmalig) |")
    print("|---|---|" + "---|" * (len(PASS_ROWS) + 7))
    for cell in order:
        runs_ = [load(l)[0] for l in by[cell]]
        cols = []
        for p in PASS_ROWS:
            v = [r["gpuPassPercentiles"].get(p) for r in runs_ if r.get("gpuPassPercentiles")]
            v = [x for x in v if x]
            cols.append("–" if not v else f"{med([x['p50'] for x in v]):.2f} ({med([x['p10'] for x in v]):.2f})")
        sc_ = [r["gpuPassPercentiles"]["Scene"]["p50"] for r in runs_]
        key = cell.split("-")[0]
        bcell = cell.rsplit("-", 1)[0] if cell.endswith("-V3") else key + "-off"
        if cell.endswith("-V3"):
            bcell = bcell if bcell in by else None
        bs = scene_med(bcell) if bcell and bcell != cell else None
        gpu = med([r.get("gpuMs_median") for r in runs_])
        bg = med([load(l)[0].get("gpuMs_median") for l in by[bcell]]) if bcell in by and bcell != cell else None
        d_s = f"{med(sc_) - bs:+.2f}" if bs is not None else "Basis"
        d_g = f"{gpu - bg:+.2f}" if bg is not None else "Basis"
        fl = foliage_line(by[cell][0])
        tms = [tick_ms(l) for l in by[cell]]
        print(f"| {cell} | {len(runs_)} | " + " | ".join(cols) + f" | {min(sc_):.2f}..{max(sc_):.2f} | {d_s} | {f(gpu)} | {d_g} | "
              f"{fl[0] if fl else '–'} | {(fl[1] + ' / ' + fl[2]) if fl else '–'} | {f(med(tms), 1)} |")
    print()


pass_table("detail2x", "Tabelle 3b: GPU-Zeit je Pass (Detailed-Capture, RenderScale 2), ms")
pass_table("detail", "Tabelle 3: GPU-Zeit je Pass (Detailed-Capture, RenderScale 1), ms")


def slow_frames_ms(label, upto=6):
    """Sum of the busy time of the first slow frames ('[startup] app slow frame=N busyMs=X'): terrain build, texture
    array upload, material warm-up. Frame 1 (window, device) is not included."""
    tot = 0.0
    try:
        for l in open(f"{R}/{label}.log", errors="replace"):
            m = re.search(r"app slow frame=(\d+) busyMs=([\d.]+)", l)
            if m and 2 <= int(m.group(1)) <= upto:
                tot += float(m.group(2))
    except OSError:
        return None
    return tot


def mem_table(group, title, array_mb):
    by = defaultdict(list)
    for p in sorted(glob.glob(f"{R}/{group}-*-r*.summary.json")):
        label = os.path.basename(p)[: -len(".summary.json")]
        m = re.fullmatch(r"%s-(.+)-r(\d+)" % re.escape(group), label)
        if m:
            by[m.group(1)].append(label)
    if not by:
        return
    print(f"### {title}\n")
    print("| Format (V = 3, 3 Arrays) | Läufe | Arrays im Speicher MB | RSS-Spitze je Lauf, min..max MB (Median) | Scene-Pass p50 ms | Tonemap p50 ms (Takt-Zeuge) | Scene / Tonemap | GPU Σ Pässe p50 ms | CPU-Frame p50 ms | Slow-Frames 2-6 Σ ms (Start: Terrain, Upload, Warm-up) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for cell in ["V3-rgba8", "V3-astc", "V3-bc7", "V3-bc3"]:
        if cell not in by:
            continue
        rs = [load(l)[0] for l in by[cell]]
        sc_ = [r["gpuPassPercentiles"]["Scene"]["p50"] for r in rs if r.get("gpuPassPercentiles")]
        rm = [(r.get("rssMB") or {}).get("max") for r in rs]
        tm_ = [r["gpuPassPercentiles"]["Tonemap"]["p50"] for r in rs if r.get("gpuPassPercentiles")]
        print(f"| {cell[3:]} | {len(rs)} | {array_mb.get(cell[3:], '–')} | {min(rm):.0f}..{max(rm):.0f} ({med(rm):.0f}) "
              f"| {f(med(sc_))} | {f(med(tm_))} | {f(med([a / b for a, b in zip(sc_, tm_)]), 1)} | {f(med([r.get('gpuMs_median') for r in rs]))} "
              f"| {f(med([(r.get('cpuMs') or {}).get('p50') for r in rs]))} | {f(med([slow_frames_ms(l) for l in by[cell]]), 0)} |")
    print()


MB = {"rgba8": "873 (3 x 290,8)", "astc": "218 (3 x 72,7)", "bc7": "218 (3 x 72,7)", "bc3": "218 (3 x 72,7)"}
mem_table("mem2x", "Tabelle 4: Texturformate der Arrays auf Metal (nach dem GI-Slice-Means-Fix)", MB)
