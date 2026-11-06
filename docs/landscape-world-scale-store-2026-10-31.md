# Landscape im Weltmaßstab: Tile-Format, Quellen und LandscapeStore (Schritt 1)

Stand 31.10.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 1 von 8 (`LandscapeStore`, Tile-Format, Streaming-Kern, ohne Rendering).
Plan: `landscape-world-scale-plan-2026-10-28.md`; Messbasis (Schritt 0): `landscape-world-scale-measurements-2026-10-31.md`.

Das Dokument beschreibt Format und API so, dass Schritt 2 (Quadtree-Renderer) Tiles lesen kann, ohne `LandscapeTile.cpp` zu öffnen.
`TerrainComponent` und `TerrainSystem` sind **nicht** angefasst: die Einzel-Landschaft bleibt der Sonderfall (Plan 3.3), der Modus "Tile-Landschaft" kommt in Schritt 2 und 6.

## 1. Dateien

| Datei | Inhalt |
|---|---|
| `src/HE_Scene/include/HorizonScene/LandscapeTile.h`, `src/LandscapeTile.cpp` | Geometrie, `LandscapeTile`, `LandscapeCodec`, `encodeLandscapeTile` / `decodeLandscapeTile` / `peekLandscapeTile` |
| `.../LandscapeSource.h`, `src/LandscapeSource.cpp` | `LandscapeTileSource` (Schnittstelle), `ProceduralLandscapeSource`, `LandscapePackWriter`, `LandscapePackSource` |
| `.../LandscapeStore.h`, `src/LandscapeStore.cpp` | `LandscapeStore`: Ring-Planung, Laden auf dem Job-Pool, Byte-Deckel |
| `tests/test_landscape_store.cpp` | 15 Testfälle (Format, Pack, Store, Wanderung, Abbruch) |
| `src/HE_Tools/src/LandscapeStoreBench/main.cpp` | Messwerkzeug aus Schritt 0, jetzt auf diesem Code (ein Format, nicht zwei) |
| `src/HE_Scene/CMakeLists.txt` | drei neue Quellen; LZ4/zstd PRIVATE an `HorizonScene` (wie bei `HorizonCore`) |

## 2. Tile-Container, Version 1 (little endian)

Eine Kachel = ein Container (`encodeLandscapeTile`). Jede Mip-Stufe ist ein eigener Strom, damit Stufe 0 allein decodiert werden kann (`maxLevels = 1`).

| Offset | Größe | Feld |
|---:|---:|---|
| 0 | u32 | Magic `0x31544c48` (Bytes `48 4C 54 31`) |
| 4 | u8 | Version, heute `1`; ein Leser lehnt jede andere ab |
| 5 | u8 | Backend: 0 Store, 1 LZ4, 2 zstd |
| 6 | u8 | Flags, Bit 0 = Prädiktor |
| 7 | u8 | Höhen-Stufen |
| 8 | u8 | Gewichts-Stufen |
| 9 | u8 | Kanäle je Texel (1..16) |
| 10 | u16 | `hCells` (Höhen-Zellen je Seite auf Stufe 0, Zweierpotenz, 256 = 2 m bei 512 m) |
| 12 | u16 | `wTexels` (Gewichts-Texel je Seite auf Stufe 0, Zweierpotenz, 128 = 4 m) |
| 14 | f32 | `tileM` (Kantenlänge in Metern) |
| 18 | | dann `Höhen-Stufen + Gewichts-Stufen` Ströme, Höhen zuerst, Stufe 0 zuerst: je `u32 rawSize`, `u32 storedSize`, `storedSize` Bytes |

* **Inhalt Stufe k.** Höhen: `uint16`, `(hCells >> k) + 1` Werte je Seite, Zeilen entlang +x, gestapelt entlang +z. Randwerte werden mit dem Nachbarn geteilt (257 je Seite auf Stufe 0), gröbere Stufen sind punktabgetastet (`z << k`, `x << k`), also öffnet sich keine Naht.
  Gewichte: `uint8`, `wTexels >> k` Texel je Seite, `channels` Bytes je Texel verschränkt (vier Kanäle = eine Gewichtsseite, auf 0.8.0 drei Seiten = 12), gröbere Stufen als 2x2-Mittel `(a+b+c+d+2)/4`. Auf Stufe 0 summieren sich die Kanäle eines Texels auf 255 (die gröberen Stufen sind gemittelt und gerundet).
