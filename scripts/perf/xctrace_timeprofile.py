#!/usr/bin/env python3
"""Summarise an Instruments Time Profiler export (perf audit Thema 99, Schritt 3).

  xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 10s --output t.trace
  xcrun xctrace export --input t.trace \
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > t.xml
  python3 scripts/perf/xctrace_timeprofile.py t.xml [--thread Main] [--top 40]

Prints CPU samples (1 ms each while running) per thread, the P-/E-core split per
thread, and for one thread the inclusive samples per function (a function counts
once per sample however often it recurses) plus the leaf (self) functions.
The export de-duplicates elements with id/ref, which is resolved here.
"""
import argparse
import re
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml")
    ap.add_argument("--thread", default="Main Thread")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--filter", default="", help="only samples whose stack contains this function")
    a = ap.parse_args()

    root = ET.parse(a.xml).getroot()
    ids = {}
    for el in root.iter():
        i = el.get("id")
        if i is not None:
            ids[i] = el

    def res(el):
        r = el.get("ref")
        return ids[r] if r is not None else el

    per_thread = Counter()
    core_split = defaultdict(Counter)
    incl = Counter()
    leaf = Counter()
    n_sel = 0
    for row in root.iter("row"):
        th = row.find("thread")
        core = row.find("core")
        bt = row.find("tagged-backtrace")
        if bt is None:
            bt = row.find("backtrace")
        if th is None or bt is None:
            continue
        th = res(th)
        tname = th.get("fmt", "?")
        tname = re.sub(r" \(0x[0-9a-f]+\)", "", tname).split(" (HorizonEditor")[0]
        per_thread[tname] += 1
        cfmt = res(core).get("fmt", "") if core is not None else ""
        core_split[tname]["P" if "P Core" in cfmt else "E" if "E Core" in cfmt else "?"] += 1
        if a.thread not in tname:
            continue
        bt = res(bt)
        names = []
        for fr in bt.iter("frame"):
            fr = res(fr)
            names.append(fr.get("name", "?"))
        if a.filter and not any(a.filter in n for n in names):
            continue
        n_sel += 1
        if names:
            leaf[names[0]] += 1
        for n in set(names):
            incl[n] += 1

    total = sum(per_thread.values())
    print(f"samples: {total} (1 ms each while on-CPU)")
    print("per thread (samples, P/E core):")
    for t, c in per_thread.most_common(25):
        cs = core_split[t]
        print(f"  {c:6d}  P {cs['P']:5d}  E {cs['E']:5d}  {t}")
    print(f"\n== {a.thread}{' filter ' + a.filter if a.filter else ''}: {n_sel} samples; inclusive by function")
    for n, c in incl.most_common(a.top):
        print(f"  {c:6d} {100.0 * c / max(1, n_sel):5.1f}%  {n[:170]}")
    print(f"\n== leaf (self) functions")
    for n, c in leaf.most_common(25):
        print(f"  {c:6d} {100.0 * c / max(1, n_sel):5.1f}%  {n[:170]}")


if __name__ == "__main__":
    main()
