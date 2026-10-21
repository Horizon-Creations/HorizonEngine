# Auto-Landscape-Material 2.0: Performance-Audit (Thema 180, Schritt 6)

Stand 21.10.2026, MacBook Air M5 (10 Kerne, 24 GB), Metal. Bezug: `../auto-landscape-material-2-plan.md` (Schritt 6), `../auto-landscape-material-2-variants-2026-10-21.md`,
`../auto-landscape-material-2-instancing-2026-10-21.md`, `../auto-landscape-material-bcn-2026-10-21.md`. Rohdaten: `raw-auto-landscape-2/` (`*.summary.json`, `*.scopes.json`,
`*.conditions.txt`, `*.log`, `tables.md` = die Tabellen dieses Dokuments, `bench-array-formats.txt`, `binaries.txt`, `ci-run-39532559913-excerpt.txt`). Bild: `img-auto-landscape-2/live_s128_x4.png`.
**Alle Zeiten sind M5/Metal. Direct3D 11, Direct3D 12 und Vulkan wurden nicht gemessen** (kein Windows-Rechner online, NN-WS03 offline); was von dort kommt, steht in Abschnitt 8 und ist als CI gekennzeichnet.

## 0. Kurzfassung

| Frage | Antwort (M5, Metal) |
|---|---|
| Kosten der Layer-Meshes | **CPU, nicht GPU.** `Render` +0,3 / +0,9 / +3,4 ms je Frame bei x1 / x4 / x16 auf 128 m (6k / 25k / 102k gespeicherte Instanzen); 70 % davon `ExtractFoliage`. GPU (Pass-p50, Scene+Shadow+SSAO) +0,3 / +0,6 / +1,5 ms. |
| Draw-Zahl | **flach**: 2 Draws ohne Meshes, 13 bei x1, x4 und x16 (128 m); 8 / 21 / 22 auf 512 m. Dreiecke wachsen 17k → 995k. Dasselbe Bild auf Vulkan/lavapipe in der CI (2 / 13 / 13). |
| Variation (V = 3 statt 1) | Scene-Pass +0,4 ms (128 m, im Streubereich) bis **+1,9 ms (512 m, +12 %)**, 2,6x so viele Slices; CPU unverändert. Arrays 335 → 873 MB. |
| ASTC / BC7 / BC3 statt RGBA8 (V = 3) | Arrays **873 → 218 MB (4:1)**, Laden 4 bis 6x schneller (420 ms → 65 bis 105 ms je Array), Start der Szene 2,1 s → 0,9 s, RSS-Spitze 0,8 GB statt 0,8 bis 2,1 GB. Kein GPU-Nachteil. Encoder-Zeit (einthreadig): ASTC 66 bis 320 s je Array, BC7 ~85 s, BC3 6,5 s. |
| Gefunden und behoben | (1) **BC1/BC3/BC4/BC5-Arrays kosteten 440 ms je Frame** (GI-Mittelwerte dekodierten jedes Frame das ganze Array), jetzt 0; (2) die GI-Mittelwerte lasen bei V = 3 die falschen Slices; (3) der Windows-Schritt der CI für Layer-Meshes starb vor dem ersten Bild an `cp1252`. |
| Größter offener Engpass | `ExtractFoliage` rechnet jedes Frame neu, auch bei stehender Kamera (2,2 ms bei 31k Instanzen in Reichweite); einmalige Streuung 0,3 bis 0,4 µs je Instanz (125 ms bei 425k). |

## 1. Wie gemessen wurde

- **Binary:** ein Build für alle Zeilen, Hashes in `raw-auto-landscape-2/binaries.txt` (Editor `23ead1b74880`, `libHorizonRendering` `71f981c7681a`). Nach dem Fix in Abschnitt 7 ist
  `libHorizonRendering` `39a6b289cafa`; die Format-Läufe (Tabelle 4) laufen auf dem neuen, die Mesh- und Variantenläufe auf dem alten Stand. Eine Kontrolle auf dem neuen
  (`after-gi-slice-means-fix/`, Scene p50 12,95 / 13,13 / 16,22 ms gegen 12,71 / 13,17 / 16,41 ms vorher) zeigt, dass der Fix nichts anderes berührt.
- **Szene:** `scripts/perf/gen_autoland_scene.py` schreibt eine quadratische Landschaft (128 m oder 512 m, rollendes Relief, Steigung bis 1-N.y ~0,3), das **ausgelieferte** Auto-Material
  (`kAutoLandscapeMaterialId`) und die fünf Preset-Ebenen aus `LayerMeshPresets.h` (Tuft, Pebble, Rock A/B/C; Snow Drift fehlt, die Schneegrenze 60 m liegt über dem Relief) mit Dichte x1 / x4 / x16
  des Presets. „aus“ = dieselbe Landschaft ohne Foliage-Komponente. Der Editor öffnet sie im echten Loop (`he_perf_capture.py --scene`), Kamera 15 m über dem Boden, feste Pose, Editor-Bodengitter aus,
  vsync aus, 200 Frames Warmup, 600 Frames. Bild: `img-auto-landscape-2/live_s128_x4.png` (Felsen mit Schatten, Büschel, Kiesel auf dem Auto-Material). Der Dump-Zeuge reicht nicht: er tickt die Welt nicht.
