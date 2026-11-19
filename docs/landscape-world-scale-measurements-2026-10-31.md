# Landscape im Weltmaßstab: Messbasis (Schritt 0)

Stand 31.10.2026, Basis `release/0.8.0`. Plan: `landscape-world-scale-plan-2026-10-28.md`. Werkzeug: `landscape_store_bench`
(`src/HE_Tools/src/LandscapeStoreBench/main.cpp`, CMake-Target gleichen Namens, nicht in `he_tests`).

**Alle Zahlen: MacBook Air (Mac17,4, Apple M5, 10 Kerne, 24 GB), macOS 27.0.1, Release, interne SSD. NN-WS03 (9950X / 4070 / NVMe), auf der
Plan §4 abnimmt, war offline (`hive_devices`: zuletzt vor 10 h). Jede Zahl hier ist also "pending on NN-WS03"; das Werkzeug läuft dort unverändert,
liefert unter Windows aber nur warme Lesewerte (siehe "Nicht verifiziert").**

> **Nachtrag (Schritt 1):** Seit Schritt 1 benutzt das Werkzeug den Engine-Code (`LandscapeTile`, `ProceduralLandscapeSource`, `LandscapePackWriter`) statt eines eigenen Containers. Die Größen sind unverändert; zur Neumessung siehe `landscape-world-scale-store-2026-10-31.md` §5.

## 1. Was gemessen wird

Eine prozedurale 100-km-Welt als reiner Datenspeicher, ohne Rendering. Die Höhen kommen aus dem vorhandenen `terrainFbm` (12 Oktaven, 6 Zyklen
über die Welt, ~8 m kleinste Wellenlänge); die Gewichte werden aus Höhenband, Neigung und Rauschen abgeleitet und auf 255 normiert. Pro Tile (512 m):

* Höhen 16 Bit, 2-m-Raster, 257 x 257, Mip-Kette punktabgetastet bis 128 m (7 Stufen).
* Gewichte RGBA8, 4-m-Raster, 128 x 128, **12 Kanäle = 3 Seiten** (`kTerrainWeightPages = 3` auf 0.8.0, der Plan rechnete mit 2), Mip-Kette per 2x2-Mittel bis 128 m (6 Stufen).
* Container je Tile: jede Mip-Stufe ein eigener komprimierter Strom, damit Stufe 0 allein decodierbar ist.
* Codecs: `store`, `lz4`, `zstd` (Stufe 3), je mit und ohne Prädiktor (`+pred`: Höhen = Ebenen-Prädiktor links+oben-obenlinks, Zickzack, Lo-/Hi-Byte-Ebenen; Gewichte = Kanalebenen mit Zeilen-Delta).
  Verlustfrei, jede Zeile prüft Roundtrip bit-genau.

Welt = 196 x 196 Tiles (100,4 km, 100 000 / 512 geht nicht auf; 195 wären 99,84 km). Stichprobe A: 48 Tiles verteilt, 1 Thread. Stichprobe B/C: ein 4,1 x 100,4-km-Streifen
(8 x 196 = 1568 Tiles) und einmal die **ganze Welt** (38 416 Tiles). Schreiben umgeht den Page-Cache (macOS `F_NOCACHE`, Linux `fsync` + `POSIX_FADV_DONTNEED`), "cold" liest ohne Cache,
"warm" liest nach einem ungemessenen Durchlauf. (Ein erster Lauf ohne das Umgehen beim Schreiben zeigte "cold" = "warm", 21 GB/s: verworfen.)

## 2. Ergebnisse

### Größe und Codec-Zeit (1 Thread, ganze Pyramide; Streifen, Höhen ohne Rauschen)