* **Stufenzahl** folgt aus der Geometrie: so viele Stufen, bis die Zellen je Seite unter `tileM / 128 m` fallen (Pyramide endet beim 128-m-Raster). 512 m: Höhen 7 Stufen (257 129 65 33 17 9 5), Gewichte 6 Stufen (128 … 4).
* **`rawSize`** ist vorgegeben (Höhen `dim² · 2`, Gewichte `dim² · channels`); ein Leser prüft sie gegen die Geometrie. `storedSize` über das Ende des Puffers ist ein Fehler, ein Leser liest nie über `size` hinaus.
* **Prädiktor (Flag 1).** Höhen: Rest zu `links + oben - oben-links` in 16 Bit mit Überlauf (erste Zeile: links, erste Spalte: oben), Zickzack `(d << 1) ^ (d >> 15)`, dann erst alle Lo-Bytes, dann alle Hi-Bytes. Gewichte: Kanal-Ebenen hintereinander, je Ebene Zeilen-Delta mod 256. Verlustfrei; ohne Flag liegen die Rohdaten im Strom.
* **Fest verdrahtet:** `landscape tile: format version 1 is fixed byte for byte` in den Tests enthält einen 81-Byte-Container als Literal. Ändert sich das Format, muss `kLandscapeTileVersion` steigen und der alte Leser bleibt (Regel aus `HAsset.h`: neues Format = Versionsbyte, alter Weg bleibt lesbar).
* **Backend nicht im Build:** `encodeLandscapeTile` fällt auf Store zurück (der Header sagt, was benutzt wurde); `decodeLandscapeTile` verweigert ein Backend, das der Build nicht hat, mit einer Fehlermeldung.

## 3. Pack-Datei, Version 1

| Offset | Größe | Feld |
|---:|---:|---|
| 0 | u32 | Magic `0x31504c48` (`48 4C 50 31`) |
| 4 | u32 | Version `1` |
| 8 / 12 | u32 | `tilesX` / `tilesZ` (je höchstens 65 536) |
| 16 | f32 | `tileM` |
| 20 / 22 | u16 | `hCells` / `wTexels` |
| 24 | u8 | Kanäle (Rest bis 32: Nullen) |
| 32 | | Index, `tilesX · tilesZ` Einträge, zeilenweise (`z · tilesX + x`): `u64 offset`, `u32 size` (12 Bytes, `size = 0` = keine Kachel) |
| danach | | Tile-Container, hintereinander |

Der Index (460 KB für 100 km) bleibt im Speicher, die Datei offen (POSIX `pread`, kein Lock zwischen Workern; Windows: ein `ifstream` hinter einem Mutex). Ein Eintrag, der außerhalb der Datei oder im Index selbst liegt, lässt `open` scheitern.
`LandscapePackWriter` schreibt Kacheln in beliebiger Reihenfolge, jede einmal, und den Index erst in `finish()`: eine 4-GB-Welt braucht keine 4 GB Speicher. Eine Kachel mit anderer Geometrie als die des Packs wird abgelehnt.

## 4. LandscapeStore

```cpp
LandscapeStore store(source, config);             // source: LandscapePackSource oder ProceduralLandscapeSource
store.update(anchors, budgetMs);                  // Hauptthread, einmal je Frame
auto tile = store.tile({x, z});                   // shared_ptr<const LandscapeTile> oder null; hält die Kachel auch nach dem Entladen
```