- **Läufe:** `scripts/perf/autoland_matrix.py` (Gruppen `mesh`, `var`, `detail`, `mem`), je Zelle 3 Wiederholungen, die Reihenfolge der Zellen wechselt je Wiederholung (vorwärts, rückwärts, vorwärts), also ABAB-artig.
  `--no-counters` für die Ganzframe-Läufe, `--detailed` (Zähler an) für die Pass-Tabellen. Tabellen: `scripts/perf/autoland_table.py` (Median über die Wiederholungen, Streubereich daneben). `scripts/he_perf_tables.py` läuft auf demselben Ordner (`he_perf_tables-output.md`, Tabelle A/B je Lauf); seine Spalte „Display wach“ ist ein Namensraten nach Label-Präfix und stimmt hier nicht, der Bildschirm war gesperrt.
- **Bedingungen** (`*.conditions.txt`, 125 Läufe mit Ergebnis, dazu 2 OpenGL-Läufe im Timeout): Netzteil, `lowpowermode 0`, **Bildschirm gesperrt** (`CGSSessionScreenIsLocked=Yes` in jedem Lauf, wie in den Audits davor), Last 1,4 bis 5,3 (der Editor selbst),
  vor und nach jedem Lauf kein Compiler, Linker oder zweiter Editor (der Treiber prüft das und wiederholt sonst; es musste kein Lauf verworfen werden). Eine Bee auf dem Gerät (`hive_devices`: 9,5 % CPU, 52 °C vor dem Start).
  Ein Fehlstart: der erste Szenengenerator ließ `castsShadow`/`contributesAO` der Büschel und Kiesel auf dem Default `true` (der Serializer schreibt nur Abweichungen), die Läufe mit Meshes waren damit
  nicht das Preset; erkannt an „shadow pass instanced (first run: 237 casters)“ statt 23, alle Läufe mit Meshes gelöscht und mit korrigierter Szene wiederholt.
- **Die Falle bei der GPU-Zeit** (wichtig für jeden, der das wiederholt): bei gesperrtem Bildschirm drosselt das Fenster die Präsentation (`Metal::NextDrawable` p90 ~60 ms), die GPU wartet zwischen den Frames
  und taktet herunter. Bei billigen Frames ist die gemessene GPU-Zeit dann **aufgebläht**: Tabelle 3 (RenderScale 1, in `raw-auto-landscape-2/tables.md`) zeigt für 128 m ohne Meshes einen Scene-Pass von 7,7 ms, für 512 m 12,2 ms, und die
  festen Pässe (Tonemap, Bloom) schwanken mit (0,29 gegen 0,54 ms). Darum: **RenderScale 2** (4x die Pixel, die GPU ist ausgelastet und taktet durch) und **Pass-p50 aus dem Detailed-Capture**,
  Tonemap als Takt-Zeuge (0,35 bis 0,40 ms in allen Zeilen der Tabellen 3b, also gleicher Takt). Die Ganzframe-Spanne `gpuMs` (Tabellen 1b/2b) enthält bei ausgelasteter GPU Wartezeit (28 ms gegen 15,4 ms Σ Pässe
  für dieselbe Szene) und ist nur als Differenz lesbar. CPU-Zahlen (`Render`, `ExtractFoliage`) sind vom Takt nicht betroffen.

## 2. Layer-Meshes an und aus

Tabelle 3b (Detailed, RenderScale 2, Median aus 3): GPU-Zeit je Pass, Δ gegen „off“ derselben Größe. Cluster = `RenderObject`s der Foliage, Instanzen = in Reichweite / gespeichert (aus `FoliageExtract` im Log).