| Codec | KB/Tile | davon Höhen | davon Gewichte | Faktor | Encode ms | Decode ms | Decode nur Stufe 0 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| store | 428,7 | 172,7 | 256,0 | 1,00 | 0,02 | 0,019 | 0,016 |
| lz4 | 282,2 | 135,0 | 147,2 | 1,52 | 0,66 | 0,097 | 0,067 |
| lz4+pred | 180,6 | 55,7 | 124,9 | 2,37 | 0,58 | 0,298 | 0,206 |
| zstd | 186,1 | 96,1 | 90,0 | 2,30 | 1,30 | 0,465 | 0,333 |
| **zstd+pred** | **97,3** | 32,7 | 64,6 | **4,40** | 0,91 | **0,483** | 0,345 |
| store+pred (nur der Prädiktor) | 428,7 | | | 1,00 | 0,15 | 0,164 | 0,123 |

Mit `--rough 2` (Höhenrauschen ±2 LSB, ≈ 9 cm, wenn 65 535 Stufen 3000 m abdecken): zstd+pred 116,5 KB (3,68x), lz4+pred 217,0 KB (1,98x), lz4 301,7 KB (1,42x); Decodezeiten unverändert.
Erzeugung (fBm + Gewichte + beide Pyramiden): 8,4 ms/Tile bei 1 Thread; die ganze Welt samt Encode in 68 s auf 10 Threads (564 Tiles/s, 17,7 ms je Tile und Thread, weil sich 10 Threads Speicherbandbreite und Kerne teilen).

### Ganze Welt (38 416 Tiles, zstd+pred, gemessen)

| | Wert |
|---|---:|
| auf Platte, 1 Pack-Datei | 3,85 GB (97,9 KB/Tile, Faktor 4,38) |
| roh, alle Stufen | 16,9 GB (Plan §2: 5 GB Höhen + 5 GB Gewichte + 33 %) |
| roh, nur Stufe 0 | 12,6 GB |
| mit lz4+pred (Streifen) | ≈ 7,2 GB |
| mit Höhenrauschen ±2 LSB | 4,6 GB |

Der Streifen sagt die Welt gut voraus (97,7 vs. 97,9 KB/Tile).

### Lesen und Decodieren, zufällige Reihenfolge, ganze Welt

| Quelle | Cache | Threads | Tiles/s | Lesen ms/Tile | Decode ms/Tile |
|---|---|---:|---:|---:|---:|
| Pack | kalt | 1 | 964 | 0,198 | 0,491 |
| Pack | kalt | 10 | 6370 | 0,267 | 0,740 |
| Pack | warm | 10 | 7388 | 0,029 | 0,770 |
| Datei je Tile | kalt | 1 | 892 | 0,282 | 0,491 |
| Datei je Tile | kalt | 10 | 6384 | 0,246 | 0,757 |

Pack und Datei-je-Tile liegen auf dem Mac gleichauf (APFS); der Unterschied (0,2 vs. 0,28 ms bei 1 Thread) liegt im `open()` je Tile. Rohe Platte (512-MB-Probe): sequentiell kalt 3,7 GB/s,
zufällig 128 KiB kalt 7 000 IOPS (1 Thread) bis 17 700 IOPS (10 Threads) = 0,9 bis 2,2 GB/s. (Das Schreiben mit 6,6 GB/s hat keinen `fsync` und ist keine Plattenzahl.)

### Residenter Satz und Wanderung (aus den Messwerten)

| Radius | Tiles | entpackt, alle Stufen | komprimiert | Decode für Komplettfüllung (1 Thread / 10) |
|---|---:|---:|---:|---:|
| 4 km | 192 | 80 MB | 18 MB | 0,09 s / 0,01 s |
| 8 km | 767 | 321 MB | 73 MB | 0,36 s / 0,04 s |
| 12 km | 1726 | 722 MB | 165 MB | 0,82 s / 0,08 s |

Neu eintretende Tiles bei Radius 8 km: 30 m/s → 1,8/s, 100 m/s → 6,1/s, 250 m/s → 15,3/s. Bei 250 m/s sind das 15,3 x 0,49 ms ≈ 7,5 ms Decode je Sekunde.