* **Koordinaten:** absolute Welt (x Ost, z Süd, y ignoriert); Kachel (x, z) deckt `[x·tileM, (x+1)·tileM)`. **Floating Origin ist nicht Teil von Schritt 1:** der Aufrufer gibt Weltposition + Ursprung, wie bei `CellStreamer`. Anker haben `position`, `velocity` (Vorausschau `lookaheadSec`, 2 s) und `radiusScale`.
* **Ringe (Plan 3.6):** *wanted* = Kacheln bis `loadRadius` (8 km), nächste zuerst, nie mehr als das Byte-Budget fasst (`budgetClamped`, der Radius schrumpft statt des Budgets, einmal als Warnung im Log); *kept* = bis `keepFactor · loadRadius` (1,25). Eine geladene Kachel bleibt, bis sie den kept-Ring verlässt. Mehrere Anker: nächster gewinnt, `radiusScale` teilt den Abstand.
* **Zustände je Schlüssel:** `Loading` → `Resident`, oder `Failed`. Der Schlüssel steht höchstens einmal in der Map; ein Schlüssel im Zustand `Loading` wird nicht noch einmal geladen ("kein Tile doppelt"). Eine geplante Kachel zählt ab dem Einplanen gegen das Budget, daher gilt `resident + loading <= byteBudget / tileBytes` immer. Eine nicht ladbare Kachel (`Failed`) wird erst wieder versucht, nachdem sie den kept-Ring verlassen hat.
* **Laden:** `ThreadPool::scheduleFn` mit `JobPriority::Low` und einem `CancelToken` je Kachel (Kind des Store-Tokens). Der Worker lädt und decodiert; der Hauptthread schiebt in `update()` nur fertige Kacheln in die Map (innerhalb `budgetMs`, mindestens eine). Verlässt eine Kachel den kept-Ring: ein noch nicht gestarteter Job wird abgebrochen (`JobHandle::cancel`, `onCancelled` meldet es), ein laufender fertig geladen und verworfen (`loadsDiscarded`). `JobDesc::stale` ist ungenutzt, weil `plan()` ausdrücklich abbricht.
* **Planung nur bei Bewegung:** neu geplant wird, wenn sich ein Anker (oder sein Vorausschaupunkt) um `replanTiles · tileM` (0,25 · 512 m) bewegt hat, die Ankerzahl oder ein Maßstab sich ändert oder `setConfig` aufgerufen wurde; sonst läuft nur `pump()` (Lookup der fehlenden Kacheln, bis `maxInFlight` = 32).
* **Eviction im Budget:** reicht der Platz nicht, fliegen zuerst die entferntesten residenten Kacheln, die zwar im kept-Ring, aber nicht mehr wanted sind. Wanted-Kacheln werden nie für andere verdrängt.
* **`maxLevels`** (Config): -1 = ganze Pyramide, 1 = nur Stufe 0. Das ist die Entscheidung aus Schritt 0 (321 MB gegen 240 MB bei 8 km Radius); sie bleibt für Schritt 2 offen, der Store macht beides.
* **Stats:** `resident`, `loading`, `failed`, `wanted`, `residentBytes`, `peakBytes`, `maxTiles`, `loadsStarted/Finished/Failed/Cancelled/Discarded`, `evictions`, `plans`, Hauptthread-Zeit je `update()` (letzte, Maximum, Mittel). Es gilt `loadsStarted == finished + failed + cancelled + discarded + loading`.

## 5. Abnahme (Plan §4, Schritt 1) und was gemessen wurde

| Forderung | Beleg |
|---|---|
| Wanderung über 100 km | Testfall "walking 100 km keeps the resident set inside the kept ring…": 654 Updates von (1 km, 1 km) bis (99 km, 99 km), 196 x 196 Kacheln, nach **jedem** Update: kein Schlüssel doppelt, alles Residente im kept-Ring, `residentBytes <= Budget` |
| RAM-Deckel hält | derselbe Testfall (`peakBytes <= Budget`), "a budget smaller than the ring shrinks the radius, never the budget" (10 Kacheln Budget, 224 gewünscht: nie mehr als 10) |
| Kein Tile doppelt | "loads the tiles around an anchor, each exactly once": `loadsStarted == Anzahl der Kacheln im Ring`, nach 20 Leer-Updates weiter 1 Plan; die Buchhaltung geht auf (nichts geht verloren) |
| Nichts für Verlassenes geladen | "an anchor that jumps away cancels…": Sprung um 50 km mit blockierten Workern, danach keine Kachel des alten Rings resident, `loadsCancelled > 0`, `loadsDiscarded >= 1`. Ein absichtlich eingebauter Fehler (verspätete Kacheln nicht verwerfen) lässt genau diesen Test mit drei Fehlern rot werden |
| Hauptthread < 1 ms/Frame | `update()` im Wanderungstest: Mittel 0,03 ms, Maximum 0,14 bis 0,88 ms in drei Läufen (M5) |

