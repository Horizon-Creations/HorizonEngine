#!/usr/bin/env python3
"""Vergleicht Profiler-Captures verschiedener Backends auf ihre Per-Pass-GPU-Zeiten.

Erzeugt werden die Dateien vom Zeugen in Application.cpp:

    HE_DUMP_RHI=<Backend> HE_DUMP_PROFILE=<Frames> <deployed HorizonEditor>

Der schreibt <Editor>/dumps/profile_<stamp>.json. Dieses Skript liest mehrere solche
Dateien und prüft, ob die Zielbackends dasselbe MELDEN wie die Referenz.

Was hier NICHT verglichen wird: die Zeiten selbst. Eine Radeon braucht für einen
Bloom-Pass nicht so lange wie eine GeForce, und ein D3D12-Frame verteilt Arbeit
anders als ein GL-Frame. Verglichen werden Pass-NAMEN, der Modus und die
Plausibilität — das ist die Zusicherung, die über Backends hinweg überhaupt
Bestand haben kann.

Aufruf:
    python scripts/he_profile_ab.py OpenGL.json D3D11.json D3D12.json Vulkan.json
    python scripts/he_profile_ab.py --ref OpenGL.json D3D11.json
"""
import argparse
import json
import sys
from collections import Counter

# Modi, die exklusive und additive Pro-Pass-Zeiten liefern. Muss mit der
# Whitelist in src/HE_Editor/ProfilerPanel.cpp übereinstimmen — steht dort ein
# Modus nicht drin, zeigt das Panel korrekte Zahlen unter falschem Etikett an.
ADDITIVE_MODES = {"detailed", "gl-timer", "d3d11-timer", "d3d12-timer", "vulkan-timer"}

# Pässe, die es auf den ZIELBACKENDS (D3D11/D3D12/Vulkan) nachweislich nicht gibt.
# Sie DÜRFEN dort fehlen; sie dürfen aber nicht mit 0.0 ms auftauchen, denn das
# behauptet einen Pass, der gelaufen ist und nichts gekostet hat.
#
# Auf die Referenz wird das NICHT angewandt: OpenGL hat ParticleSim wirklich
# (Transform Feedback, Capabilities::supportsGpuParticles) und meldet ihn zu Recht.
KNOWN_ABSENT = {"ParticleSim", "GIRefl", "GIShadow", "GIPrepass"}


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    backend = d.get("session", {}).get("backend", "?")
    frames = d.get("frames", [])
    return backend, frames


def summarise(frames):
    # Führende Frames ohne gpu-Array sind RING-VORLAUF, kein Fehler: die Ergebnisse
    # eines Slots werden erst kGpuTimerRing Frames später abgeholt, damit das Auslesen
    # die Pipeline nicht anhält. OpenGL umgeht das mit einem glFinish im
    # Detail-Modus, D3D12 und Vulkan haben kein Gegenstück — dort sind die ersten
    # drei bis vier Frames einer Aufnahme also erwartungsgemäß leer. Sie werden hier
    # abgeschnitten und getrennt ausgewiesen, statt die Quote zu verfälschen.
    warmup = 0
    for fr in frames:
        if fr.get("gpu"):
            break
        warmup += 1
    frames = frames[warmup:]

    modes = Counter()
    names = Counter()
    with_gpu = 0
    sum_violations = 0
    bad_values = 0
    for fr in frames:
        m = fr.get("gpuMode") or ""
        modes[m] += 1
        gpu = fr.get("gpu") or []
        if not gpu:
            continue
        with_gpu += 1
        total = 0.0
        for e in gpu:
            n = e.get("n", "")
            ms = e.get("ms", None)
            names[n] += 1
            if ms is None or ms != ms or ms < 0.0 or ms > 1000.0:
                bad_values += 1
            else:
                total += ms
        # Additivität: die Summe der Pässe darf den Frame nicht überschreiten.
        # Das ist die einzige Prüfung, die eine verschachtelte Messung oder einen
        # zu früh gesetzten Zeitstempel auffliegen lässt — beide sehen sonst
        # völlig plausibel aus.
        gf = fr.get("gpuFrameMs", 0.0) or 0.0
        if gf > 0.0 and total > gf * 1.05:
            sum_violations += 1
    return modes, names, with_gpu, sum_violations, bad_values, warmup, len(frames)


def report(path, ref_names=None, is_ref=False):
    backend, frames = load(path)
    modes, names, with_gpu, sum_viol, bad, warmup, n = summarise(frames)
    total = n + warmup
    mode = modes.most_common(1)[0][0] if modes else ""
    print(f"\n=== {backend}  ({path})")
    print(f"    Frames               : {total}  (davon {warmup} Ring-Vorlauf uebersprungen)")
    print(f"    mit gpu-Array        : {with_gpu}/{n}")
    print(f"    Modus                : {mode!r}"
          f"{'  (additiv)' if mode in ADDITIVE_MODES else '  (NICHT additiv)'}")
    print(f"    Pässe                : {len(names)}")
    for nm, cnt in names.most_common():
        print(f"        {nm:<14} in {cnt}/{n}")

    ok = True
    if with_gpu == 0:
        print("    ›› KEINE Per-Pass-Daten — Backend meldet nur Whole-Frame.")
        ok = False
    else:
        if mode not in ADDITIVE_MODES:
            print(f"    ›› FEHLER: Modus {mode!r} steht nicht in der additiven Whitelist.")
            ok = False
        if with_gpu < 0.9 * n:
            print(f"    ›› FEHLER: nur {with_gpu}/{n} Frames tragen Pass-Daten (< 90 %).")
            ok = False
        if bad:
            print(f"    ›› FEHLER: {bad} unplausible ms-Werte (negativ, NaN oder > 1000).")
            ok = False
        if sum_viol:
            print(f"    ›› FEHLER: in {sum_viol} Frames übersteigt die Pass-Summe die Frame-Zeit.")
            print("       Das deutet auf verschachtelte Messung oder einen zu früh")
            print("       gesetzten Zeitstempel hin — beides sieht sonst plausibel aus.")
            ok = False
        if not is_ref:
            zero_absent = [nm for nm in names if nm in KNOWN_ABSENT]
            if zero_absent:
                print(f"    ›› FEHLER: meldet Pässe, die es hier nicht gibt: {zero_absent}")
                ok = False
        if ref_names is not None:
            extra = set(names) - set(ref_names)
            missing = set(ref_names) - set(names) - KNOWN_ABSENT
            if extra:
                print(f"    ›› Hinweis: Pässe ohne Entsprechung in der Referenz: {sorted(extra)}")
            if missing:
                print(f"    ›› Hinweis: Pässe der Referenz, die hier fehlen: {sorted(missing)}")
    return backend, set(names), ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="profile_*.json, erste gilt als Referenz")
    ap.add_argument("--ref", help="ausdrücklich die Referenzdatei")
    args = ap.parse_args()

    ref_path = args.ref or args.files[0]
    others = [f for f in args.files if f != ref_path]

    print("Referenz zuerst; verglichen werden Namen, Modus und Plausibilität —")
    print("NICHT die Zeiten, die zwischen Backends und GPUs nicht vergleichbar sind.")
    _, ref_names, ref_ok = report(ref_path, is_ref=True)
    results = [ref_ok]
    for p in others:
        _, _, ok = report(p, ref_names)
        results.append(ok)

    print()
    if all(results):
        print("ERGEBNIS: alle geprüften Captures tragen additive Per-Pass-Daten.")
        return 0
    print("ERGEBNIS: mindestens ein Capture hat die Prüfung nicht bestanden (siehe oben).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
