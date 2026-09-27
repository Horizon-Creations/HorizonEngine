#!/usr/bin/env python3
"""GPU time per process on Apple Silicon, without sudo (Perf-Audit Thema 99, Schritt 2).

Reads the per-client "AppUsage" -> accumulatedGPUTime (ns) that the AGX driver
publishes in the IORegistry, takes two snapshots INTERVAL seconds apart and
prints each process's share of wall time on the GPU. Shares can exceed 100 %
in sum: the GPU runs vertex, fragment and compute work of different clients
concurrently.

  python3 scripts/perf/gpu_time_by_process.py [INTERVAL_S=10] [--json]
"""
import json
import re
import subprocess
import sys
import time
from collections import defaultdict

CREATOR = re.compile(r'"IOUserClientCreator" = "pid (\d+), ([^"]*)"')
USAGE = re.compile(r'"accumulatedGPUTime"=(\d+)')


def snapshot():
    out = subprocess.run(["/usr/sbin/ioreg", "-l", "-w0", "-r", "-c", "IOAccelerator"],
                         capture_output=True, text=True).stdout
    total = defaultdict(int)
    names = {}
    # ioreg prints a node's properties sorted by key, so "AppUsage" comes BEFORE
    # the "IOUserClientCreator" of the same user client: hold it until then.
    pending = 0
    for line in out.splitlines():
        if '"AppUsage"' in line:
            pending = sum(int(v) for v in USAGE.findall(line))
            continue
        m = CREATOR.search(line)
        if m:
            pid = int(m.group(1))
            names[pid] = m.group(2)
            total[pid] += pending
            pending = 0
    return total, names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    interval = float(args[0]) if args else 10.0
    t0 = time.monotonic()
    a, names = snapshot()
    time.sleep(interval)
    b, names2 = snapshot()
    wall = (time.monotonic() - t0) * 1e9
    names.update(names2)
    rows = sorted(((b[p] - a.get(p, 0)) / wall * 100.0, p) for p in b)
    rows = [(pct, p) for pct, p in reversed(rows) if pct >= 0.1]
    if "--json" in sys.argv:
        print(json.dumps({"interval_s": interval,
                          "processes": [{"pid": p, "name": names.get(p, "?"), "gpu_pct": round(pct, 2)}
                                        for pct, p in rows]}, indent=1))
    else:
        for pct, p in rows:
            print(f"{pct:7.2f} %  pid {p:<6} {names.get(p, '?')}")


if __name__ == "__main__":
    main()