**Einschränkung zur letzten Zeile:** gemessen mit **kleinen Test-Kacheln** (33 x 33 Höhen, 16 x 16 Gewichte, 4 Kanäle), nicht mit 257² / 128² / 12 Kanälen. Das ist die Buchhaltung auf dem Hauptthread (Planen, Map, Einhängen). Das Decodieren (0,5 ms je Kachel in Schritt 0) läuft auf den Workern und liegt nie auf dem Hauptthread; die Zahl für volle Kacheln ist damit "diese + nichts". Einen Lauf mit vollen Kacheln gibt es noch nicht. CI-Zeiten schwanken: der Test erlaubt nur eine lose Schranke (100 ms).

### Neu gemessen mit dem Engine-Format (Bench, M5, Streifen 8 x 196)

Der Bench (`landscape_store_bench`) erzeugt, codiert, schreibt und liest jetzt mit dem Engine-Code (`ProceduralLandscapeSource`, `encodeLandscapeTile`, `LandscapePackWriter`). Neulauf, Streifen 8 x 196, M5:

| | Schritt 0 (privater Container) | Schritt 1 (Engine-Format) |
|---|---:|---:|
| zstd+pred, KB je Kachel | 97,3 | 96,9 |
| davon Höhen / Gewichte | 32,7 / 64,6 | 32,7 / 64,2 |
| lz4+pred, KB je Kachel | 180,6 | 180,1 |
| ganze Welt (zstd+pred) | 3,85 GB | 3,84 GB |
| Erzeugung je Kachel, 1 Thread | 8,4 ms | 8,15 ms |
| Decode zstd+pred, 1 Thread, Abschnitt A | 0,48 ms | 0,55 ms |

Die Größen sind gleich (der Container ist 10 Bytes größer, die Fbm-Normierung leicht anders: `max(tilesX, tilesZ) · tileM` = 100 352 m statt 100 000 m). **Die Zeiten sind diesmal nicht vergleichbar:** der Mac war während der Neuläufe nicht ruhig
(Load-Average 6 bis 10, CLion indizierte das Build-Verzeichnis mit 100 bis 200 % CPU), und Abschnitt C (zufällig lesen + decodieren) zeigte 1,3 bis 1,6 ms Decode statt 0,49 ms. Ein direkter A/B-Lauf des alten Step-0-Benchs (Commit 5b6bbd99) und des neuen,
abwechselnd unter denselben Bedingungen, ergab keinen erkennbaren Unterschied (Decode 0,73 bis 0,76 ms alt gegenüber 0,66 bis 1,45 ms neu; Abschnitt C 0,83 bis 1,24 ms alt, 0,74 bis 1,15 ms neu). Daraus folgt nur: **kein Rückschritt durch den Engine-Code erkennbar, aber auch keine saubere Neumessung.**
Die Zahlen aus `landscape-world-scale-measurements-2026-10-31.md` bleiben die Referenz; sie sind mit einem ruhigen Rechner entstanden. Selbst mit 1,5 ms je Kachel sind 15 neue Kacheln pro Sekunde (250 m/s) nur 23 ms Worker-Zeit je Sekunde.

## 6. Bewusst offen / nicht gemacht

* **Floating Origin:** nicht Teil dieses Schritts (siehe oben). Schritt 3 prüft, ob Kachel-lokale Koordinaten reichen.
* **Der immer residente Grobring** (128-m-Raster der ganzen Welt, ≈ 10 MB, Plan 3.2): kommt mit Schritt 2. Jede Kachel trägt ihre gröbste Stufe bereits mit; ein Weltraster daraus zu bauen heißt, alle 38 416 Kacheln einmal zu lesen oder beim Backen zu schreiben.
* **Physik, Navigation, Foliage, Wasser:** lesen künftig `store.tile(key)`; nichts davon ist verdrahtet.
* **Schreiben von Kacheln** (Sculpt/Paint, Schritt 6): der Store ist schreibgeschützt.
* **Gewichtsseiten:** der Container trägt 1..16 Kanäle; welche Seitenzahl die Welt hat (3 auf 0.8.0, Plan sagt 2), entscheidet die Quelle.