## 3. Befunde

1. **Das Plan-Budget war zu klein, aus zwei Gründen.** Es rechnete Gewichte mit 2 Seiten und ohne Mips (256 MB bei 8 km). Gemessen: 3 Seiten, Mips dazu: **321 MB bei 8 km, 722 MB bei 12 km**. Die Gewichte sind 256 von 429 KB roh
   und etwa 2/3 des komprimierten Tiles. 8 km passt unter 0,5 GB (CPU wie VRAM); 12 km nicht. Nur Stufe 0 resident: 8 km = 240 MB (Stufe 0 = 321 KB je Tile, die Mips kosten weitere 107 KB).
2. **Streaming ist weder CPU- noch Platten-gebunden.** 15 Tiles/s x 0,5 ms Decode liegt weit unter dem Frame-Budget, wenn das Decodieren auf den Workern läuft; kalt liest eine Platte ≈ 900 Tiles/s mit einem Thread
   (6 400 mit 10) einschließlich Decode. Der Hauptthread-Teil von Plan-Schritt 1 (< 1 ms/Frame) ist damit eine Frage des Hochladens/Einhängens, nicht des Ladens.
3. **Codec: zstd+pred auf Platte (3,85 GB), lz4+pred, wo `HE_HAVE_ZSTD` aus ist (≈ 7,2 GB).** Der Prädiktor allein kostet 0,16 ms beim Decodieren, also die Hälfte von lz4+pred und ein Drittel von zstd+pred; er ist
   gut vektorisierbar, aber kein Grund, ihn wegzulassen: er halbiert die Höhen-Bytes. zstd war schon im Build (`CMakeLists.txt:244`), nichts Neues nötig.
4. **Die Kompressionsfaktoren sind eine Klammer, keine Vorhersage.** fBm-Höhen sind glatter als echte DEMs (obere Schranke); die synthetischen Gewichte belegen alle 12 Kanäle mit Rauschen und komprimieren schlechter als
   gemalter Inhalt (meist 2 bis 4 lebende Kanäle). Mit ±2 LSB Rauschen 3,7x statt 4,4x. Für echte Inhalte ist 2,5x bis 4x ein ehrlicher Bereich, also 4 bis 6,5 GB für die Welt.
5. **Prozedurale Erzeugung zur Ladezeit (Plan §3.1) ist machbar, aber nicht gratis:** 8,4 ms je Tile bei 1 Thread allein (≈ 120 Tiles/s); mit allen 10 Threads samt Encode 17,7 ms je Tile und Thread.
   Für 15 Tiles/s Wanderung reicht das; gespeicherte Tiles (0,5 ms Decode) sind 17x billiger.

## 4. Nicht verifiziert

* Alles auf NN-WS03 (9950X, 4070, NVMe): Decode- und Erzeugungszeiten hängen am Kern, Lesewerte an der Platte; keiner von beiden ist mit dem Mac vergleichbar gemessen.
* Windows: `Reader`/`Writer` im `#else`-Zweig sind nie übersetzt worden. Dort fehlt der Cache-Bypass (`FILE_FLAG_NO_BUFFERING`), "cold" wird nicht gemessen, und das Werkzeug sagt das in der Überschrift von Abschnitt C.
  Auf Linux sind `posix_fadvise`-Pfade ebenfalls nicht gelaufen.
* "Lesen ms/Tile" enthält `open()`/`close()` je Tile; ein echter Streamer hält die Pack-Datei offen. VRAM-Upload und BC-Kompression der Gewichte sind nicht gemessen (die 321 MB sind unkomprimierte Texturen).
* Echte Heightmaps (World Machine/Gaea) und gemalte Gewichte: nicht vorhanden, siehe Befund 4.
* Plattendauer nach längerer Last (Thermik, SLC-Cache der SSD): die Läufe dauerten je höchstens 5 min.

