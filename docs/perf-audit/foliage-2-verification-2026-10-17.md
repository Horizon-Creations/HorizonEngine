# Foliage 2: Verifikation vorher gegen nachher auf vier Backends (Thema 182, Schritt 5)

Stand 17.10.2026, NN-WS03 (Windows 11, RTX 4070, Anzeige 60 Hz, 2000x1125 Editorfenster). Bezug: `../foliage-instancing-lod-plan-2026-10-10.md` Abschnitt 18.
Rohdaten: `raw-foliage-2-verify/` (`*.summary.json`, `*.conditions.txt`, `tables.md`, `ctest-summary.txt`, `img/`).

## 1. Wie gemessen wurde

- **Vorher:** Release-Build des Merge-Basis-Commits `1e41c2d2` (release/0.7.0 vor Thema 182: Cluster-Pfad aus Thema 163 schon drin, aber keine
  Graph-Material-Zwillinge, keine Instanz-Arena, keine Schichten, keine Stufen), `git archive` nach `C:/hw182b`. **Nachher:** `C:/hw182` auf `b23f765b`
  (letzter Code-Commit `d183c063`; ninja: nichts zu tun). Beide MSVC Release, eigener `DEPLOY_DIR`.
- **Zeit:** `scripts/he_perf_capture.py` (jetzt mit `--rhi`, Windows-RSS, Backend über `config.json` "RHI") im echten Editor-Loop, verstecktes Fenster,
  300 Warmup-Frames, 300 Frames mit erzwungenem vsync aus. Treiber `scripts/perf/foliage_ladder_win.py`: Szenen aus `gen_reference_world.py --foliage`
  (400-m-Feld, eingebauter Würfel, Kamera mitten im Feld), dieselbe Szene-Datei für beide Binaries, je Fall vorher/nachher **abwechselnd**, 2 Wiederholungen.
  GL: je Binary ein verworfener Warmlauf (glprogcache). `Render` = CPU-Scope je Frame (enthält das Warten auf die GPU), GPU = ganzer Frame aus Timestamps.
- **Fremdlast:** ein anderer Bee kompilierte zeitweise mit 8 `cl.exe` (in `*.conditions.txt` je Lauf vermerkt, betroffen vor allem der D3D11-Block und
  Vulkan 100k "alle"). Vulkan läuft in diesem Release-Stand **immer mit Validierungsschicht** (beide Binaries), die Vulkan-CPU-Zeiten enthalten sie.
- **Bild:** `scripts/perf/foliage_backend_ab.py`, Dump-Zeuge `HE_DUMP_FOLIAGETEST`, vorher gegen nachher plus nachher gegen nachher (Rauschboden);
  D3D/Vulkan Boden ab Zeile 100 mit Toleranz 8, Pflanzenmaske exakt.

## 2. Zeit (p50 ms je Frame, Median der Wiederholungen, vorher / nachher)

| Backend | Instanzen | Reichweite | `Render` | GPU | draws | tris | Lesart |
|---|---|---|---|---|---|---|---|
| OpenGL | 10k / 100k / 500k | alle | 1,25/1,27 · 5,26/5,23 · 22,2/22,2 | 0,77/0,77 · 3,19/3,22 · 12,5/12,5 | 3·3·5 gleich | gleich | unverändert (erwartet) |
| OpenGL | 10k / 100k / 500k | 100 m | 1,16/1,18 · 2,02/2,02 · 6,52/6,50 | gleich | gleich | gleich | unverändert |
| D3D11 | 10k / 100k | alle | 0,50/0,50 · 3,71/3,95 | 0,67/0,67 · 1,71/1,71 | gleich | gleich | unverändert |
| D3D11 | 500k | alle | 5+5 Läufe: vorher 17,7-23,5, nachher 18,0-26,6 | 6,4-8,1 / 6,5-8,5 | 5/5 | gleich | **zweigipflig in beiden Binaries** (≈18 oder ≈24 ms je Lauf), keine Regression belegbar |
| D3D11 | 10k / 100k / 500k | 100 m | 0,41/0,48 · 0,88/0,89 · 4,85/5,00 | gleich | gleich | gleich | unverändert (10k unter Fremdlast) |
| D3D12 | 10k | alle, 100 m | gleich | gleich | 18/18 | gleich | unverändert |
| D3D12 | 100k | alle | 2,58 / 3,11 | 1,30 / 2,19 | **16 386 / 19** | 1,04 M / 2,42 M | vorher **unvollständig** (Ring leer, Bild nur Himmel/Grau), nachher vollständig |
| D3D12 | 100k | 100 m | 0,96/0,96 | 1,14/1,14 | 18/18 | gleich | unverändert |
| D3D12 | 500k | 100 m | 3,37 / 4,60 | 1,22 / 2,80 | **16 385 / 20** | 0,95 M / 3,55 M | vorher unvollständig, nachher vollständig |
| D3D12 | 500k | alle | 8,74 / 16,8 | 1,31 / 7,61 | 16 385 / 16 398 | 1,02 M / 10,5 M | **nachher weiter unvollständig**: Arena voll bei 983 040 (Grenze 1 M je Slot), Rest-Schleife leert den Draw-Ring („rest of the frame is not drawn“) |
| Vulkan | 10k | alle, 100 m | gleich | gleich | 3/3 | gleich | unverändert |
| Vulkan | 100k | alle | **271,6 / 4,44** (61x) | 31,5 / 2,14 | 34 637 / 3 | gleich | vorher Pro-Objekt-Schleife (Ring 65 536 voll) |
| Vulkan | 100k | 100 m | 1,25/1,27 | 1,40/1,27 | 3/3 | gleich | unverändert |
| Vulkan | 500k | 100 m | **410,8 / 5,35** (77x) | 39,4 / 2,71 | 37 410 / 3 | gleich | vorher Schleife |
| Vulkan | 500k | alle | 1 600 / 290 (je 20 Frames; vorher-Lauf mit 300 Frames brach nach 600 s ab) | 96 / 28 | 172 910 / 107 375 | gleich | **beide Schleife**: Arena voll bei 983 040, nachher nur 5,5x besser |