## Handoff

- **Topic / step:** Hive-Topic 326, Schritt 1 von 8 ("LandscapeStore, Tile-Format und Streaming-Kern, ohne Rendering")
- **Branch / commit:** `claude/landscape-weltmassstab-100km` (PR 214, deckt jetzt Schritt 0 und 1), Code @ 8b2ea1a4, Bench-Umstellung und dieses Doc im Folgecommit (pushed: siehe PR)
- **Done:** Tile-Format v1, prozedurale Quelle, Pack-Datei mit Index, `LandscapeStore` mit wanted/kept-Ring, Laden auf dem Pool, Abbruch, hartem Byte-Deckel; 15 Testfälle grün; Bench liest und schreibt jetzt das Engine-Format.
- **Files touched:** `src/HE_Scene/{include/HorizonScene,src}/Landscape{Tile,Source,Store}.*`, `src/HE_Scene/CMakeLists.txt` (Quellen, LZ4/zstd), `tests/test_landscape_store.cpp` + `tests/CMakeLists.txt`, `src/HE_Tools/src/LandscapeStoreBench/main.cpp`, `src/HE_Scene/include/HorizonScene/LandscapeSource.h:storedOffset`.
- **Verified:** Test-Binary gegen `libHorizonScene` (15/15, drei Läufe, 68 000 Prüfungen); absichtlicher Fehler im Store schlägt im Abbruch-Test an; Bench-Neulauf (siehe §5). Im echten `he_tests`-Binary (`--source-file=*test_landscape_store.cpp`): 15/15; Nachbarn nach dem CMake-Eingriff in `HorizonScene`: `*test_terrain*.cpp` 155/155, `*test_cell_*.cpp` 34/34.
- **Not verified:** Windows (`LandscapePackSource` mit `ifstream` + Mutex, Bench-`#else`-Zweige) und Linux (`pread`, `posix_fadvise`) nie übersetzt oder gelaufen; MSVC und gcc sehen den Code zuerst in der CI; Hauptthreadzeit mit vollen Kacheln; alles auf NN-WS03; Verhalten mit echten Heightmaps.
- **Decisions taken:** eigener dünner Codec in `HorizonScene` statt Hpak-Umbau (der Codec steckt in `HpakReader/Writer`); LZ4/zstd PRIVATE an `HorizonScene`; kein `HE_API` (die Bibliothek exportiert unter Windows alles, wie `CellStreamer`); absolute `dvec3`-Anker; ein Cancel-Token je Kachel statt `stale`; Planung nur nach 0,25 Kachel Bewegung.
- **Decisions suggested, not taken:** Budget 350 MB RAM / 350 MB VRAM, Radius 8 km (siehe Messdoc); `maxLevels` für Schritt 2 entscheiden (321 oder 240 MB); Grobring beim Backen der Pack-Datei mitschreiben statt beim Start alle Kacheln zu lesen.
- **Pending (Mensch):** Raster und Authoring (von Hand malen oder Generator/Import) weiter unbeantwortet.
- **Traps:** Build mit `HE_BUILD_TESTS=ON` baut alle Testdateien (lang); schnell geht `c++ ... tests/test_landscape_store.cpp` gegen `libHorizonScene`/`libHorizonCore`/`libHorizonNet` mit einem eigenen doctest-`main`. Zwei Ninja-Läufe im selben Build-Ordner gleichzeitig scheitern. macOS-`F_NOCACHE` räumt vorhandene Cache-Seiten nicht (der Bench schreibt deshalb um den Cache herum).
- **Next steps:** 1. Schritt 2 (Quadtree-Renderer, Vulkan zuerst) liest `store.tile(key)` und entscheidet `maxLevels`; Höhen-Textur/Atlas aus den residenten Kacheln.  2. Grobring (Weltraster 128 m) im Pack mitschreiben.  3. Bench und Tests auf NN-WS03 laufen lassen, Windows-/Linux-Zweige übersetzen.
