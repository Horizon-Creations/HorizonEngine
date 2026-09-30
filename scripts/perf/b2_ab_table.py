#!/usr/bin/env python3
"""One Markdown row per dump of a b2_ab_load.sh series (Thema 102, B2).

    python3 scripts/perf/b2_ab_table.py /tmp/b2s2_ab [more dirs...]

Columns come from step3_frames.py (same numbers as audit step 3, table 1.2), plus
the load average the series log recorded at the start of each run.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def start_loads(series_log):
    loads = {}
    if series_log.exists():
        for line in series_log.read_text().splitlines():
            m = re.match(r"\[[\d:]+\] start (\S+) \| ([\d.]+)", line)
            if m:
                loads[m.group(1)] = m.group(2)
    return loads


def main():
    print("| Lauf | Load 1 min | Jobs/Frame | extract+cull Hauptthread p50/p90 ms | CPU ohne ND p50/p90 ms | Frames > 5 ms |")
    print("|---|---|---|---|---|---|")
    for d in map(Path, sys.argv[1:]):
        loads = start_loads(d / "series.log")
        dumps = sorted(d.glob("*.profile.json"), key=lambda p: p.stat().st_mtime)
        out = subprocess.run([sys.executable, str(HERE / "step3_frames.py"), "--json", *map(str, dumps)],
                             capture_output=True, text=True, check=True).stdout
        for r in json.loads(out):
            label = Path(r["dump"]).name.removesuffix(".profile.json")
            cpu = r["cpu-minus-ND p50/p90/p99/max"].split(" / ")
            print(f"| {label} | {loads.get(label, '?')} | {r.get('jobs/frame p50 (max)', '-')} "
                  f"| {r.get('main-thread extract+cull ms/frame p50/p90', '-')} "
                  f"| {cpu[0]} / {cpu[1]} | {r['frames with cpu-minus-ND > 5.0 ms']}/{r['frames']} |")


if __name__ == "__main__":
    main()
