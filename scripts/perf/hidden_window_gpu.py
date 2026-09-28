#!/usr/bin/env python3
"""GPU/CPU load of a hidden editor, background throttle off vs. on (Thema 101, B1).

Starts the deployed editor the way he_mcp_multiclient.py does (private HOME,
a copy of HorizonTutorial, Metal, HE_SKY_TIME=30, HE_HIDDEN_WINDOW=1) -- the
orphan from Perf-Audit B1 -- waits for the throttle line in its log, lets it
settle, then measures WINDOW seconds of GPU time (AGX accumulatedGPUTime in the
IORegistry, as scripts/perf/gpu_time_by_process.py on main) and CPU time of
that one process. Prints one JSON line.

  python3 scripts/perf/hidden_window_gpu.py LABEL [KEY=VAL ...] [--editor PATH]

  # old behaviour vs. the default throttle, alternating:
  python3 scripts/perf/hidden_window_gpu.py A-off HE_BACKGROUND_FPS=0
  python3 scripts/perf/hidden_window_gpu.py B-on

Use a RELEASE editor: a Debug one builds the 256^3 sky noise on the CPU and
can take over 10 minutes to its first frame (Low Power Mode). Environment:
MEASURE_S (20), SETTLE_S (20), HE_MEASURE_OUT (/tmp/he_hidden_window_gpu).
"""
import json, os, pathlib, re, shutil, subprocess, sys, time

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"
PROJECT_SRC = pathlib.Path.home() / "Documents" / "HorizonEngine" / "HorizonTutorial"
OUT = pathlib.Path(os.environ.get("HE_MEASURE_OUT", "/tmp/he_hidden_window_gpu"))
WINDOW = float(os.environ.get("MEASURE_S", "20"))
SETTLE = float(os.environ.get("SETTLE_S", "20"))
BOOT_LIMIT = 500

CREATOR = re.compile(r'"IOUserClientCreator" = "pid (\d+), ([^"]*)"')
USAGE = re.compile(r'"accumulatedGPUTime"=(\d+)')
MARKER = re.compile(r"(Window in background[^\n]*|Background throttle off[^\n]*)")


def gpu_ns(pid):
    out = subprocess.run(["/usr/sbin/ioreg", "-l", "-w0", "-r", "-c", "IOAccelerator"],
                         capture_output=True, text=True).stdout
    total, pending = 0, 0
    # ioreg prints "AppUsage" before the "IOUserClientCreator" of the same client.
    for line in out.splitlines():
        if '"AppUsage"' in line:
            pending = sum(int(v) for v in USAGE.findall(line))
            continue
        m = CREATOR.search(line)
        if m:
            if int(m.group(1)) == pid:
                total += pending
            pending = 0
    return total


def cpu_s(pid):
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                       capture_output=True, text=True).stdout.strip()
    secs = 0.0
    for p in t.replace("-", ":").split(":"):     # [[dd-]hh:]mm:ss.cc
        secs = secs * 60 + float(p)
    return secs


def main():
    args = sys.argv[1:]
    editor = EDITOR
    if "--editor" in args:
        i = args.index("--editor")
        editor = pathlib.Path(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__); sys.exit(2)
    label, extra = args[0], dict(a.split("=", 1) for a in args[1:])

    run = OUT / label
    shutil.rmtree(run, ignore_errors=True)
    home = run / "home"
    userdata = home / "Library" / "Application Support" / "HorizonEngine"
    userdata.mkdir(parents=True)
    project = run / "project"
    shutil.copytree(PROJECT_SRC, project)
    heproj = next(project.glob("*.heproj"))
    (userdata / "config.json").write_text(json.dumps(
        {"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)],
         "RHI": 4, "CustomConfig": []}))
    env = dict(os.environ)
    env.update({"HOME": str(home), "HE_DUMP_RHI": "Metal", "HE_SKY_TIME": "30",
                "HE_COLLAB_OFFLINE": "1", "HE_HIDDEN_WINDOW": "1"})
    for k in ("HE_BACKGROUND_FPS", "HE_EXIT_AFTER_FRAMES", "HE_DUMP_PATH", "HE_CAPTURE_FRAME"):
        env.pop(k, None)
    env.update(extra)

    # script -q: a pseudo-TTY keeps the editor's stdout line-buffered; into a
    # pipe or file the log would only arrive at exit.
    out = open(run / "script.out", "wb")
    proc = subprocess.Popen(["/usr/bin/script", "-q", str(run / "editor.log"), str(editor)],
                            env=env, stdin=subprocess.DEVNULL, stdout=out,
                            stderr=subprocess.STDOUT, cwd=str(editor.parent))
    pid, marker, t0 = None, None, time.time()
    try:
        while time.time() - t0 < BOOT_LIMIT and not marker:
            if proc.poll() is not None:
                raise RuntimeError("editor exited early rc=%s" % proc.returncode)
            if pid is None:
                kids = subprocess.run(["pgrep", "-P", str(proc.pid)],
                                      capture_output=True, text=True).stdout.split()
                pid = int(kids[0]) if kids else None
            log = run / "editor.log"
            if log.exists():
                m = MARKER.search(log.read_text(errors="replace"))
                marker = m.group(1).strip() if m else None
            time.sleep(1)
        if not marker:
            raise RuntimeError("no throttle line in the log after %d s" % BOOT_LIMIT)
        boot = time.time() - t0
        time.sleep(SETTLE)
        g0, c0, w0 = gpu_ns(pid), cpu_s(pid), time.monotonic()
        time.sleep(WINDOW)
        g1, c1, w1 = gpu_ns(pid), cpu_s(pid), time.monotonic()
        wall = w1 - w0
        res = {"label": label, "env": extra, "pid": pid, "marker": marker,
               "boot_s": round(boot, 1), "window_s": round(wall, 2),
               "gpu_pct": round((g1 - g0) / (wall * 1e9) * 100, 2),
               "cpu_pct": round((c1 - c0) / wall * 100, 1)}
        print(json.dumps(res, ensure_ascii=False))
        (run / "result.json").write_text(json.dumps(res, indent=1, ensure_ascii=False))
    finally:
        if pid:
            subprocess.run(["kill", "-TERM", str(pid)])
        proc.terminate()
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait(5)
        out.close()


if __name__ == "__main__":
    main()
