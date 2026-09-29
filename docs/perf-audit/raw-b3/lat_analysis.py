#!/usr/bin/env python3
"""Auswertung der B3-Latenzläufe (Thema 104, Schritt 2).

Liest aus jedem lat-*.log die Abschlusszeile von HE_PERF_INPUT_LATENCY_HZ
(Eingabealter pro Event) und, falls die *.profile.json danebenliegen (nicht
eingecheckt, ~5 MB je Lauf), den Takt der Drawable-Wartezeit pro Frame:

  - Lag-1-Korrelation der Wartezeit W (lang/kurz im Wechsel?)
  - W des Folgeframes nach einem langen Warten (> 30 ms)
  - Anteil Frames mit Abstand < 8 ms / > 50 ms zum Vorframe
  - Vorhersage der Ersparnis pro Event aus dem späten Lauf: ein Event, das im
    Intervall deltaMs[i] (vor Frame i) ankommt, wird von Frame i abgeholt;
    früh spart es dessen Warten W[i]. Mittel = sum(deltaMs[i]*W[i])/sum(deltaMs).

  python3 docs/perf-audit/raw-b3/lat_analysis.py
"""
import glob
import json
import os
import re
import statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
LINE = re.compile(r"HE_PERF_INPUT_LATENCY \(final\): (\d+) events over (\d+) frames .*?pushed (\d+) Hz\) \| "
                  r"event->commit p50 ([\d.]+) p90 ([\d.]+) p99 ([\d.]+) ms \| "
                  r"event->poll p50 ([\d.]+) p90 ([\d.]+) \| poll->commit p50 ([\d.]+) p90 ([\d.]+)")


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def corr(a, b):
    ma, mb = st.mean(a), st.mean(b)
    den = (sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b)) ** 0.5
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / den if den else 0.0


def main():
    print("| Lauf | Events | Hz | Event→Commit p50 / p90 / p99 | Event→Poll p50 / p90 | Poll→Commit p50 / p90 |")
    print("|---|---|---|---|---|---|")
    for log in sorted(glob.glob(os.path.join(HERE, "lat-*.log"))):
        m = None
        for line in open(log, encoding="utf-8", errors="replace"):
            m = LINE.search(line) or m
        name = os.path.basename(log)[:-4]
        if not m:
            print(f"| {name} | (keine Abschlusszeile) |")
            continue
        g = m.groups()
        print(f"| {name} | {g[0]} | {g[2]} | {g[3]} / {g[4]} / {g[5]} | {g[6]} / {g[7]} | {g[8]} / {g[9]} |")

    print()
    for prof in sorted(glob.glob(os.path.join(HERE, "lat-*.profile.json"))):
        fr = json.load(open(prof))["frames"]
        W = [sum(c["ms"] for c in f["cpu"] if c["n"] == "Metal::NextDrawable") for f in fr]
        L = [f["deltaMs"] for f in fr]
        n = len(W)
        after = [W[i + 1] for i in range(n - 1) if W[i] > 30]
        line = (f"{os.path.basename(prof)[:-13]}: W p50 {pct(W, 50):.1f} p90 {pct(W, 90):.1f} "
                f"| lag1 {corr(W[:-1], W[1:]):+.2f} | W nach W>30: p50 {pct(after, 50) if after else 0:.1f} "
                f"| deltaMs <8 {100 * sum(x < 8 for x in L) / n:.0f}% >50 {100 * sum(x > 50 for x in L) / n:.0f}%")
        if "early0" in prof:
            line += f" | Vorhersage Ersparnis/Event {sum(l * w for l, w in zip(L, W)) / sum(L):.2f} ms"
        print(line)


if __name__ == "__main__":
    main()
