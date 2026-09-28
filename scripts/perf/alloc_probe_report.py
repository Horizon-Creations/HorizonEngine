#!/usr/bin/env python3
"""Group alloc_probe stack dumps by the engine function that caused them.

Reads the file written by he_alloc_probe_dump (HE_ALLOC_PROBE_OUT) and attributes
every stack to its first frame inside the engine's own images (HorizonEditor,
libHorizon*.dylib), skipping std:: / libc++ helpers, so "std::vector::push_back"
lands on the function that pushed. Symbols are demangled with c++filt.

  python3 scripts/perf/alloc_probe_report.py /tmp/A2_stacks.txt [--top 40] [--depth 3]
"""
import argparse
import re
import subprocess
from collections import defaultdict

OWN = re.compile(r"^(HorizonEditor|libHorizon\w*\.dylib|HorizonGame)$")
SKIP_SYM = re.compile(r"^(std::|void std::|__gnu|operator new|operator delete|nlohmann::|fmt::|"
                      r"std::__1::|decltype|__cxx|glm::)")


def demangle(names):
    names = list(names)
    if not names:
        return {}
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.split("\n")
    return {n: (out[i] if i < len(out) and out[i] else n) for i, n in enumerate(names)}


def parse(path):
    stacks, head = [], ""
    cur = None
    for line in open(path, errors="replace"):
        if line.startswith("#"):
            head = line.strip()
        elif line.startswith("STACK"):
            m = re.match(r"STACK kind=(\w+) count=(\d+) bytes=(\d+)", line)
            cur = {"kind": m.group(1), "count": int(m.group(2)), "bytes": int(m.group(3)), "frames": []}
            stacks.append(cur)
        elif line.startswith("  ") and cur is not None:
            m = re.match(r"\s+(.*?)!(.*?)\+(\d+) @", line)
            if m:
                cur["frames"].append((m.group(1), m.group(2)))
    return head, stacks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stacks")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--depth", type=int, default=3, help="engine frames shown per group")
    ap.add_argument("--kind", default="alloc")
    a = ap.parse_args()
    head, stacks = parse(a.stacks)
    m = re.search(r"frames=(\d+)", head)
    frames = int(m.group(1)) if m else 1
    syms = {s for st in stacks for _, s in st["frames"]}
    dm = demangle(sorted(syms))

    groups = defaultdict(lambda: [0, 0])
    total = [0, 0]
    for st in stacks:
        if st["kind"] != a.kind:
            continue
        chain = []
        for img, s in st["frames"]:
            name = dm.get(s, s)
            if OWN.match(img) and not SKIP_SYM.match(name):
                chain.append(name)
            if len(chain) >= a.depth:
                break
        if not chain:
            # System-only stack (e.g. Metal/QuartzCore callbacks): name the first frame.
            img, s = st["frames"][0] if st["frames"] else ("?", "?")
            chain = [f"[{img}] {dm.get(s, s)}"]
        key = "  <-  ".join(c[:140] for c in chain)
        groups[key][0] += st["count"]
        groups[key][1] += st["bytes"]
        total[0] += st["count"]
        total[1] += st["bytes"]

    print(f"{head}")
    print(f"kind={a.kind}: {total[0]} events in {frames} frames = {total[0]/frames:.1f}/frame, "
          f"{total[1]/frames/1024:.1f} KiB/frame")
    print(f"{'per frame':>9} {'KiB/frame':>9}  owner (first engine frames, innermost first)")
    for key, (c, b) in sorted(groups.items(), key=lambda kv: -kv[1][0])[:a.top]:
        print(f"{c/frames:9.1f} {b/frames/1024:9.1f}  {key}")


if __name__ == "__main__":
    main()
