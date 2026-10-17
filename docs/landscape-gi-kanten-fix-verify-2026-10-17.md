# Landscape: GI-Striche auf Chunk-Nähten, Fix für Defekt B und Verifikation (Thema 204, Schritt 2)

Stand: 17.10.2026, NN-WS03 (RTX 4070). Ursache: `docs/landscape-gi-kanten-ursache-2026-10-14.md`
(Defekt B), Vorgeschichte: `docs/gi-landscape-tile-seams-verify-2026-10-17.md` (Thema 190).

## Ergebnis in drei Sätzen

Der GI-G-Buffer (Startpunkt der Sonnen-Schattenstrahlen) hat auf D3D11 und D3D12 jetzt
`DXGI_FORMAT_D32_FLOAT` statt `D16_UNORM`, wie Vulkan und Metal; damit gewinnen die Chunk-Skirts
den Tiefentest nicht mehr und die dunklen Striche auf den Chunk-Nähten (auch auf den steilen
Bergkanten zwischen den Landscape-Parts) sind auf D3D weg. Im Zeugen aus Thema 190 ist D3D danach
bis auf ±1 Luma gleich Vulkan, im Projekt des Menschen (echte Texturen) fällt „D3D dunkler als
Vulkan“ am Berg von 4 600 / 12 601 auf 0 / 156 Pixel, und der Fix hellt genau die Nahtlinien auf
und sonst nichts. Die Config des Menschen steht laut Thema 190 auf `RHI: 2` = **D3D11**: dort
gab es diese Striche, auf Vulkan (wo Schritt 1 suchte) nie. Defekt A (vergrabene DDGI-Sonden) und das „zu dunkel/zu harte Schatten“-Thema
aus Schritt 1 sind **nicht** Teil dieses Fixes und bleiben offen.

## Änderung

| Datei | Was |
|---|---|
| `D3D11Renderer.cpp` (G-Buffer-Prepass-Tiefe) | `D16_UNORM` → `D32_FLOAT` |
| `D3D12Renderer.cpp` | neue Konstante `kGiGBufDepthFormat = D32_FLOAT` für die Textur **und** `DSVFormat` beider G-Buffer-PSOs (das instanzierte erbt `pd`) |

Die Tiefe wird nirgends als SRV gelesen (nur DSV + Clear), es gibt keinen weiteren Verbraucher.
Die SSAO-Positions-Prepässe (D3D11 :2341, D3D12 :5162/:5513/:5799, Vulkan :10874) sind ebenfalls
D16, gehören aber nicht zur GI und sind unverändert.

## Messaufbau

Unser Zweig basiert auf `main`; die Zeugen aus Thema 190 (`HE_DUMP_TERRAINTICK`,
`HE_DUMP_NOSTITCH`, `cap190.ps1`, `live190.py`) und das Vernähen gibt es nur auf
`release/0.7.0`. Darum: Scratch-Worktree `C:\hw204src` = `origin/release/0.7.0` (1e41c2d2,
detached, nichts committet), Release-Baum `C:\hw204b\b` (eigener `DEPLOY_DIR`), erst
**ungepatcht** gebaut → `C:\hw204b\before`, dann derselbe Diff (`git apply`) → `C:\hw204b\after`.
Die beiden Deploys unterscheiden sich nur in `HorizonRendering.dll`. Auswertung:
`scripts/landscape-gi-edge-repro/ana204fix.py`.

## Belege

**1. Dump-Zeuge fern** (`cap190.ps1 -Name far -CamX 0 -CamY 360 -CamZ 320 -Pitch -12`,
Berg-Landschaft 4×4 Chunks, GI an). Pixel > 25 Luma **dunkler als Vulkan** desselben Builds:

| Variante | vorher D3D11 / D3D12 / GL | nachher D3D11 / D3D12 / GL |
|---|---|---|
| lod0 | 71 / 71 / 0 | **0 / 0 / 0** |
| nostitch | 321 / 440 / 0 | **0 / 0 / 0** |
| stitched | 614 / 938 / 0 | **0 / 0 / 0** |