| Zelle | Läufe | Shadow p50 (p10) | SSAO p50 (p10) | Scene p50 (p10) | Bloom p50 (p10) | Tonemap p50 (p10) | Present p50 (p10) | Scene p50 min..max | Δ Scene ms | GPU Σ Pässe p50 | Δ GPU ms | Cluster | Instanzen in Reichweite / gespeichert | tickWorld Frame 3 (einmalig) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s128-off | 3 | 0.18 (0.16) | 0.94 (0.88) | 12.71 (12.13) | 0.83 (0.80) | 0.35 (0.34) | 0.27 (0.25) | 12.51..12.91 | Basis | 15.37 | Basis | 0 | 0 / 0 | – |
| s128-x1 | 3 | 0.19 (0.18) | 0.97 (0.90) | 12.97 (12.28) | 0.86 (0.80) | 0.36 (0.35) | 0.28 (0.26) | 12.88..13.08 | +0.26 | 15.66 | +0.29 | 68 | 1938 / 6361 | – |
| s128-x4 | 3 | 0.32 (0.22) | 0.99 (0.91) | 13.17 (12.38) | 0.87 (0.81) | 0.38 (0.37) | 0.28 (0.26) | 13.02..13.27 | +0.45 | 16.01 | +0.64 | 86 | 7695 / 25510 | – |
| s128-x16 | 3 | 0.57 (0.51) | 1.06 (0.99) | 13.72 (13.17) | 0.89 (0.84) | 0.40 (0.38) | 0.28 (0.26) | 13.70..13.77 | +1.01 | 16.91 | +1.54 | 93 | 30838 / 101983 | 31.2 |
| s128-x4ns | 3 | 0.19 (0.17) | 0.97 (0.89) | 13.21 (12.31) | 0.87 (0.80) | 0.38 (0.37) | 0.28 (0.26) | 13.20..13.26 | +0.50 | 15.96 | +0.58 | 86 | 7695 / 25510 | – |
| s512-off | 3 | 0.23 (0.23) | 1.18 (1.17) | 15.96 (15.83) | 0.82 (0.81) | 0.36 (0.35) | 0.26 (0.25) | 15.95..15.97 | Basis | 18.84 | Basis | 0 | 0 / 0 | – |
| s512-x1 | 3 | 0.24 (0.24) | 1.19 (1.18) | 16.03 (15.87) | 0.83 (0.82) | 0.37 (0.37) | 0.26 (0.25) | 15.86..16.06 | +0.07 | 18.96 | +0.13 | 148 | 2327 / 106228 | 44.0 |
| s512-x4 | 3 | 0.47 (0.30) | 1.21 (1.20) | 16.41 (16.26) | 0.82 (0.82) | 0.39 (0.39) | 0.26 (0.25) | 16.15..16.43 | +0.44 | 19.58 | +0.74 | 203 | 9258 / 425343 | 125.6 |
| s512-x4ns | 3 | 0.41 (0.23) | 1.18 (1.17) | 16.28 (16.13) | 0.82 (0.82) | 0.40 (0.39) | 0.26 (0.25) | 16.22..16.28 | +0.32 | 19.36 | +0.52 | 203 | 9258 / 425343 | 133.6 |
| s128-off-V3 | 3 | 0.17 (0.16) | 0.90 (0.88) | 13.16 (13.01) | 0.80 (0.80) | 0.36 (0.35) | 0.26 (0.25) | 13.12..13.23 | +0.45 | 15.65 | +0.28 | 0 | 0 / 0 | 26.4 |
| s512-off-V3 | 3 | 0.23 (0.23) | 1.19 (1.17) | 17.88 (17.73) | 0.83 (0.82) | 0.39 (0.38) | 0.26 (0.25) | 17.86..17.89 | +1.91 | 20.80 | +1.96 | 0 | 0 / 0 | 16.1 |
| s128-x4-V3 | 3 | 0.36 (0.23) | 0.93 (0.90) | 13.18 (12.98) | 0.81 (0.80) | 0.37 (0.36) | 0.26 (0.25) | 13.12..13.23 | +0.01 | 15.87 | -0.14 | 86 | 7695 / 25510 | – |

CPU je Frame (Tabelle 1b, `Render` p50; `Extract+Tick` = `ExtractFoliage` + `Foliage`; Spalten Shadow/Scene enc = `Metal::EncodeShadowMap` / `EncodeScene`), Ganzframe-GPU nur als Differenz:

| Zelle | Läufe | GPU p50 ms (min..max) | Δ GPU ms / % | Render p50 ms | Δ Render ms | Extract+Tick p50 ms | Shadow enc p50 ms | Scene enc p50 ms | draws | tris | RSS max / last MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| s128-off | 3 | 28.18 (28.16..28.22) | Basis | 1.00 | Basis | 0.001 | 0.14 | 0.11 | 2 | 17408 | 1082 / 940 |
| s128-x1 | 3 | 27.91 (27.87..27.96) | -0.27 / -1.0 % | 1.30 | +0.30 | 0.162 | 0.19 | 0.18 | 13 | 76356 | 1085 / 1077 |
| s128-x4 | 3 | 27.35 (27.26..27.41) | -0.83 / -3.0 % | 1.93 | +0.92 | 0.572 | 0.22 | 0.34 | 13 | 256636 | 1092 / 1092 |
| s128-x16 | 3 | 25.45 (25.45..25.48) | -2.73 / -9.7 % | 4.38 | +3.38 | 2.364 | 0.30 | 0.90 | 13 | 995076 | 1110 / 1110 |
| s128-x4ns | 3 | 27.29 (27.28..27.34) | -0.89 / -3.2 % | 1.86 | +0.85 | 0.577 | 0.16 | 0.34 | 13 | 256636 | 1074 / 940 |
| s512-off | 3 | 36.23 (36.21..36.30) | Basis | 1.05 | Basis | 0.001 | 0.15 | 0.12 | 8 | 69632 | 1094 / 1094 |
| s512-x1 | 3 | 35.63 (35.44..35.79) | -0.60 / -1.7 % | 1.51 | +0.47 | 0.247 | 0.23 | 0.22 | 21 | 131276 | 1104 / 1092 |
| s512-x4 | 3 | 35.24 (35.16..35.27) | -0.99 / -2.7 % | 2.37 | +1.32 | 0.794 | 0.27 | 0.45 | 22 | 351696 | 1155 / 1149 |
| s512-x4ns | 3 | 35.37 (35.26..35.37) | -0.86 / -2.4 % | 2.29 | +1.25 | 0.800 | 0.22 | 0.45 | 22 | 351696 | 1159 / 1155 |