`ExtractFoliage` vorher = nachher auf allen Backends (0,01-0,05 ms "alle", 0,45-0,55 ms bei 500k/100 m): die Schichten und Stufen aus Schritt 1 und 3 kosten
ohne Stufen nichts messbar. RSS nachher: D3D12 +330 MB (100k "alle") und +720 MB (500k "alle"), Vulkan +70 bis +110 MB; Ursache vermutlich die gewachsene Arena
(128 B je Instanz und Slot), nicht einzeln nachgewiesen.

## 3. Bild vorher gegen nachher

| Fall | OpenGL | D3D11 | D3D12 (8 Settle-Frames) | Vulkan |
|---|---|---|---|---|
| S 6000, forward | identisch | Boden identisch | Boden identisch | Boden identisch |
| SD 6000, deferred | identisch | Boden identisch | Boden identisch | Boden identisch |
| C 100k, 100 m | identisch | Boden identisch | Boden identisch | Boden identisch |
| A 100k, alle | identisch | Boden identisch | **46,6 % anders**: vorher leer (16 386 draws), nachher vollständig (12 draws) | Boden identisch |
| M 500k, alle (Dump-Kamera) | identisch | Boden identisch | **46,9 % anders**: vorher leer, nachher vollständig (16 draws) | Boden identisch, draws 38 553 → 3 |
| G 800, Graph-Material | identisch, draws 774 → 5 | Boden + Maske identisch, 774 → 5 | Boden + Maske identisch, 774 → 5 | Boden + Maske identisch, 774 → 5 |

Rauschboden (nachher gegen nachher) in jedem Fall 0. Gegenprobe: D3D12-nachher gegen D3D11-nachher, Fall A und M: Boden identisch. Bilder: `img/d3d12_A.png`, `img/d3d12_M.png`
(links vorher, rechts nachher). Die Dump-Kamera sieht bei M weniger Pflanzen als die Editor-Kamera der Leiter, deshalb läuft dort die Arena nicht über.

## 4. Voller ctest (`C:/hw182`, Release, scratch `APPDATA`, `-j 8`)

301 Tests: 297 grün, 2 übersprungen (`runtime_size_app_*`, kein Paket), 1 rot `test_inspector_ui` (Zwischenablage, vorbestehend). Im ersten Lauf zusätzlich
rot nach 0,02 s ohne Ausgabe: `test_foliage_layers`, `test_foliage_lod`, `test_graph_instancing`: ihr Temp-Ordner `%TEMP%\he_tests\<test>` (nur beim Konfigurieren
angelegt) fehlte; angelegt, alle drei grün (Lektion 378). `test_profiler` diesmal grün.

## 5. Nicht gemessen

Metal (kein Mac erreichbar); Graph-Material-**Zeit** (der Szenengenerator kennt kein Graph-Material; nur Draws und Bild); GI an; Kamera am Feldrand; 1 Mio.;
exportiertes Spiel; FPS-Läufe mit vsync.
