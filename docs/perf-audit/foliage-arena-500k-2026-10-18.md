# Instanz-Arena bei 500k Pflanzen in Reichweite: Laufzeitprüfung auf D3D12 und Vulkan (Thema 182, Schritt 9)

Stand 18.10.2026, NN-WS03 (Windows 11, RTX 4070, 2000x1125 Editorfenster, Viewport 1211x650). Bezug: `../foliage-instancing-lod-plan-2026-10-10.md`
Abschnitte 20 (der Fix `e96e60df`) und 21. Rohdaten: `raw-foliage-arena-500k/` (`*.summary.json`, `*.conditions.txt`, `tables.md`, `arena-lines.txt` =
Arena-/Wachstums-/Capture-Zeilen aus jedem Log, `binaries.txt`, `img/arena_500k_all_grid.png`).

## 1. Wie gemessen wurde

- **Vorher** = Kopie des Deploys von Schritt 6 (`653f343a`, `C:/hw182/deploy_s6`): der direkte Vorgänger des Fixes, zwischen `653f343a` und `e96e60df~1`
  ändert sich in `src/`/`tests/` nichts. Seine Logs tragen den alten Warntext „instance arena full (983040 instances in this frame slot …)
  (D3D12Renderer.cpp:3477)“. **Nachher** = `C:/hw182` auf `e96e60df` (ninja baute nur Renderer + Editor neu). **Merge-Basis** `1e41c2d2`
  (`C:/hw182b`, Schritt 5) nur für die RSS-Basis. DLL-Hashes in `binaries.txt`.
- **Leiter:** `scripts/perf/foliage_ladder_win.py` mit den Szenen aus Schritt 5 (`C:/hw182/s5/scenes`, 400-m-Feld, Würfel), Kamera `0,30,0,0,-0.2`,
  300 Warmup + 300 Frames, vsync aus, vorher/nachher abwechselnd, 2 Wiederholungen, D3D12 und Vulkan × 100k/500k × alle/100 m.
- **Bild:** `he_perf_capture.py` mit derselben Kamera und `HE_CAPTURE_FRAME=100` (Editor-Viewport als PPM). Der Dump-Zeuge (`HE_DUMP_*`) taugt hier
  nicht: seine Kamera sieht weniger Pflanzen, der Überlauf tritt dort nicht auf.
- **Fremdlast:** die ersten beiden Vulkan-100k/100-m-Läufe „nachher“ liefen unter 34 `cl.exe` (CPU 100 %, Frame-Hitches 250-350 ms), verworfen
  (`discarded-under-load/`) und in Ruhe wiederholt. Alle übrigen Läufe: CPU ≤ 10 %, keine Compiler.

## 2. Ergebnis 500k „alle in Reichweite“

| | D3D12 vorher | D3D12 nachher | Vulkan vorher | Vulkan nachher |
|---|---|---|---|---|
| „instance arena full“ im Log | ja, Frame 3 | **keine** | ja, Frame 3 | **keine** |
| „grows from 65536 to …“ | 3× auf 1 048 576 (f6-8) | **3× auf 1 310 720** (f4-6), danach keine | 2× auf 1 048 576 | **2× auf 1 310 720** (f4-5), danach keine |
| draws (Frame-Mitte) | 16 398 (Ring leer) | **31** | 107 375 (Schleife) | **5** |
| `Render` p50 ms | 19,26 | 17,90 | 291,14 | **19,05** (15x) |
| RSS max / last MB (r1) | 1078 / 709 | 1073 / 756 | 979 / 810 | 1078 / 818 |

- **Wachstum: 1 310 720 statt der erwarteten 1 245 184** (20 statt 19 Granulen à 65 536). Die Regel (verlangt + 1/8, auf 64k gerundet) ergibt
  daraus, dass der echte Frame etwa 1,107-1,165 M Instanzen verlangt; der Nachbau im Test (Abschnitt 20) rechnete mit ~1,1 M. Erwartet war
  der Mechanismus, und der stimmt: je Frame-Slot genau eine Zeile (D3D12 3 Slots, Vulkan 2), alle bis Frame 6, danach öffnet kein Frame mehr etwas.
  160 MiB je Slot, D3D12 480 MiB, Vulkan 320 MiB.
