#!/usr/bin/env python3
"""RVT 2.0 step 7/1615: pool and paint stats per backend, from the HE_DUMP_RVTDRIVER/HE_DUMP_RVTBASE witness
(the same end-to-end run EditorApplication.cpp runs for the Metal witnesses in steps 4-6, docs/rvt-2-0-step*.md).

One editor launch per backend, hidden window, a scratch APPDATA each (never the user's real config/profile).
Parses the RvtLandscape pool/paint log lines and the HE_DUMP_RVTBASE/HE_DUMP_RVTDRIVER summary lines, and writes
one JSON report with, per backend: the pool size, total pages painted, total paint time, and the baked-base and
journal-repaint deltas the witness already checks. Meant for a quick A/B across backends or across commits, not
as a replacement for the per-frame GPU profiler (docs/rvt-2-0-step4-auto-landscape-split-2026-11-08.md Handoff).

    python3 scripts/perf/landscape_rvt_pool_regression.py --editor C:/hwRvt335/deploy/Editor/HorizonEditor.exe \
        --scratch-root C:/hwRvt335 --out docs/perf-audit/rvt-pool-regression.json
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
BACKENDS = ["OpenGL", "Vulkan", "D3D11", "D3D12"]

POOL_RE = re.compile(r"RvtLandscape: pool of (\d+) pages of (\d+) px \(([\d.]+) MB\)")
PAINT_RE = re.compile(
    r"RvtLandscape: (\d+) page\(s\) painted, (\d+) marked stale, \d+ table\(s\) uploaded, .*?"
    r"(\d+) resident, (\d+) stale, painting took ([\d.]+) ms")
DRIVER_DONE_RE = re.compile(r"HE_DUMP_RVTDRIVER done: (\d+) pages painted in total")
BASE_REF_RE = re.compile(r"HE_DUMP_RVTBASE reference \(page graphs\): .*?mean ([\d.]+) /255, max (\d+)")
BASE_BAKED_RE = re.compile(r"HE_DUMP_RVTBASE baked: .*?vs reference pages mean ([\d.]+) /255 max (\d+)")
SCULPT_RE = re.compile(r"HE_DUMP_RVTDRIVER sculpt stroke: (\d+) pages marked stale .*?(\d+) repainted")
PAINT_STROKE_RE = re.compile(r"HE_DUMP_RVTDRIVER paint stroke: (\d+) pages marked stale .*?(\d+) repainted")
ERROR_RE = re.compile(r"\[ERROR\]")


def run_backend(editor: Path, scratch_root: Path, backend: str) -> dict:
    scratch = scratch_root / f"scratch_{backend.lower()}"
    scratch.mkdir(parents=True, exist_ok=True)
    log_path = scratch / "HorizonEngine" / "HorizonEngine.log"
    if log_path.exists():
        log_path.unlink()
    env_overrides = {
        "APPDATA": str(scratch),
        "HE_HIDDEN_WINDOW": "1",
        "HE_BACKGROUND_FPS": "0",
        "HE_COLLAB_OFFLINE": "1",
        "HE_DUMP_RHI": backend,
        "HE_RVT": "1",
        "HE_DUMP_AUTOLAND": "1",
        "HE_DUMP_RVTDRIVER": "1",
        "HE_DUMP_RVTBASE": "1",
        "HE_DUMP_PATH": str(scratch / "x.bmp"),
        "HE_DUMP_QUIT": "1",
    }
    import os
    env = dict(os.environ)
    env.update(env_overrides)
    proc = subprocess.run([str(editor)], env=env, capture_output=True, text=True, timeout=120)
    log = proc.stdout + proc.stderr
    if log_path.exists():
        log = log_path.read_text(errors="replace")

    pools = [m.groups() for m in POOL_RE.finditer(log)]
    paints = [m.groups() for m in PAINT_RE.finditer(log)]
    total_painted = sum(int(p[0]) for p in paints)
    total_paint_ms = sum(float(p[4]) for p in paints)
    driver_done = DRIVER_DONE_RE.search(log)
    base_ref = BASE_REF_RE.search(log)
    base_baked = BASE_BAKED_RE.search(log)
    sculpt = SCULPT_RE.search(log)
    paint_stroke = PAINT_STROKE_RE.search(log)
    errors = len(ERROR_RE.findall(log))

    return {
        "backend": backend,
        "exit_code": proc.returncode,
        "pool_slots": int(pools[0][0]) if pools else None,
        "pool_page_px": int(pools[0][1]) if pools else None,
        "pool_mb": float(pools[0][2]) if pools else None,
        "paint_events": len(paints),
        "total_pages_painted": total_painted,
        "total_paint_ms": round(total_paint_ms, 2),
        "driver_total_painted": int(driver_done.group(1)) if driver_done else None,
        "baked_base_mean_vs_reference": float(base_baked.group(1)) if base_baked else None,
        "baked_base_max_vs_reference": int(base_baked.group(2)) if base_baked else None,
        "page_graphs_mean_vs_whole_material": float(base_ref.group(1)) if base_ref else None,
        "sculpt_stroke_stale_repainted": [int(sculpt.group(1)), int(sculpt.group(2))] if sculpt else None,
        "paint_stroke_stale_repainted": [int(paint_stroke.group(1)), int(paint_stroke.group(2))] if paint_stroke else None,
        "log_error_lines": errors,
        "log_path": str(log_path),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--editor", required=True, help="path to the deployed HorizonEditor.exe")
    ap.add_argument("--scratch-root", required=True, help="a private, disposable directory for per-backend APPDATA")
    ap.add_argument("--backends", default=",".join(BACKENDS))
    ap.add_argument("--out", default=str(REPO / "docs" / "perf-audit" / "rvt-pool-regression.json"))
    a = ap.parse_args()

    editor = Path(a.editor)
    scratch_root = Path(a.scratch_root)
    results = []
    for backend in a.backends.split(","):
        print(f"== {backend} ==", flush=True)
        r = run_backend(editor, scratch_root, backend)
        results.append(r)
        print(json.dumps(r, indent=2))

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(results, indent=2))
    print(f"\nwrote {out}")

    failed = [r for r in results if r["exit_code"] != 0 or r["driver_total_painted"] in (None, 0)]
    if failed:
        print(f"backends with no witness output or a non-zero exit: {[r['backend'] for r in failed]}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
