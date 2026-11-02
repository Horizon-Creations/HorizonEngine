#!/usr/bin/env python3
"""One timed run of the landscape performance investigation (Thema 237, Schritt 1).

A thin wrapper around scripts/he_perf_capture.py that adds what the investigation needs and the capture script
does not do on its own:

  * the settings of a project's EXPORTED game (config.json of an export, "CustomConfig") laid over the private
    editor config, so the editor loop renders with the same switches the player's game was built with
    (--export-config; the keys that only a running game reads, Game*, are dropped);
  * the display kept awake for the run (caffeinate -d -u): with the display asleep or the screen locked the first
    present never returns and a run hangs until its timeout (seen on 21.10.2026, 300 s TIMEOUT, then 15 s with
    the display awake);
  * a *.conditions.txt next to every result (power, lock, load, GPU use, busiest processes before and after) and
    a refusal to start while a compiler / linker / second editor is running (a run that overlapped one is moved
    to discarded-under-load/ and repeated);
  * the per-frame p50 / p90 of every CPU scope in <label>.scopes.json.

  python3 scripts/perf/landscape_regression.py --label cur-catania-metal-gi --editor /tmp/he237/cur/Editor/HorizonEditor \
      --project /tmp/he237/catania/HEProject.heproj --export-config <export>/HEProject.app/Contents/Resources/config.json \
      --cam 0,2,8,0,0 --rhi Metal --out docs/perf-audit/raw-landscape-regression [--set K=V ...] [--detailed] [--scale 2]

The run itself is he_perf_capture.py: real editor loop, vsync off, 200 frames warm-up, 600 recorded.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CAPTURE = REPO / "scripts" / "he_perf_capture.py"
FOREIGN = re.compile(r"\b(cc1|clang|clang\+\+|ninja|make|cmake|ld|HorizonEditor|HorizonGame|HEProject|he_tests|rustc|swift-frontend)\b")
# keys that only the exported game reads (window of the game, its backend), or that make no sense in an editor loop
DROP = re.compile(r"^(Game[A-Z]|GpuParticles$)")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def foreign_procs():
    seen = []
    for line in run(["ps", "-Ao", "pid,pcpu,comm"]).splitlines()[1:]:
        pid, cpu, comm = line.split(None, 2)
        base = os.path.basename(comm)
        if FOREIGN.fullmatch(base) and "clangd" not in comm:
            seen.append(f"{base}({cpu}%)")
    return seen


def conditions():
    ioreg = run(["/usr/sbin/ioreg", "-n", "Root", "-d1"])
    lock = re.search(r'"CGSSessionScreenIsLocked"=(\w+)', ioreg)
    pm = run(["/usr/bin/pmset", "-g"])
    low = re.search(r"lowpowermode\s+(\d)", pm)
    gpu = re.search(r'"Device Utilization %"=(\d+)', run(["/usr/sbin/ioreg", "-r", "-c", "IOAccelerator", "-d", "1"]))
    top = sorted(run(["ps", "-Ao", "pcpu,comm"]).splitlines()[1:], key=lambda l: -float(l.split(None, 1)[0]))[:4]
    return {
        "uptime": re.sub(r".*load", "load", run(["uptime"]).strip()),
        "locked": lock.group(1) if lock else "none",
        "lowpower": low.group(1) if low else "?",
        "gpu": gpu.group(1) if gpu else "?",
        "power": run(["/usr/bin/pmset", "-g", "batt"]).splitlines()[0],
        "busy": "; ".join(f"{l.split(None, 1)[0]}% {os.path.basename(l.split(None, 1)[1])}" for l in top),
        "foreign": ",".join(foreign_procs()) or "none",
    }


def fmt(c):
    return "".join(f"  {k + ':':10} {v}\n" for k, v in c.items())


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def scopes_p50(profile: Path):
    d = json.loads(profile.read_text())
    per = {}
    for f in d.get("frames", []):
        acc = {}
        for sc in f.get("cpu", []):
            acc[sc["n"]] = acc.get(sc["n"], 0.0) + sc["ms"]
        for k, v in acc.items():
            per.setdefault(k, []).append(v)
    return {k: {"p50": pct(v, 50), "p90": pct(v, 90), "mean": statistics.fmean(v), "frames": len(v)} for k, v in per.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--editor", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--scene")
    ap.add_argument("--export-config", help="config.json of an exported game: its CustomConfig is laid over the run")
    ap.add_argument("--cam", default=None)
    ap.add_argument("--rhi", default="Metal")
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--detailed", action="store_true")
    ap.add_argument("--scale", type=float, default=None, help="RenderScale")
    ap.add_argument("--out", default=str(REPO / "docs" / "perf-audit" / "raw-landscape-regression"))
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--tries", type=int, default=3)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    sets = ["ViewportGroundGrid=false"]
    if a.export_config:
        cfg = json.loads(Path(a.export_config).read_text())
        for e in cfg.get("CustomConfig", []):
            if not DROP.match(e["Key"]):
                sets.append(f"{e['Key']}={json.dumps(e['Value'])}")
    if a.scale is not None:
        sets.append(f"RenderScale={a.scale}")
    sets += a.set          # later wins: he_perf_capture writes the dict in order

    for attempt in range(1, a.tries + 1):
        t0 = time.time()
        while foreign_procs() and time.time() - t0 < 1800:
            print(f"  waiting for a quiet machine: {foreign_procs()}", flush=True)
            time.sleep(30)
        before = conditions()
        cmd = [sys.executable, "-I", str(CAPTURE), "--project", a.project, "--label", a.label, "--out", str(out),
               "--warmup", str(a.warmup), "--frames", str(a.frames), "--rhi", a.rhi, "--editor", a.editor,
               "--timeout", str(a.timeout), "--no-counters" if not a.detailed else "--detailed"]
        if a.scene:
            cmd += ["--scene", a.scene]
        if a.cam:
            cmd += ["--cam", a.cam]
        for s in sets:
            cmd += ["--set", s]
        for e in a.env:
            cmd += ["--env", e]
        # the display awake for the whole run (a sleeping or locked display stalls the first present)
        cf = subprocess.Popen(["caffeinate", "-d", "-u", "-t", str(a.timeout + 60)])
        time.sleep(1.0)
        try:
            res = subprocess.run(cmd, capture_output=True, text=True)
        finally:
            cf.terminate()
        after = conditions()
        txt = f"label: {a.label}\nattempt: {attempt}\neditor: {a.editor}\nrhi: {a.rhi}\nsets: {sets}\n"
        txt += "before:\n" + fmt(before) + "after:\n" + fmt(after)
        (out / f"{a.label}.conditions.txt").write_text(txt)
        line = (res.stdout.strip().splitlines() or [""])[-1]
        print(line or res.stderr.strip()[-300:], flush=True)
        prof = out / f"{a.label}.profile.json"
        if res.returncode == 0 and prof.exists():
            if after["foreign"] != "none" and attempt < a.tries:
                d = out / "discarded-under-load"
                d.mkdir(exist_ok=True)
                for f in out.glob(f"{a.label}.*"):
                    shutil.move(str(f), d / f.name)
                print(f"  a compiler or second editor was running ({after['foreign']}), repeating", flush=True)
                continue
            (out / f"{a.label}.scopes.json").write_text(json.dumps(scopes_p50(prof), indent=1))
            return
        print(f"  run failed (rc={res.returncode}), attempt {attempt}/{a.tries}", flush=True)
    sys.exit(3)


if __name__ == "__main__":
    main()