- **GPU:** x1 kostet 0,1 bis 0,3 ms, x4 0,5 bis 0,7 ms, x16 (100k Instanzen, 1 Mio. Dreiecke) 1,5 ms von rund 16 ms. Scene +1,0, Shadow +0,4, SSAO +0,1 ms bei x16. Die Ganzframe-Spanne fällt mit den Meshes sogar (−0,8 ms bei x4, −2,7 ms bei x16);
  eine Erklärung, nicht gemessen: auf der Apple-GPU (Tile-Deferred, Hidden-Surface-Removal) ersetzen opake Mesh-Pixel das teure Auto-Material des Bodens (36 Texturzugriffe) in den Pixeln, die sie überdecken.
- **CPU:** `Render` +0,3 / +0,9 / +3,4 ms (128 m) und +0,5 / +1,3 ms (512 m, x1 / x4). Von +3,2 ms bei x16 sind **2,24 ms `ExtractFoliage`**, 0,75 ms `EncodeScene`, 0,16 ms `EncodeShadowMap`,
  0,02 ms `FrustumCuller::cull` (`*.scopes.json`). Das Culling kostet praktisch nichts: 203 Cluster tragen 9 258 Instanzen in Reichweite aus 425 343 gespeicherten.
- **Schatten der Felsen:** `x4ns` (kein Mesh wirft Schatten, kein AO) gegen `x4`: Shadow-Pass 0,19 gegen 0,32 ms (128 m), 0,41 gegen 0,47 ms (512 m, Streuung 0,23..0,30 p10). Die Preset-Schatten (nur Felsen, 23 Werfer) sind billig.

## 3. Draw-Zahl bei Dichte 1 und 4

Live im Editor (`counters_midframe` der Läufe, Metal; ohne Meshes sind es Himmel + Gelände):

| Größe | Dichte | gespeichert | in Reichweite | Cluster | draws | Dreiecke |
|---|---|---|---|---|---|---|
| 128 m | aus | 0 | 0 | 0 | 2 | 17 408 |
| 128 m | x1 | 6 361 | 1 938 | 68 | **13** | 76 356 |
| 128 m | x4 | 25 510 | 7 695 | 86 | **13** | 256 636 |
| 128 m | x16 | 101 983 | 30 838 | 93 | **13** | 995 076 |
| 512 m | aus | 0 | 0 | 0 | 8 | 69 632 |
| 512 m | x1 | 106 228 | 2 327 | 148 | 21 | 131 276 |
| 512 m | x4 | 425 343 | 9 258 | 203 | 22 | 351 696 |

Die Draw-Zahl hängt an der Zahl der Meshes und Detailstufen in Sicht (höchstens 14 Dateien), nicht an den Instanzen. Auf 512 m kommen 8 bis 9 Draws für die Geländechunks dazu. Die Schritt-5-Zählung (Dump, `he_shot.py`: 47 / 49 / 49 Draws bei x1 / x4 / x16, Metal = OpenGL)
zeigt dasselbe; `tests/test_layer_mesh_instancing.cpp` hält es als Test.

## 4. Variation an und aus

Es gibt keinen Laufzeitschalter: V steht in der Tiefe der Arrays (`4 V + 1` Slices, das Material rechnet `V = Tiefe / 4`). „Aus“ = die ausgelieferten 5-Slice-Arrays (2K, RGBA8, 3 x 111,8 MB), „an“ = 13-Slice-Arrays gleicher Größe
(`landscape_tex_gen --arrays-only --variants 3`, 3 x 290,8 MB; die Variantenslices sind die Texturen der Nachbarschicht, nicht Fotos), als Projekt-Override `Content/Engine/Textures/Landscape/` (Beleg: 38,6 % der Pixel unterscheiden sich vom V = 1-Bild).
Beide Seiten laufen im selben Editor, nur das Projekt wechselt.

Detailed-Pässe (Zeilen `-V3` in Tabelle 3b, Δ gegen dieselbe Szene mit V = 1): Scene **+0,45 ms (128 m, ohne Meshes; p10 +0,9)**, **+1,91 ms (512 m, +12 %)**, +0,01 ms (128 m mit x4). Ganzframe (Tabelle 2b, RenderScale 2):

