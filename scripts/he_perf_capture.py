#!/usr/bin/env python3
"""Reproducible profiler capture of the real editor loop (Metal).

Runs the deployed editor of THIS tree with a private config dir (so nobody's
Preferences leak in), opens one project, lets it warm up, records a profiler
capture through HE_PROFILE_CAPTURE (the scripted F9, see Application::Run),
waits for the editor to leave on its own and summarises the dump.

  python3 scripts/he_perf_capture.py --project /tmp/pa1/proj/Test/Test.heproj \
      --label base-vsyncoff --out docs/perf-audit/raw [--detailed] [--vsync keep] \
      [--set SSAOEnabled=false --set RenderScale=0.5] [--scene variant.hescene]

--scene copies the given scene over the project's startup scene first (keep a
pristine copy of the original yourself). --set writes editor CustomConfig keys
(bool/int/float/str, parsed as JSON when possible). The editor camera is pinned
with --cam x,y,z,yaw,pitch so every run looks at the same picture.
"""
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"
DUMPS = EDITOR.parent / "dumps"


def parse_val(s):
    try:
        return json.loads(s)
    except json.JSONDecodeError:
        return s


def write_config(cfg_dir: Path, project: Path, sets, cam):
    cfg_dir.mkdir(parents=True, exist_ok=True)
    custom = {}
    if cam:
        x, y, z, yaw, pitch = (float(v) for v in cam.split(","))
        custom.update({"EditorCamValid": True, "EditorCamPosX": x, "EditorCamPosY": y,
                       "EditorCamPosZ": z, "EditorCamYaw": yaw, "EditorCamPitch": pitch})
    for kv in sets:
        k, v = kv.split("=", 1)
        custom[k] = parse_val(v)
    cfg = {
        "CustomConfig": [{"Key": k, "Value": v} for k, v in sorted(custom.items())],
        "KnownProjects": [str(project)],
        "LastProjectPath": str(project),
        "RHI": 4,  # HE::RendererBackend::Metal
    }
    (cfg_dir / "config.json").write_text(json.dumps(cfg, indent=4))


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def summarise(dump: Path):
    d = json.loads(dump.read_text())
    s, sess = d["summary"], d["session"]
    st = s.get("stats", {})
    frames = d.get("frames", [])
    out = {
        "session": sess,
        "fps": st.get("fps"),
        "deltaMs": st.get("deltaMs"),
        "cpuMs": st.get("cpuMs"),
        "gpuMs": st.get("gpuMs"),
        "gpuTimingModes": s.get("gpuTimingModes"),
        "gpuPassesOverlap": s.get("gpuPassesOverlap", False),
        "gpuPasses": {k: {"avg": v.get("avg"), "min": v.get("min"), "max": v.get("max")}
                      for k, v in (s.get("gpuPasses") or {}).items()},
        "topScopesBySelfTime": s.get("topScopesBySelfTime"),
        "hitchCount": s.get("hitchCount", 0),
    }
    if frames:
        f0 = frames[len(frames) // 2].get("stats", {})
        out["counters_midframe"] = f0
        gm = [f["gpuMs"] for f in frames if "gpuMs" in f]
        if gm:
            out["gpuMs_median"] = statistics.median(gm)
        # Σ(detailed passes)/gpuMs — the self-check that the detailed capture's
        # passes did not overlap (≈1.00 when they are exclusive and additive).
        ratios = []
        for f in frames:
            if f.get("gpuMode") == "detailed" and f.get("gpuMs"):
                tot = sum(p["ms"] for p in f.get("gpu", []) if not p.get("approx"))
                ratios.append(tot / f["gpuMs"])
        if ratios:
            out["detailedSumOverGpu_median"] = statistics.median(ratios)
        # Per-pass percentiles from the frames themselves (the summary only
        # carries min/avg/max), and the per-frame CPU time of each depth ≤ 1
        # scope — p50 next to the mean says whether a cost is steady or spiky.
        per_pass = {}
        for f in frames:
            for p in f.get("gpu", []):
                per_pass.setdefault(p["n"], []).append(p["ms"])
        out["gpuPassPercentiles"] = {
            k: {"min": min(v), "p10": pct(v, 10), "p50": pct(v, 50), "p90": pct(v, 90)}
            for k, v in per_pass.items()}
        per_scope = {}
        for f in frames:
            acc = {}
            for sc in f.get("cpu", []):
                acc[sc["n"]] = acc.get(sc["n"], 0.0) + sc["ms"]
            for k, v in acc.items():
                per_scope.setdefault(k, []).append(v)
        out["cpuScopePerFrame"] = {
            k: {"p10": pct(v, 10), "p50": pct(v, 50), "p90": pct(v, 90), "frames": len(v)}
            for k, v in per_scope.items()}
    cs = s.get("cpuScopes", {})
    out["cpuScopes"] = {k: {"avg": v["avg"], "self": v["selfMs"] / max(1, len(frames)),
                            "p95": v["p95"], "count": v["count"], "depth": v["depth"]}
                        for k, v in cs.items()}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", default=str(REPO / "docs" / "perf-audit" / "raw"))
    ap.add_argument("--cfgdir", default="/tmp/he_perf_cfg")
    ap.add_argument("--scene", help="scene file copied over the project's startup scene")
    ap.add_argument("--set", action="append", default=[], help="CustomConfig KEY=VALUE")
    ap.add_argument("--cam", help="x,y,z,yawRad,pitchRad editor camera")
    ap.add_argument("--warmup", type=int, default=300)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--detailed", action="store_true")
    ap.add_argument("--vsync", default="off", choices=["off", "keep"])
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE env")
    a = ap.parse_args()

    project = Path(a.project).resolve()
    if a.scene:
        heproj = json.loads(project.read_text())
        dst = project.parent / heproj["startupScene"]
        shutil.copyfile(a.scene, dst)
    write_config(Path(a.cfgdir), project, a.set, a.cam)

    env = dict(os.environ)
    env.update({
        "HE_CONFIG_DIR": a.cfgdir,
        "HE_PROFILE_CAPTURE": str(a.frames),
        "HE_PROFILE_WARMUP": str(a.warmup),
        "HE_PROFILE_DETAILED": "1" if a.detailed else "0",
        "HE_PROFILE_VSYNC": a.vsync,
        "HE_PROFILE_NOTE": a.label,
    })
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v

    DUMPS.mkdir(parents=True, exist_ok=True)
    before = set(DUMPS.glob("profile_*.json"))
    log = EDITOR.parent / "HorizonEngine.log"
    t0 = time.time()
    proc = subprocess.Popen([str(EDITOR)], cwd=str(EDITOR.parent), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Resident memory of the editor, sampled once a second while it runs (the
    # profiler's vram fields stay 0 on Metal, so this is the memory number).
    rss_kb = []
    while True:
        try:
            rc = proc.wait(timeout=1.0)
            break
        except subprocess.TimeoutExpired:
            if time.time() - t0 > a.timeout:
                proc.kill()
                print(f"[{a.label}] TIMEOUT after {a.timeout}s", file=sys.stderr)
                sys.exit(2)
            r = subprocess.run(["ps", "-o", "rss=", "-p", str(proc.pid)],
                               capture_output=True, text=True)
            if r.stdout.strip():
                rss_kb.append(int(r.stdout.strip()))
    wall = time.time() - t0
    new = sorted(set(DUMPS.glob("profile_*.json")) - before)
    if not new:
        print(f"[{a.label}] no dump written (rc={rc}, {wall:.0f}s)", file=sys.stderr)
        sys.exit(3)
    outdir = Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)
    dump_dst = outdir / f"{a.label}.profile.json"
    shutil.copyfile(new[-1], dump_dst)
    shutil.copyfile(log, outdir / f"{a.label}.log")
    summ = summarise(dump_dst)
    if rss_kb:
        summ["rssMB"] = {"max": max(rss_kb) / 1024.0, "last": rss_kb[-1] / 1024.0,
                         "samples": len(rss_kb)}
    summ["runner"] = {"label": a.label, "rc": rc, "wallSeconds": round(wall, 1),
                      "sets": a.set, "cam": a.cam, "scene": a.scene, "warmup": a.warmup,
                      "frames": a.frames, "detailed": a.detailed, "vsync": a.vsync}
    (outdir / f"{a.label}.summary.json").write_text(json.dumps(summ, indent=2))
    fps = (summ.get("fps") or {}).get("avg")
    cpu = (summ.get("cpuMs") or {}).get("p50")
    gpu = (summ.get("gpuMs") or {}).get("p50")
    print(f"[{a.label}] rc={rc} wall={wall:.0f}s fps={fps} cpu_p50={cpu} gpu_p50={gpu} "
          f"modes={summ.get('gpuTimingModes')} size={summ['session'].get('width')}x"
          f"{summ['session'].get('height')}")


if __name__ == "__main__":
    main()
