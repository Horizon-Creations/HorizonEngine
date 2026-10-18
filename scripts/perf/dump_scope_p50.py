#!/usr/bin/env python3
"""p50/p90 of profiler scopes in a capture dump (HE_PROFILE_CAPTURE, F9), per call and per frame.

  python3 scripts/perf/dump_scope_p50.py /tmp/ws_game/dumps/profile_*.json [Scope ...]

Default scopes: RenderExtractor::extract, CellStreaming, Render. A scope entered several times in a
frame (the extractor runs once per pass, 162's FrameScope answers the later ones from a copy) is
listed per call and summed per frame; the frame's CPU time is the dump's own cpuMs.
"""
import json
import statistics
import sys


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def main():
    path = sys.argv[1]
    names = sys.argv[2:] or ["RenderExtractor::extract", "CellStreaming", "Render"]
    d = json.load(open(path))
    frames = d["frames"]
    print(f"{path}: {len(frames)} frames, {d['session'].get('note', '')}")
    cpu = [f["cpuMs"] for f in frames]
    print(f"  frame CPU p50 {pct(cpu, 50):.2f} ms  p90 {pct(cpu, 90):.2f}  max {max(cpu):.2f}")
    for name in names:
        calls, per_frame = [], []
        for f in frames:
            ms = [c["ms"] for c in f["cpu"] if c["n"] == name]
            calls += ms
            per_frame.append(sum(ms))
        if not calls:
            print(f"  {name}: not in the dump")
            continue
        print(f"  {name}: {len(calls)} calls, per call p50 {pct(calls, 50):.3f} ms p90 {pct(calls, 90):.3f} max {max(calls):.3f}"
              f" | per frame p50 {pct(per_frame, 50):.3f} ms p90 {pct(per_frame, 90):.3f} (mean {statistics.mean(per_frame):.3f})")


if __name__ == "__main__":
    main()