| Zelle | Läufe | GPU p50 ms (min..max) | Δ GPU ms / % | Render p50 ms | Δ Render ms | Extract+Tick p50 ms | Shadow enc p50 ms | Scene enc p50 ms | draws | tris | RSS max / last MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| s128-off-V1 | 3 | 28.14 (28.05..28.27) | Basis | 1.00 | Basis | 0.001 | 0.14 | 0.11 | 2 | 17408 | 1082 / 1074 |
| s128-off-V3 | 3 | 30.52 (30.45..30.54) | +2.38 / +8.5 % | 1.01 | +0.01 | 0.001 | 0.14 | 0.11 | 2 | 17408 | 2067 / 1529 |
| s512-off-V1 | 3 | 36.27 (36.22..36.31) | Basis | 1.05 | Basis | 0.001 | 0.15 | 0.12 | 8 | 69632 | 1091 / 1091 |
| s512-off-V3 | 3 | 40.79 (40.59..40.82) | +4.52 / +12.5 % | 1.03 | -0.01 | 0.001 | 0.15 | 0.12 | 8 | 69632 | 2269 / 1817 |
| s128-x4-V1 | 3 | 27.28 (27.20..27.31) | Basis | 1.93 | Basis | 0.572 | 0.22 | 0.34 | 13 | 256636 | 1091 / 1090 |
| s128-x4-V3 | 3 | 29.26 (29.24..29.33) | +1.97 / +7.2 % | 1.90 | -0.03 | 0.568 | 0.21 | 0.34 | 13 | 256636 | 1695 / 1561 |

CPU (`Render`, `Extract`) ändert sich nicht (±0,03 ms). Die Zahl der Texturzugriffe ist gleich (36, aus `heBombGridV`); es wächst der Speicherbereich, aus dem gelesen wird, und mit der Entfernung (512 m, Gelände weit weg, stark verkleinert) wächst der Preis.
Wenn Meshes den Boden teils verdecken, bleibt davon bei 128 m nichts übrig. Speicher: Arrays 335 → 873 MB (Faktor 2,6), RSS-Spitze im Lauf 1,08 GB → 2,1 bis 2,3 GB (Tabelle 2b, die Spalte ist wegen der Speicherkompression von macOS unscharf, siehe Abschnitt 5).

## 5. Speicher und Ladezeit: ASTC, BC7, BC3 gegen RGBA8

**Größen** (exakt, `textureLevelBytes`; `bench-array-formats.txt`): ein 2K-Array mit 13 Slices und 12 Mips: RGBA8 290,8 MB; ASTC 4x4, BC7, BC3 und BC5 72,7 MB (4:1); BC1 und BC4 36,4 MB (8:1). Die drei Arrays: 873 MB gegen 218 MB (bzw. 109 MB mit BC1/BC4).
Das V = 1-Set (5 Slices): 335 MB RGBA8, 84 MB komprimiert.

**Encoder** (`tests/test_bench_landscape_arrays.cpp`, Qualität 0, einthreadig, der echte Pack-Cook mit `cookTextureArrays`; Zeit für ein ganzes Array, 156 Slice-Ebenen):

| Format | Albedo | Normal | Mask |
|---|---|---|---|
| ASTC 4x4 | 66 s | 320 s | 158 s |
| BC7 | 81 s | 87 s | 85 s |
| BC3 | 6,5 s | 6,7 s | 6,4 s |
| BC1 / BC4 / BC5 | 2,7 / 2,3 / 1,9 s | 2,7 / 2,7 / 4,3 s | 5,8 / 3,3 / 4,1 s |

**Laden** (dieselbe Messung; Laden aus der losen `.hasset` bzw. aus dem Pak): RGBA8 412 / 446 / 419 ms je Array (Albedo / Normal / Mask), komprimiert 65 bis 105 ms lose und 96 bis 176 ms aus dem Pak.

**Im Editor auf Metal** (die komprimierten Arrays als loses Projekt-Override, `mem`-Gruppe, Szene 128 m ohne Meshes, RenderScale 2, Detailed). Die Bilder weichen vom RGBA8-Bild um 0,6 (ASTC), 0,8 (BC7), 1,3 (BC3) mittlere Kanalstufen ab (0,05 / 0,39 / 1,21 % der Pixel über 8):

| Format (V = 3, 3 Arrays) | Läufe | Arrays im Speicher MB | RSS-Spitze je Lauf, min..max MB (Median) | Scene-Pass p50 ms | Tonemap p50 ms (Takt-Zeuge) | Scene / Tonemap | GPU Σ Pässe p50 ms | CPU-Frame p50 ms | Slow-Frames 2-6 Σ ms (Start: Terrain, Upload, Warm-up) |
|---|---|---|---|---|---|---|---|---|---|
| rgba8 | 3 | 873 (3 x 290,8) | 806..2139 (1217) | 13.16 | 0.36 | 36.9 | 15.66 | 19.14 | 2134 |
| astc | 3 | 218 (3 x 72,7) | 817..818 (818) | 11.72 | 0.59 | 19.9 | 16.06 | 20.01 | 885 |
| bc7 | 3 | 218 (3 x 72,7) | 818..818 (818) | 11.44 | 0.52 | 21.9 | 15.58 | 19.34 | 896 |
| bc3 | 3 | 218 (3 x 72,7) | 818..818 (818) | 11.62 | 0.59 | 19.8 | 15.99 | 20.01 | 864 |