Nachher ist D3D − Vulkan über das ganze Bild in [−0,9; +1,0] Luma. Rauschboden: zweiter Lauf
nachher D3D11/D3D12 bitgleich zum ersten (max |d| 0,0), Vulkan 0,6; zweiter Lauf vorher wieder
615 / 752 (der Effekt ist reproduzierbar). Gegenprobe GI aus (vorher): D3D11/D3D12 0 / 0, die
Striche waren also GI. Bild vorher/nachher: `C:\hw204b\shots\far-stitched-D3D11-{before,after}.png`
(vorher Nahtlinien und dunkle Gipfelkante, nachher sauber).

**2. Dump-Zeuge nah** (Standardkamera (15, 360, 45), Regression fürs Vernähen),
`ana190.py`-Strichpixel im Nahtfenster x 360..460 y 280..700:

| | lod0 | nostitch | stitched |
|---|---|---|---|
| vorher D3D11 / D3D12 / Vulkan | 176 / 164 / 164 | 330 / 334 / 330 | 33 / 33 / 33 |
| nachher D3D11 / D3D12 / Vulkan | 164 / 164 / 164 | 330 / 330 / 330 | 33 / 33 / 33 |

Das Vernähen wirkt unverändert (330 → 33), D3D ist nachher in jeder Variante gleich Vulkan;
dunkler-als-Vulkan ganzes Bild vorher 0–75 (D3D11 nostitch 0) → nachher 0.

**3. Projekt des Menschen, Live-Editor, echte Texturen** (`live190.py` mit
`LIVE190_CACHE=%APPDATA%\HorizonEngine\EngineContentCache` wie in Thema 190: 509 MB, die drei
Landscape-Arrays; Kopie `C:\hw204b\proj` von `C:\hw190\proj`, TOD 0,4, deferred, Bodengitter aus,
Editor-Kamera fern). Kennzahl aus Thema 190, Pixel > 25 Luma **D3D dunkler als Vulkan**
(D3D11 / D3D12):

| | flat_close | flat_mid | flat_far | mtn_close | mtn_far |
|---|---|---|---|---|---|
| vorher, ganzes Bild | 12 874 / 16 869 | 10 493 / 12 330 | 18 537 / 22 831 | 4 600 / 4 453 | 12 601 / 10 294 |
| nachher, ganzes Bild | 13 342 / 18 184 | 9 736 / 11 788 | 15 450 / 19 964 | **0 / 0** | **156 / 172** |
| vorher, nur Boden (y ≥ 280) | 1 / 1 | 36 / 16 | 2 838 / 2 736 | | |
| nachher, nur Boden (y ≥ 280) | 1 / 1 | **0 / 0** | **1 / 2** | | |

In den flachen Aufnahmen steht der Himmel mit im Bild, und die Wolken unterscheiden sich
zwischen D3D und Vulkan (vorher wie nachher); auf dem Boden allein sind die gestrichelten
Nahtlinien vorher da und nachher weg (`C:\hw204b\masks\tex_flat_far_d3d11_vs_vk_before_after.png`:
rot = D3D11 > 25 dunkler als Vulkan, links vorher, rechts nachher). Vorher → nachher, Pixel > 25
heller | dunkler: Vulkan 0–6 | 0–4, D3D11 433 … 11 883 | ≤ 13, D3D12 368 … 9 652 | ≤ 19.

**3b. Dasselbe ohne Texturen-Cache** (so lief ich zuerst). Ohne Cache rendert das Projekt auf
`release/0.7.0` mit dem sechseckigen Platzhaltermuster (Thema 210), dessen hartkontrastige
Textur sich zwischen D3D und Vulkan pixelweise unterscheidet (Filterung); „D3D dunkler als
Vulkan“ ist dort zugeschüttet (Editor-Kamera fern ~7 000–20 000 vorher **und** nachher; nah
nachher bis hinunter zu ~730). Gemessen wird dann pro Backend vorher → nachher (nur das
Tiefenformat unterscheidet sich), Pixel > 25 heller | dunkler:

