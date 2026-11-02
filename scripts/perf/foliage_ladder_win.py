#!/usr/bin/env python3
"""Foliage timing ladder on Windows, before against after, every GPU backend (Thema 182 step 5).

The Windows counterpart of foliage_ladder.sh (zsh, macOS probes): one he_perf_capture.py run per
case, in the real editor loop, with a hidden window and vsync forced off for the capture. Before and
after alternate within each case, so a pair shares the machine's load; the ratio inside a pair is the
number to read, the milliseconds are not comparable to another day.

    python -I scripts/perf/foliage_ladder_win.py --project C:/hw182/s5/proj/FolPerf/FolPerf.heproj \
        --scenes C:/hw182/s5/scenes --out C:/hw182/s5/raw \
        --before C:/hw182b/deploy/Editor --after C:/hw182/deploy/Editor \
        [--rhi OpenGL,D3D11,D3D12,Vulkan] [--counts 10000,100000,500000] [--dists 1000000,100] [--reps 2]

Scenes are fol_<N>_d<dist>.hescene from gen_reference_world.py --foliage N --foliage-distance dist
(400 m field, built-in cube). The project's startup scene is OVERWRITTEN, so pass a private project.
GL on Windows keeps its program-binary cache under SDL_GetPrefPath, shared by every run and not
redirected by the private config dir: the first GL run of each binary is a discarded warm run.

Each run writes <label>.conditions.txt (CPU load, GPU use, other editors and test binaries running,
before and after). foliage_ladder_win.py --table <out> prints the tables from the summaries.
"""
import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CAPTURE = HERE.parent / "he_perf_capture.py"


def conditions() -> str:
    def run(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return "?"
    gpu = run(["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,clocks.sm",
               "--format=csv,noheader"])
    ps = ("$c=(Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average; "
          "$p=(Get-Process HorizonEditor,HorizonGame,he_tests,cl,link -ErrorAction SilentlyContinue | "
          "Group-Object ProcessName | ForEach-Object { $_.Name + 'x' + $_.Count }) -join ' '; "
          "'cpu ' + $c + '%  procs: ' + $p")
    return f"  {run(['powershell', '-NoProfile', '-Command', ps])}\n  gpu (util, temp, sm clock): {gpu}"


def capture(a, editor: Path, rhi: str, scene: Path, label: str, warm=False) -> int:
    cond = Path(a.out) / f"{label}.conditions.txt"
    text = f"== {label}\nbefore:\n{conditions()}\n"
    cmd = [sys.executable, "-I", str(CAPTURE), "--project", a.project, "--label", label,
           "--out", a.out, "--scene", str(scene), "--warmup", str(a.warmup),
           "--frames", str(a.frames if not warm else 30), "--no-counters", "--timeout", str(a.timeout),
           "--cam", a.cam, "--rhi", rhi, "--cfgdir", str(Path(a.out) / "cfg"),
           "--editor", str(editor / "HorizonEditor.exe"), "--env", "HE_HIDDEN_WINDOW=1"]
    rc = subprocess.run(cmd).returncode
    text += f"after (rc={rc}):\n{conditions()}\n"
    if not warm:
        cond.write_text(text)
    return rc


def ladder(a) -> int:
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    sides = [("before", Path(a.before)), ("after", Path(a.after))]
    bad = 0
    for rhi in a.rhi.split(","):
        if rhi == "OpenGL":
            for side, ed in sides:
                capture(a, ed, rhi, Path(a.scenes) / "fol_10000_d100.hescene", f"warm-{side}-{rhi}", warm=True)
        for n in a.counts.split(","):
            for d in a.dists.split(","):
                scene = Path(a.scenes) / f"fol_{n}_d{d}.hescene"
                tag = "all" if float(d) >= 1e5 else f"d{d}"
                for r in range(1, a.reps + 1):
                    # Alternate who goes first, so neither side always runs on the warmer GPU.
                    order = sides if r % 2 else sides[::-1]
                    for side, ed in order:
                        label = f"{side}-{rhi}-{n}-{tag}-r{r}"
                        if (out / f"{label}.summary.json").exists():
                            continue
                        bad += capture(a, ed, rhi, scene, label) != 0
    return 1 if bad else 0


def table(outdir: Path) -> int:
    rows = {}
    for f in sorted(outdir.glob("*.summary.json")):
        if f.name.startswith("warm-"):
            continue
        side, rhi, n, tag, rep = f.name[:-len(".summary.json")].split("-")
        s = json.loads(f.read_text())
        c = s.get("cpuScopePerFrame", {})
        mid = s.get("counters_midframe", {})
        rows.setdefault((rhi, int(n), tag), {}).setdefault(side, []).append({
            "render": c.get("Render", {}).get("p50"),
            "cpu": (s.get("cpuMs") or {}).get("p50"),
            "gpu": (s.get("gpuMs") or {}).get("p50"),
            "extract": c.get("RenderExtractor::extract", {}).get("p50"),
            "foliage": c.get("ExtractFoliage", {}).get("p50"),
            "draws": mid.get("draws"), "tris": mid.get("tris"), "visible": mid.get("visible"),
            "rss": (s.get("rssMB") or {}).get("max"), "rep": rep})

    def med(lst, k):
        v = [x[k] for x in lst if x.get(k) is not None]
        return statistics.median(v) if v else None

    def fmt(v, p=2):
        return "-" if v is None else (f"{v:.{p}f}" if isinstance(v, float) else str(v))

    print("| Backend | Instanzen | Reichweite | Wdh. | `Render` p50 vorher / nachher | CPU p50 vorher / nachher | "
          "GPU p50 vorher / nachher | `ExtractFoliage` vorher / nachher | draws vorher / nachher | "
          "tris vorher / nachher | RSS max MB vorher / nachher |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    order = {"OpenGL": 0, "D3D11": 1, "D3D12": 2, "Vulkan": 3}
    for (rhi, n, tag) in sorted(rows, key=lambda k: (order.get(k[0], 9), k[1], k[2] != "all")):
        b, a = rows[(rhi, n, tag)].get("before", []), rows[(rhi, n, tag)].get("after", [])
        cells = [f"{fmt(med(b, k), p)} / {fmt(med(a, k), p)}"
                 for k, p in (("render", 2), ("cpu", 2), ("gpu", 2), ("foliage", 3))]
        cnt = [f"{fmt(med(b, k), 0)} / {fmt(med(a, k), 0)}" for k in ("draws", "tris", "rss")]
        print(f"| {rhi} | {n:,} | {'alle' if tag == 'all' else tag} | {min(len(b), len(a))} | "
              + " | ".join(cells + cnt) + " |")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", help="print the tables of an out dir and stop")
    ap.add_argument("--project")
    ap.add_argument("--scenes")
    ap.add_argument("--out")
    ap.add_argument("--before")
    ap.add_argument("--after")
    ap.add_argument("--rhi", default="OpenGL,D3D11,D3D12,Vulkan")
    ap.add_argument("--counts", default="10000,100000,500000")
    ap.add_argument("--dists", default="1000000,100")
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--cam", default="0,30,0,0,-0.2", help="in the middle of the field, looking along -Z")
    a = ap.parse_args()
    if a.table:
        return table(Path(a.table))
    for k in ("project", "scenes", "out", "before", "after"):
        if not getattr(a, k):
            ap.error(f"--{k} is required")
    return ladder(a)


if __name__ == "__main__":
    sys.exit(main())