- **Speicher:** 873 → 218 MB Arrays; RSS-Spitze 818 MB, in jedem der 9 komprimierten Läufe gleich. Die RGBA8-Zeile schwankt von 806 bis 2 139 MB (Median 1 217): der Sampler (`ps`, 1 Hz) sieht auf macOS nur die nicht komprimierten Seiten, und der Zeitpunkt entscheidet. Verlässlich sind die Array-Bytes, nicht die RSS-Zeile.
- **Start:** die ersten Slow-Frames (Terrain, Datei laden, Upload, Warm-up) summieren 2,13 s mit RGBA8 und 0,86 bis 0,90 s komprimiert.
- **GPU:** Scene-Pass 13,2 ms (RGBA8) gegen 11,4 bis 11,7 ms. Die festen Pässe liefen in den komprimierten Läufen mit niedrigerem Takt (Tonemap 0,52 bis 0,59 gegen 0,36 ms), die Spalte Scene / Tonemap (37 gegen 20 bis 22) deutet auf mehr Gewinn, ist aber nicht belastbar. Belastbar: **komprimiert ist nicht langsamer.**
- **ASTC, BC7 und BC3 sind auf dieser GPU gleichwertig;** BC7/ASTC haben keinen CPU-Decoder (siehe Abschnitt 7: nur deshalb waren sie vom Fehler nicht betroffen).
- **BCn auf D3D11, D3D12, Vulkan, OpenGL (Windows/Linux):** nicht gemessen. Auf OpenGL (Apple, 4.1) kamen keine Livezahlen zustande: `he_perf_capture.py --rhi OpenGL` lief in beiden Fällen (RGBA8 wie BC3) in den 400-s-Timeout
  (`raw-auto-landscape-2/opengl-live-timeout/`, Ursache nicht untersucht; der Dump-Pfad geht). Aus der CI: `test_texture_bcn` (Encoder, Decoder, Pak-Round-Trip) lief grün auf Windows (0,85 s) und `test_layer_mesh_instancing` ebenfalls.

## 6. Engpässe

| Stelle | Befund | Beleg |
|---|---|---|
| **Streuung** (einmalig) | `tickWorld` im dritten Frame: 31 ms (102k Instanzen), 44 ms (106k), 126 bis 133 ms (425k): rund 0,3 bis 0,4 µs je Instanz, einmal beim Öffnen. Bei einer Ebene mit Paint-Regel (`weightLayer`) streut jeder Pinselstrich die ganze Ebene neu (`markWeightGatedFoliage`): bei 425k wären das ~125 ms je Strich. **Nicht gemessen.** | Tabelle 3b, Spalte tickWorld; Logs |
| **Extrahieren** (je Frame) | `ExtractFoliage` 0,16 / 0,57 / 2,24 ms bei 6k / 25k / 102k gespeicherten Instanzen (128 m), 0,75 ms bei 425k (512 m). Es läuft jedes Frame neu, auch bei stehender Kamera und unveränderter Szene. 70 % des Mehrpreises der Meshes auf der CPU. | Tabelle 1b, `*.scopes.json` |
| **Culling** | vernachlässigbar: `FrustumCuller::cull` +0,02 bis +0,04 ms; 203 Cluster statt 425 343 Objekten | `*.scopes.json`, Log `FoliageExtract` |
| **Zeichnen** | Draw-Zahl flach (Abschnitt 3); `EncodeScene` +0,2 / +0,2 / +0,8 ms (x1 / x4 / x16), `EncodeShadowMap` +0,1 ms. GPU +1,5 ms bei 1 Mio. Dreiecken. Kein Engpass auf dieser Hardware. | Tabellen 1b, 3b |
| **Speicher (Instanzen)** | RSS +24 MB (102k) und +71 MB (425k): eine Instanz sind 64 B (`glm::mat4` in `cachedInstances`) plus Bucket- und Puffer-Listen, rund 170 bis 230 B je Instanz in der Summe | Tabelle 1 (RSS-Spalte) |
| **Speicher (Texturen)** | der große Posten: V = 3 RGBA8 sind 873 MB gegen 24 MB Instanzen. BCn/ASTC holen 655 MB zurück | Abschnitt 5 |
| **Pack-Zeit** | ASTC 66 bis 320 s je 13-Slice-Array, einthreadig; 9 Minuten für das Set | Abschnitt 5 |

## 7. Behoben in diesem Schritt

1. **BCn-Arrays kosteten 440 ms je Frame** (`src/HE_Rendering/src/GiAutoLandscape.cpp`, `giAutoLandscapeSliceMeans`). Für ein blockkomprimiertes Array rief sie `decodeTextureToRGBA8` auf das **ganze** Array (13 Slices, 12 Mips)
   und das aus `RenderExtractor::extract` in jedem Frame. Gefunden, weil der erste BC3-Lauf mit 2,25 fps und 462 ms CPU-Frame lief; `sample` auf dem Hauptthread: 69 von 69 Samples in `giAutoLandscapeSliceMeans`, 67 davon in `decodeTextureToRGBA8`.
   Jetzt wird nur die kleinste Stufe (≤ 16 x 16) der fünf gebrauchten Slices dekodiert, je Stufe in einen Puffer (`decodeBlockLevelToRGBA8`); das Ergebnis ist dasselbe. BC7/ASTC (kein Decoder) liefern wie vorher `false` und kosten nichts, darum fiel es bei ihnen nicht auf.
   Nichts im Editor setzt `cookTextureArrays` bisher, der Fehler wäre mit dem ersten Export von BCn-Arrays aufgetreten.

   | BC3-Arrays (V = 3), Metal | vorher | nachher |
   |---|---|---|
   | CPU-Frame p50 | 489,6 ms | 20,0 ms |
   | fps (Detailed-Lauf) | 2,06 | 51,7 |
   | Wanduhr des Laufs (800 Frames) | 383 s | 21 s |
   | Scene-Pass p50 | 16,5 ms | 11,6 ms |

   Rohdaten beider Seiten: `before-gi-slice-means-fix/` (ein Lauf je Format, der BC3-Lauf allein dauerte 6,5 Minuten) und die `mem2x-*`-Läufe im Hauptordner.