- **Draws 31/5 statt ~20/3:** jeder instanzierte Lauf wird bei `kMaxInstancesPerDraw` = 65 536 geschnitten (`RenderPass.cpp:151`,
  `RenderSorter.cpp:104`); bei 5x so vielen Pflanzen gibt es je Pass mehr Schnitte (D3D12 zählt die Schattenkaskaden mit). Nicht je Draw zugeordnet;
  eine Schleife sähe man an fünfstelligen Zahlen.
- **Bild** (`img/arena_500k_all_grid.png`): D3D12 vorher fehlt die ferne Feldhälfte (Editorgitter sichtbar, Gelände abgeschnitten), nachher
  vollständig bis zum Horizont. Vulkan vorher == nachher **pixelgleich** (0 px): der instanzierte Pfad zeichnet genau, was die Schleife zeichnete.
  D3D12 nachher gegen Vulkan nachher: 0,52 % der Pixel > 8 (Boden 0,96 %); Rauschboden = dasselbe bei 100k „alle“, wo beide vollständig sind:
  1,57 % (Boden 2,86 %). D3D12 vorher gegen nachher: 7,53 %.

## 3. Alle Fälle (p50 ms, Median aus 2 Wiederholungen, vorher / nachher; `tables.md`)

| Backend | Pflanzen | Reichweite | `Render` | draws | RSS max MB | Hauptpuffer je Slot vorher → nachher |
|---|---|---|---|---|---|---|
| D3D12 | 100k | alle | 3,36 / 3,39 | 19 / 19 | 617 / **394** | 524 288 → 262 144 |
| D3D12 | 100k | 100 m | 1,12 / 1,19 | 18 / 18 | 277 / 275 | kein Wachstum |
| D3D12 | 500k | alle | 19,26 / 17,90 | 16 398 / **31** | 1077 / 1074 | 1 048 576 → 1 310 720 |
| D3D12 | 500k | 100 m | 4,81 / 4,77 | 20 / 20 | 670 / **524** | 524 288 → 393 216 |
| Vulkan | 100k | alle | 3,65 / 3,68 | 3 / 3 | 603 / 539 | 524 288 → 262 144 |
| Vulkan | 100k | 100 m | 1,26 / 1,31 | 3 / 3 | 426 / 430 | kein Wachstum |
| Vulkan | 500k | alle | 291,14 / **19,05** | 107 375 / **5** | 979 / 1076 | 1 048 576 → 1 310 720 |
| Vulkan | 500k | 100 m | 5,30 / 5,27 | 3 / 3 | 655 / 624 | 524 288 → 393 216 |

Zeit, draws und Dreiecke sind außerhalb der beiden Zielzeilen unverändert. Der Hauptpuffer folgt jetzt dem Bedarf statt der Verdopplung, deshalb
sinkt RSS bei 100k „alle“ und 500k/100 m.

## 4. RSS: was aus „vorher +720 MB“ wird

- **Gegen den direkten Vorgänger** ist der Fix bei 500k „alle“ auf D3D12 neutral (max 1077 → 1074, last 709 → 756) und kostet auf Vulkan +97 MB max
  (979 → 1076): Vulkan hält jetzt die Instanzdaten statt pro Objekt zu zeichnen.
- **Gegen die Merge-Basis** (heute gemessen, D3D12 500k „alle“, 2 Läufe: max 367/369, last 353/369 MB, 16 385 draws = nur Himmel) bleibt +706 MB max,
  +390 MB last. Das ist kein Leck: 480 MiB sind die drei 160-MiB-Upload-Ringe, die ein vollständiger Frame braucht, der Rest der Spitze ist der
  Anlauf (Frames 1-6 öffnen auf jedem Slot Chunks, bevor der Hauptpuffer wächst). Die Merge-Basis zeichnete gar keine Szene.
- Kleiner wird es nur mit weniger Bytes je Instanz (gemeinsamer 64-B-Modellpuffer je Frame, Abschnitt 20, nicht gebaut) oder weniger Pässen.

## 5. Nicht geprüft

- `HE_INST_ARENA_MIB` (Budgetpfad, Warntext neu) und 1 M Pflanzen auf Hardware nicht laufen lassen; der Budgetpfad ist nur im Test (`test_instance_arena.cpp`).
- Metal, GL, D3D11 (vom Fix nicht berührt). GI an, exportiertes Spiel.
- Vulkan meldet beim Beenden (Frame 602) auf beiden Seiten dieselben `vkDestroy*`-„in use“-Validierungsfehler: vorbestehend, nicht von diesem Fix.