## Wiederholen

```
cmake --build cmake-build-release --target landscape_store_bench -j 8
landscape_store_bench --dir <ordner auf der zu messenden Platte>                    # Streifen 8 x 196, ~15 s
landscape_store_bench --dir <ordner> --full --sample 16 --disk-mb 512                # ganze Welt, ~5 min, ~4 GB (wird am Ende gelöscht)
landscape_store_bench --dir <ordner> --rough 2                                       # pessimistischere Höhen
landscape_store_bench --tile-m 512 --raster-h 1 --raster-w 4                         # 1-m-Höhen (4x Höhenbytes)
```

## Handoff

- **Topic / step:** Hive-Topic 326 (Landscape im Weltmaßstab), Schritt 0 von 8 ("Messbasis und Budgets")
- **Branch / commit:** `claude/landscape-weltmassstab-100km` (von `origin/release/0.8.0`), Werkzeug @ 5b6bbd99, dieses Doc im Folgecommit (pushed: siehe PR)
- **Done:** Reproduzierbares Messwerkzeug plus Messwerte auf dem Mac; Plan-Zahlen §2 durch Messwerte ersetzt, Budget korrigiert (3 Gewichtsseiten, Mips).
- **Files touched:** `src/HE_Tools/src/LandscapeStoreBench/main.cpp` (Werkzeug), `src/HE_Tools/CMakeLists.txt` (Target vor `weather_sound_gen`), dieses Doc.
- **Verified:** Build auf macOS (Apple clang, Ninja, Release); 3 Läufe (Streifen, Streifen mit ±2 LSB, ganze Welt), alle Codecs Roundtrip "ok" (bit-genau); Cold/Warm-Unterschied bestätigt (3,7 vs. 17,9 GB/s sequentiell).
- **Not verified:** NN-WS03-Zahlen; Windows- und Linux-Zweige des Werkzeugs; echte DEM-/Malinhalte; VRAM-Upload (siehe §4).
- **Decisions taken:** keine für die Engine. Werkzeug misst zstd Stufe 3 (`--zstd-level`), nicht mehrere Stufen.
- **Decisions suggested, not taken:** Tile 512 m / Höhen 2 m / Gewichte 4 m bleibt Startwert (Schritt 1 kann es per Konstante ändern); residenter Radius 8 km; Deckel ≈ 350 MB RAM und ≈ 350 MB VRAM für Terraindaten;
  zstd+pred auf Platte, lz4+pred als Rückfall; entscheiden, ob grobe Mip-Stufen nur auf Platte oder immer im RAM liegen (sie kosten ≈ 33 % auf Stufe 0, 25 % der entpackten Gesamtgröße); Tile-Datei = ein Pack mit Index (offen gehalten).
- **Pending (Mensch):** Raster und Authoring (Plan "Handoff": 100 km von Hand malen oder aus Generator/Import?) ist weiterhin unbeantwortet und bestimmt, wie realistisch die Kompressionsfaktoren sind.
- **Traps:** Build-Ordner im Worktree braucht `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` und eine Kopie von `_deps` (`cp -Rc`); Ninja liegt hier nur in CLion (`CMAKE_MAKE_PROGRAM`). macOS-`F_NOCACHE` räumt vorhandene Cache-Seiten nicht, nur Schreiben ohne Cache macht "cold" echt.
- **Next steps:** 1. Bench auf NN-WS03 laufen lassen (`--full`), Windows-Zweige übersetzen und Cache-Bypass ergänzen.  2. Schritt 1 (`LandscapeStore`, Tile-Format, Streaming-Kern) mit den Budgets aus §3 beginnen; Containerformat aus `encodeTile` übernehmen (Header, Strom je Stufe).  3. Vor Schritt 2 die Gewichte-Seitenzahl und BC-Kompression der Gewichte klären (Budget).