2. **Dieselbe Funktion las bei V = 3 die falschen Slices** (Schritt 3 hatte die Array-Tiefe `4 V + 1` eingeführt, die GI-Mittelwerte lasen weiter die Slices 0..4: Gras, zweite und dritte Grasvariante, Erde, zweite Erdvariante).
   Jetzt: `autoLandscapeSliceOf(Schicht, 0, Tiefe)`, für V = 1 dieselben Slices wie bisher. Test `tests/test_culling.cpp` („slice means read each layer's first VARIANT …“, RGBA8 und BC1, V = 3, mit Negativkontrolle: mit dem alten Slice-Index rot).
   Auswirkung: die GI-Landschaftsfarben (Reflexionen) sind mit Varianten-Arrays sonst falsch; kein Zeitgewinn.
3. **Der Windows-Schritt „Layer-mesh image test (D3D11, D3D12)“ der CI (Lauf 39532559913) starb vor dem ersten Bild** an `UnicodeEncodeError` (cp1252) beim Ausdrucken des Fallkopfes in `scripts/he_vk_imagetests.py`
   (`ci-run-39532559913-excerpt.txt`). stdout/stderr werden jetzt als UTF-8 geschrieben (`reconfigure`); der Fall läuft auf Metal mit einer cp1252-Konsole durch. Ob D3D11 und D3D12 die Layer-Meshes zeichnen, weiß damit weiter niemand.

Tests nach den Änderungen (macOS, Release, aus dem Build-Ordner): `test_culling` 68/68, `test_texture_bcn` 12/12, `test_layer_mesh_instancing` 6/6, `test_auto_landscape_variants` 8/8, `test_foliage_cluster` 22/22,
`test_hpak` 58/59 (der eine Fehler, `scanContentDirectory` 6 gegen 2, ist vorbestehend). Der volle Lauf gehört zu Schritt 7.

## 8. Direct3D und Vulkan (nur CI)

- **Vulkan, lavapipe (CPU-Rasterizer, kein GPU-Zeitwert):** Lauf 39532559913 auf `6d1c5dca`, Fall `layer_meshes`: Draws **2 / 13 / 13**, Dreiecke 17 408 / 375 804 / 1 473 604 bei „aus / x4 / x16“, `flat x4 -> x16: ok`,
  `off vs x4: mean |Δ| 2,12` (Meshes sichtbar). Die Draw-Zahlen sind auf Vulkan dieselben wie auf Metal (2 / 13 / 13), die Dreiecke ebenfalls (Metal im selben Fall: 17 408 / 375 804 / 1 473 604).
- **D3D11 und D3D12:** kein Bild, kein Zähler. Der Windows-Schritt starb am Zeichensatz (Abschnitt 7.3), `layer-mesh-shots-windows` wurde nicht hochgeladen. Nach dem Fix muss ein neuer CI-Lauf zeigen, ob der Schritt rechnet.
- **Zeiten:** keine. Hardware-Smoke-Test auf NN-WS03 bleibt offen (der Rechner war offline), ebenso GPU-Zahlen für D3D/Vulkan und BCn auf diesen Backends.

## 9. Folgethemen (nicht in diesem Schritt)

1. **Extraktion der Foliage zwischenspeichern** (Engpass Nr. 1, 2,2 ms bei 31k Instanzen): Cluster und Instanzlisten wiederverwenden, solange Kamera (Lage, Frustum, Stufengrenzen) und Foliage-Komponenten gleich sind. Fasst `FoliageExtract.cpp` und die Frame-Wiederverwendung an, also die Instancing-Schicht: eigenes Thema.
2. **Streuung je Pinselstrich** (Regel `weightLayer`): nur die betroffene Fläche neu streuen; vorher messen (`markWeightGatedFoliage`, Abschnitt 6).
3. **ASTC/BC7-Encoder parallel** über Slices und Mips (JobSystem): 9 Minuten einthreadig für ein V = 3-Set.
4. **BCn-Arrays für die Auto-Landschaft einschalten** (`cookTextureArrays`): der Weg ist frei (GI-Mittelwerte rechnen schnell und richtig); offen bleiben die Mittelwerte für BC7/ASTC (zur Packzeit vorberechnen), BC5-Normalen ohne Z (Plan §7.3) und die Plattformwahl im Export.
5. **Live-Capture auf OpenGL** (`he_perf_capture.py --rhi OpenGL` läuft in den Timeout) und auf D3D/Vulkan auf einem Rechner mit Fenster: Zahlen für die anderen Backends.
6. **RSS-Messung unter macOS** ersetzen (Metal-Allokationen, `footprint` oder die Profiler-VRAM-Felder, die auf Metal 0 bleiben); `ps rss` taugt für Texturen im Bereich von 1 GB nicht.
7. **GPU-Zeit unter gesperrtem Bildschirm:** `he_perf_capture.py` sollte das Taktzustandsproblem sichtbar machen (Tonemap/Bloom als Zeuge im Summary) oder einen GPU-gebundenen Modus anbieten (`--set RenderScale=2` dafür genutzt).
8. **Materialien der Layer-Meshes** (Plan §8.5): die Meshes sind weiß; ein Oberflächenmaterial ändert die Rechnung (Pixelkosten der Instanzen).

## 10. Nicht geprüft

- D3D11, D3D12, Vulkan (GPU) und OpenGL (live): keine Zeiten; Hardware-Smoke-Test offen.
- Die Kosten eines Pinselstrichs mit Paint-Regel, Snow Drift (die Schneegrenze liegt über dem Relief), die 65 536er-Grenze je Draw mit echten Meshes (die größte Läufe sind kleiner).
- Echte Foto-Varianten statt Nachbarschichten (für die GPU-Kosten egal, für ASTC-Encoder-Zeiten und -Qualität nicht), Qualitätsstufen 1 und 2 der Encoder, andere Macs (BC-Unterstützung), niedrigere Auflösungen der Arrays.
- Dichte x64, Größen über 512 m, bewegte Kamera (alle Läufe haben eine feste Pose; mit Bewegung kommt der LOD-Wechsel und das Nachladen der Cluster dazu).
- Die Ganzframe-Spanne `gpuMs` bei ausgelasteter GPU (28 ms gegen 15,4 ms Σ Pässe) ist nicht erklärt; die Aussagen oben stützen sich auf die Pässe.

## Handoff

- **Topic / step:** 180, Schritt 6 von 10
- **Done:** Audit-Dokument mit Messwerten (Metal): Layer-Meshes x0/x1/x4/x16 auf 128 m und 512 m, Varianten V1 gegen V3, Draw-Zahlen, ASTC/BC7/BC3 gegen RGBA8 (Größe, Encoder, Laden, Start, Pass-Zeit), Engpassliste; drei Fixes (Abschnitt 7).
- **Files:** `scripts/perf/gen_autoland_scene.py`, `autoland_matrix.py`, `autoland_table.py`; `tests/test_bench_landscape_arrays.cpp` (+ `tests/CMakeLists.txt`); `src/HE_Rendering/src/GiAutoLandscape.cpp` (`giAutoLandscapeSliceMeans`); `tests/test_culling.cpp` (neuer Fall nach „GI auto landscape: slice means …“); `scripts/he_vk_imagetests.py`; Rohdaten `docs/perf-audit/raw-auto-landscape-2/`.
- **Verified:** Zahlen und Tabellen aus den Rohläufen (`autoland_table.py` → `tables.md`); Fixes mit Test, Negativkontrolle und erneuter Messung (Abschnitt 7); `he_tests` für die genannten Dateien.
- **Not verified:** D3D11/D3D12/Vulkan-Zeiten, OpenGL live, Pinselstrich-Streuung, volle `he_tests`-Runde (Schritt 7), CI-Lauf auf dem neuen Stand (gestartet, nicht abgewartet; Link im Ergebnis-Post).
- **Decisions taken:** GPU-Vergleich nur mit RenderScale 2 und Pass-p50 (Abschnitt 1); keine Änderung im Instancing-Backend; die zwei GI-Fixes sind klein und gemessen/getestet, der Extract-Cache ist ein Folgethema.
- **Decisions suggested:** Folgethemen 1 bis 4 in Abschnitt 9, in dieser Reihenfolge.
- **Traps:** Szenengenerator: `FoliageLayer` hat `castsShadow = true` als Default und der Serializer schreibt nur Abweichungen, Preset-Ebenen brauchen explizit `false`. Gesperrter Bildschirm: GPU-Zeit bei billigen Frames aufgebläht (Abschnitt 1). `he_tests` bauen überschreibt den Deploy nur, wenn sich Quellen geändert haben (Hashes in `binaries.txt`). `pgrep -f` in einer `until`-Schleife trifft die Schleife selbst. Der V = 3-Satz in `/tmp/al2/` ist nicht im Repo (`landscape_tex_gen --arrays-only --variants 3` auf einer Kopie der Deploy-Texturen mit `_V2`/`_V3`-Dateien).
- **Next steps:** 1. CI-Lauf lesen (Windows-Schritt `layer_meshes` auf D3D11/D3D12, lavapipe); 2. Schritt 7 (voller Build und Tests); 3. Folgethema 1 (Extract-Cache).
