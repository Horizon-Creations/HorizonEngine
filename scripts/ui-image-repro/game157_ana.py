"""Thema 157 Schritt 4: Auswertung der Spiel-Aufnahmen aus game157_capture.ps1.

Sucht die beiden Image-Elemente ueber ihre Quadrantenfarben (+-TOL), statt feste
Punkte zu nehmen: Canvas 1920x1080 wird aufs Fenster skaliert und D3D12/Vulkan-Fenster
sind DPI-skaliert, feste Koordinaten waeren je Backend andere Stellen.
  centre  : Bild A (400x400, Mitte) -- Quadrantenmitten aus seiner Huellbox gemessen
  rounded : Bild B (200x200, Mitte bei (200,200), Corner Radius 60) -- Ecke muss frei sein
Ohne Textur-Pfad waeren beide Quads weiss (Tint), dann gibt es 0 Bildpixel.

Aufruf: python game157_ana.py <shots> <tag> [Rhi ...]
"""
import re, sys
import numpy as np
from PIL import Image

shots, tag = sys.argv[1], sys.argv[2]
rhis = sys.argv[3:] or ["OpenGL", "Vulkan", "D3D11", "D3D12"]
COLS = {"TL": (230, 30, 30), "TR": (30, 200, 60), "BL": (30, 80, 230), "BR": (240, 220, 40)}
TOL = 8

for rhi in rhis:
    try:
        im = np.asarray(Image.open(f"{shots}\\{tag}_{rhi}.png").convert("RGB")).astype(int)
    except OSError:
        print(f"{rhi:7} keine Aufnahme")
        continue
    h, w, _ = im.shape
    anyimg = np.zeros((h, w), bool)
    for c in COLS.values():
        anyimg |= (np.abs(im - c).max(axis=2) <= TOL)
    white = (im.min(axis=2) >= 250)
    out = [f"{rhi:7} {w}x{h}"]
    for name, (x0, x1) in {"centre": (int(w * .3), int(w * .7)), "rounded": (0, int(w * .2))}.items():
        sub = anyimg[:, x0:x1]
        ys, xs = np.nonzero(sub)
        if len(xs) == 0:
            out.append(f"{name}: KEIN Bild (weisse px im Bereich {int(white[:, x0:x1].sum())})")
            continue
        bx0, bx1, by0, by1 = xs.min() + x0, xs.max() + x0, ys.min(), ys.max()
        bw, bh = bx1 - bx0 + 1, by1 - by0 + 1
        q = {k: tuple(int(v) for v in im[by0 + int(bh * fy), bx0 + int(bw * fx)])
             for k, (fx, fy) in {"TL": (.25, .25), "TR": (.75, .25), "BL": (.25, .75), "BR": (.75, .75)}.items()}
        ok = all(max(abs(a - b) for a, b in zip(q[k], COLS[k])) <= TOL for k in COLS)
        s = f"{name}: box {bw}x{bh}@({bx0},{by0}) " + " ".join(f"{k}={q[k]}" for k in COLS) + (" -> picture" if ok else " -> FALSCH")
        if name == "rounded":
            # Ecke (3 px innen) frei, Kantenmitte Bild: die SDF-Rundung greift.
            corner = anyimg[by0 + 3, bx0 + 3]
            edge = anyimg[by0 + 3, bx0 + bw // 4]
            s += f" corner-free={not corner} edge-img={bool(edge)}"
        out.append(s)
    try:
        log = open(f"{shots}\\{tag}_{rhi}.log", encoding="utf-8", errors="replace").read()
        err = len(re.findall(r"\[ERROR\]", log))
        warn = len(re.findall(r"\[ WARN\]", log))
        val = "validation ENABLED" if re.search(r"validation layer ENABLED|debug layer", log, re.I) else "no-debug-line"
        out.append(f"log: {err} [ERROR], {warn} [ WARN], {val}")
    except OSError:
        out.append("log: fehlt")
    print("  ".join(out))
