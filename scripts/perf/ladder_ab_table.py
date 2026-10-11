#!/usr/bin/env python3
"""Old/new ladder comparison for Thema 162 step 5, from world_streaming_ladder.sh runs.

One group per build, each group several rounds of the same ladder (labels <prefix>-<entities>):

  python3 scripts/perf/ladder_ab_table.py <rawdir> old=r1old,r2old,r3old new=r1new,r2new,r3new \\
      full=r3full

Per size and group it prints the CPU time per frame (p50 of the steady frames, mean over the rounds,
with the spread), then the scopes that moved, then the calls per frame. The first group is the
reference of the delta columns. Timings of a scope that nested differently in the two builds are
summed the way 8.5 of docs/render-extractor-shadow-pass-plan.md asks for:

  walk + shadow  = Metal::EncodeShadowMap (old) / Metal::ExtractFrame + Metal::EncodeShadowMap (new)

Only the per-frame sums are compared, never one scope across the change. Absolute values are only
comparable inside one session (same load, same power state). Reads <label>.profile.json, which the
ladder writes next to the summaries but git ignores (the committed tables are the evidence).
"""
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ladder_table import pct, scope_sum  # noqa: E402


def load(raw, prefix, n):
    p = raw / f"{prefix}-{n}.profile.json"
    return json.loads(p.read_text())["frames"][2:] if p.exists() else None


def frame_sum(frame, names):
    return sum(scope_sum(frame, n) for n in names)


def p50(frames, names):
    return pct([frame_sum(f, names) for f in frames], 50)


def calls(frames, name):
    return statistics.median(sum(1 for s in f["cpu"] if s["n"] == name) for f in frames)


def present(frames, name):
    return any(s["n"] == name for f in frames for s in f["cpu"])


def main():
    raw = Path(sys.argv[1])
    groups = []
    for a in sys.argv[2:]:
        name, prefixes = a.split("=")
        groups.append((name, prefixes.split(",")))
    sizes = sorted({int(p.name.rsplit("-", 1)[1].split(".")[0])
                    for p in raw.glob("*.profile.json")})

    def cell(vals):
        if not vals:
            return "-"
        return f"{statistics.mean(vals):.1f} ({min(vals):.1f} bis {max(vals):.1f})"

    def delta(ref, vals):
        if not ref or not vals:
            return "-", "-"
        d = statistics.mean(vals) - statistics.mean(ref)
        return f"{d:+.1f}", f"{100 * d / statistics.mean(ref):+.0f} %"

    hdr = ["Entities"] + [f"{g} ({len(p)} Läufe)" for g, p in groups]
    for g, _ in groups[1:]:
        hdr += [f"Δ {g} ms", f"Δ {g} %"]
    print("CPU je Frame, p50 (ms), Mittel der Läufe (kleinster bis größter):\n")
    print("| " + " | ".join(hdr) + " |")
    print("|" + "---|" * len(hdr))
    for n in sizes:
        cols = []
        for _, prefixes in groups:
            vals = []
            for pre in prefixes:
                p = raw / f"{pre}-{n}.profile.json"
                if p.exists():
                    fr = json.loads(p.read_text())["frames"][2:]
                    vals.append(pct([f["cpuMs"] for f in fr], 50))
            cols.append(vals)
        ent = json.loads((raw / f"{groups[0][1][0]}-{n}.profile.json").read_text())["frames"][-1]["stats"].get("entities")
        row = [f"{ent}"] + [cell(c) for c in cols]
        for c in cols[1:]:
            row += list(delta(cols[0], c))
        print("| " + " | ".join(row) + " |")

    rows = [
        ("RenderExtractor::extract (Summe je Frame)", ["RenderExtractor::extract"]),
        ("Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap)",
         None),
        ("Metal::EncodeSSAO", ["Metal::EncodeSSAO"]),
        ("Metal::EncodeScene", ["Metal::EncodeScene"]),
        ("FrustumCuller::cull (Summe je Frame)", ["FrustumCuller::cull"]),
        ("OnRender", ["OnRender"]),
    ]
    print("\nScopes, p50 je Frame (ms), Mittel der Läufe:\n")
    print("| Entities | Scope | " + " | ".join(g for g, _ in groups) + " |")
    print("|" + "---|" * (2 + len(groups)))
    for n in sizes:
        ent = json.loads((raw / f"{groups[0][1][0]}-{n}.profile.json").read_text())["frames"][-1]["stats"].get("entities")
        for label, names in rows:
            cells = []
            for _, prefixes in groups:
                vals = []
                for pre in prefixes:
                    fr = load(raw, pre, n)
                    if fr is None:
                        continue
                    if names is None:
                        nm = (["Metal::ExtractFrame", "Metal::EncodeShadowMap"]
                              if present(fr, "Metal::ExtractFrame") else ["Metal::EncodeShadowMap"])
                    else:
                        nm = names
                    vals.append(p50(fr, nm))
                cells.append(f"{statistics.mean(vals):.1f}" if vals else "-")
            print(f"| {ent} | {label} | " + " | ".join(cells) + " |")

    print("\nAufrufe je Frame (Median über die Frames, erster Lauf der Gruppe):\n")
    cnt = ["RenderExtractor::extract", "RenderExtractor::reuse", "FrustumCuller::cull",
           "Metal::RefineBounds", "Metal::ExtractFrame", "Transforms::scan", "Transforms::propagate"]
    print("| Entities | Gruppe | " + " | ".join(cnt) + " |")
    print("|" + "---|" * (2 + len(cnt)))
    for n in sizes:
        ent = json.loads((raw / f"{groups[0][1][0]}-{n}.profile.json").read_text())["frames"][-1]["stats"].get("entities")
        for g, prefixes in groups:
            fr = load(raw, prefixes[0], n)
            if fr is None:
                continue
            print(f"| {ent} | {g} | " + " | ".join(
                f"{calls(fr, c):g}" if present(fr, c) else "-" for c in cnt) + " |")


if __name__ == "__main__":
    main()