| Editor-Kamera fern | flat_close | flat_mid | flat_far | mtn_close | mtn_far |
|---|---|---|---|---|---|
| Vulkan (Rauschboden) | 1 \| 1 | 0 \| 0 | 7 \| 5 | 2 \| 2 | 11 \| 5 |
| D3D11 | **250** \| 4 | **657** \| 4 | **1 888** \| 14 | **1 841** \| 17 | **5 602** \| 50 |
| D3D12 | **239** \| 6 | **636** \| 4 | **1 870** \| 15 | **1 467** \| 18 | **5 095** \| 46 |
| OpenGL (Kontrolle) | 0 \| 1 | 1 \| 1 | 0 \| 2 | 2 \| 0 | 4 \| 2 |

| Editor-Kamera nah | flat_close | flat_mid | flat_far | mtn_close | mtn_far |
|---|---|---|---|---|---|
| Vulkan | 1 \| 0 | 3 \| 1 | 2 \| 0 | 2 \| 0 | 1 \| 3 |
| D3D11 | 41 \| 0 | 85 \| 0 | 159 \| 1 | 286 \| 0 | 1 221 \| 3 |
| D3D12 | 59 \| 2 | 78 \| 11 | 169 \| 96 | 371 \| 2 | 1 493 \| 2 |

Der Fix hellt nur auf (einseitig), wächst mit der Entfernung (z²-Gesetz der D16-Stufe), und die
aufgehellten Pixel liegen genau auf dem Chunk-Gitter (125 m), auch auf den steilen Bergkanten:
`C:\hw204b\masks\mask_d3d11_far_mtn_far.png` (rot = nachher heller). Der Rest „D3D dunkler als
Vulkan“ nachher sind einzelne Sechseck-Zellen am Hang, keine Linie
(`C:\hw204b\masks\resid_d3d11_vs_vk_after_mtn_far.png`).

**4. Tests.** `he_tests` (Release, gepatchter Scratch-Baum, Scratch-APPDATA): 5386/5390.
Fehler: 2× `test_inspector_ui`-Zwischenablage (bekannt, Sitzung) und 2× `EngineProfiler`
(einzeln zweimal 9/9 grün: Reihenfolge-/Zeit-Flake, kein Renderer-Code). Die D3D11/D3D12-WARP-Fälle
(G-Buffer-/Deferred-PSOs) sind grün. Der `main`-basierte Zweig selbst läuft über CI.

## Backends

| Backend | GI-G-Buffer-Tiefe | Status |
|---|---|---|
| D3D11 | D16 → **D32** | behoben, gemessen (oben) |
| D3D12 | D16 → **D32** | behoben, gemessen (oben) |
| Vulkan | `m_depthFormat` (D32) | unverändert, Referenz und Rauschboden |
| OpenGL | `GL_DEPTH_COMPONENT24` | unverändert, Kontrolle: vorher = nachher, keine Nahtlinien |
| Metal | `kDepthFormat` (D32, `MetalRenderer.mm:8401`) | **nicht gemessen** (kein Mac erreichbar), Code unberührt |

## Offen

- Bestätigung durch den Menschen auf D3D11 (ihre Config), dass das die Linien aus dem Screenshot waren.
- Metal-Aufnahme auf dem Mac (nur Kontrolle, der Diff berührt Metal nicht).
- Defekt A (DDGI: vergrabene Sonden ohne Surface-Bias, alle Backends) und die zu dunklen,
  harten Schattenflanken aus Schritt 1: nicht Teil dieses Schritts.
- Der Fix muss auch auf `release/0.7.0` (dort zeilenverschoben, `git apply` geht sauber).
- Beobachtet, nicht verfolgt: das Sechseckmuster rendert auf D3D und Vulkan pixelweise
  verschieden; im Dump-Zeugen ist OpenGL deutlich heller als die anderen (GL-GI im Zeugen ein
  Nullvergleich, siehe Lektion zu cap158auto).
