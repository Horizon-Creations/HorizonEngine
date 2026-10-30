# Landscape im Weltmaßstab: 100 km x 100 km (Plan, Schritt 1)

Stand 28.10.2026, Basis `release/0.7.0`. **Nur Plan, nichts gebaut, nichts gemessen.** Alle Zahlen unter "Rechnung" sind
Überschläge aus den Konstanten im Code und der Hardware von NN-WS03; Stellen, die ich nicht nachgeprüft habe, sind markiert.

## 0. Ziel (vom Menschen)

Die Engine soll Landschaften von 100 km x 100 km auf dem Rechner des Menschen tragen, mit genug Luft für Skripte, Physik
usw., mit HLOD, Streaming und allem, was dazugehört. Bei größeren Landschaften sollen **ganze Bereiche entladen** werden,
wenn sie weit weg sind. Maschine: Ryzen 9 9950X (16 Kerne), 31 GB RAM, RTX 4070 (12 GB VRAM), NVMe-SSDs.

## 1. Was heute da ist (und was nicht)

| Baustein | Stand | Stelle |
|---|---|---|
| Eine Landschaft | ein `TerrainComponent`, **höchstens 1025 x 1025 Höhenwerte** (`kTerrainMaxResolution`), höchstens 32 x 32 Chunks zu 64 Zellen, 4 LODs | `TerrainComponent.h:17`, `TerrainSystem.cpp:51` |
| Daten | `sculptHeights` und `layerWeights` (2 Seiten RGBA8) **inline als base64 in der Szene** | `SceneSerializer.cpp:732`, `TerrainComponent.h:64` |
| Physik | **ein** Jolt-`HeightFieldShape` für die ganze Landschaft | `PhysicsWorld.cpp:1243` |
| GI | Höhenfeld der einen Landschaft (`builtField`) für die Schattenstrahlen, Chunk-Meshes für Proben/Reflexionen | `GiCasterRange.h`, Audit `landscape-streaming-lod-audit-2026-10-27.md` |
| Entity-Streaming | `CellStreamer`: Zellen laden/entladen nach Kameraabstand mit Vorausschau, Jobs, 4 ms Budget, Jolt je Zelle. **Terrain ist ausdrücklich nicht Teil** | `docs/entity-cell-streaming-plan-2026-10-10.md` §0 |
| Genauigkeit | `FloatingOrigin` schiebt die Wurzel-Kinder samt Jolt (Messung 153: ohne ruckeln Objekte ab ~32 km) | `FloatingOrigin.h`, Zeile in `GameApplication.cpp` (Aufruf vor `updateCellStreaming`) |
| HLOD | gibt es nicht (der Far-Ring-HLOD des Audits wurde nicht gebaut: nur Messung, kein Bedarf bei Catania) | Audit "far-ring HLOD not built" |

Folge: 100 km bei 1025 Werten heißt **98 m pro Höhenwert**. Es geht nicht um "mehr Chunks", sondern um ein anderes
Datenmodell: nichts am Stück im RAM, in der Szene oder als Entity.

## 2. Rechnung (Überschläge)

* Höhe 16 Bit, **2 m Raster**: 50 000 x 50 000 = 2,5 Mrd. Werte = 5 GB roh, mit Delta + zstd grob 1 bis 1,5 GB. Bei 1 m das Vierfache.
* Gewichte (2 Seiten RGBA8 = 8 Byte) im **4-m-Raster**: 625 Mio. Texel = 5 GB roh, komprimiert grob 1 GB. Mip-Kette obendrauf +33 %.
* Resident um die Kamera, Radius 8 km: Höhen (2 m) 128 MB, Gewichte (4 m) 128 MB. Mit Puffer unter **0,5 GB CPU und 0,5 GB VRAM**.
* Das heutige Chunk-Modell als Entities: 100 km / (64 x 2 m) = 781 Chunks je Seite = **610 000 LOD0-Chunks**. Unmöglich als Entities; es muss
  ein Quadtree mit Distanz-LOD sein (ein Patch ist immer 64 Zellen groß, nur sein Maßstab wächst): ~300 bis 600 Patches im Bild.
* Immer resident: Grob-Mesh der ganzen Welt, 128 m Raster = 782² Punkte ≈ 10 MB. So ist der Horizont nie leer, auch wenn alles andere entladen ist.
* Fließkomma: bei 50 km vom Ursprung ist eine float-Einheit ~4 mm; ohne Floating Origin und tile-lokale Koordinaten ruckeln Physik und Vertices.
  Floating Origin existiert, für Terrain-Tiles nicht nachgeprüft.

## 3. Architektur (Empfehlung)

1. **Kachelspeicher** (`LandscapeStore`): Welt = Raster aus Tiles (Vorschlag 512 m, je 257² Höhen bei 2 m), pro Tile Höhen, Gewichte, optional Wasser/Foliage-Daten,
   **Mip-Pyramide** bis zum 128-m-Raster; Dateien im Projekt, Pak-fähig; Lesen und Dekodieren auf dem `JobSystem`, LRU mit Speicherbudget.
   Eine **prozedurale Quelle** (die bestehende fBm-Formel) erzeugt Tiles zur Ladezeit: damit gibt es ohne Daten eine 100-km-Testwelt für Messungen.
2. **Quadtree-LOD (CDLOD-artig)**: Patches mit festen 64 Zellen, Maßstab pro Stufe x2, Übergänge per Morphing statt Naht-Meshes. Höhe kommt aus einer
   **Höhen-Textur** (Clipmap/Atlas der residenten Tiles); so brauchen Vulkan/D3D/GL/Metal denselben Datenpfad und es gibt keine CPU-Mesh-Neuberechnung beim Wandern.
   Gewichte genauso als sparse Textur (Array/Atlas). Das heutige Material (Layer-Blend, Auto-Landscape) liest weiter Gewichte und UV, nur aus dem Atlas.
3. **Eine Landschaft bleibt ein Spezialfall**: das bisherige `TerrainComponent` lädt unverändert (altes Format bleibt lesbar, Test nötig, `HAsset.h`-Regel);
   neu ist ein Modus "Tile-Landschaft". Der Editor migriert eine Landschaft per Befehl in einen Speicher.
4. **Physik**: ein Jolt-`HeightFieldShape` **je geladenes Tile** im Physik-Radius (z. B. 1,5 km), statische Bodies, mit dem Entladen des Tiles weg. Navigation und Foliage-Platzierung
   lesen dieselben Tiles.
5. **GI/Schatten**: Höhenfeld-Tracing und Reflexionen gegen die **residenten** Tiles (Kaskade um die Kamera), Ferne bekommt Himmel/Ambient. Die heutige 500-m-Grenze und
   `builtField` (eine Landschaft) werden dadurch zu "Höhen-Clipmap". **Das ist der größte Umbau am Renderer** (alle fünf Backends).
6. **Entladen ganzer Bereiche**: Regionen (z. B. 8 km) haben Hysterese (laden bei R, entladen bei 1,25 R) und ein Speicherbudget; beim Entladen gehen Tile-Texturen, Jolt-Shapes,
   Nav-Tiles, Foliage-Puffer, Wasserkörper und die Entity-Zellen der Region zusammen. Das Grob-Mesh bleibt. `CellStreamer` wird um diese Regionsebene erweitert, nicht ersetzt.
7. **HLOD**: (a) Terrain = die Quadtree-Stufen selbst; (b) Props/Gebäude/Bäume: pro Region ein gebackenes Proxy-Mesh (+ Impostor für Bäume) ab dem Radius, in dem die
   Entity-Zellen entladen sind; Backen im Editor/Export, Format als Teil des Pak.
8. **Authoring**: Heightmap-Import (16-Bit, tiled, World Machine/Gaea), Sculpt/Paint arbeitet auf geladenen Tiles und schreibt Tiles zurück, "Region bearbeiten" lädt einen Ausschnitt.
   Hier liegt das meiste an Editor-Arbeit; zu entscheiden ist, ob 100 km von Hand gemalt oder aus Generatoren/Importen kommen (siehe Entscheidungen).

## 4. Schritte mit Abnahmezahlen (alle auf der 4070, 1440p, Release)

| # | Schritt | Abnahme |
|---|---|---|
| 0 | Messbasis: prozedurale 100-km-Welt als reiner Datenspeicher, Speicher- und Ladezeiten, Budgets festlegen | Zahlen im Doc: Tile-Dekodierzeit, MB pro Tile, SSD-Durchsatz |
| 1 | `LandscapeStore` + Tile-Format + Streaming-Kern + Tests (ohne Rendering) | Wanderung über 100 km: Hauptthread-Anteil < 1 ms/Frame, RAM-Deckel hält, kein Tile doppelt |
| 2 | Quadtree-Renderer auf Vulkan, dann D3D12/D3D11/GL/Metal, Grob-Mesh immer da | 100 km sichtbar, Terrain-CPU < 0,5 ms, Terrain-GPU < 1,5 ms, keine Risse, kein Popping über 1 px |
| 3 | Physik, Navigation, Foliage, Wasser je Tile; Floating Origin mit Tiles | Fahrzeug/Charakter über 50 km ohne Ruckeln, Jolt-Stepzeit unverändert gegenüber Catania |
| 4 | GI/Schatten/Reflexionen auf Höhen-Clipmap | Bildvergleich gegen heutige Catania-Pfade, GIShadow/GIRefl nicht teurer als heute |
| 5 | Regionen-Entladen + Entity-Zellen + HLOD-Backen | RAM bleibt unter Budget bei Flug quer über die Karte, 1 % low Frame < 8 ms |
| 6 | Editor: Import, Sculpt/Paint auf Tiles, Migration, Streaming-Ansicht | 100-km-Welt im Editor ladbar und bearbeitbar |
| 7 | Gesamtverifikation: 100-km-Testwelt + Catania-Import, alle fünf Backends | Tabelle vorher/nachher, p99-Frame, Speicher |

## 5. Risiken

* GI/Schatten auf einer Clipmap sind ein Umbau über alle fünf Backends; das Audit zeigt, wie viel Handarbeit schon das Höhenfeld für eine Landschaft war.
* Material-Graph und Auto-Landscape-Material nehmen heute **eine** Gewichts-UV über die ganze Landschaft an (`uvTiling` = Wiederholungen über die gesamte Fläche): bei 100 km braucht es Weltkoordinaten-UVs.
* Wasser (`water::Field`) und Foliage hängen am einzelnen `TerrainComponent`; beides muss pro Tile/Region geschnitten werden.
* Metal ist hier nicht ausführbar; Schritt 2 und 4 brauchen den Mac für die Abnahme.

## Handoff

* **Topic / step:** kein Hive-Topic angelegt; Plan-Schritt 1 (Doku). Branch: `release/0.7.0`, Doc **nicht committet**.
* **Done:** Bestandsaufnahme aus Code und Docs (`TerrainComponent.h`, `TerrainSystem.cpp`, `PhysicsWorld.cpp:1243`, `SceneSerializer.cpp:732`, `entity-cell-streaming-plan`, `world-streaming-baseline`, Audit), Rechnung, Architektur, Schritte.
* **Not verified:** jede Zahl in §2 (Überschläge, keine Messung); ob Floating Origin Terrain-Tiles korrekt verschiebt; wie viel der GI-Pfade an `builtField` hängt (nur aus dem Audit); VRAM der 4070 (WMI meldet 4 GB wegen Überlauf, real 12 GB laut Karte).
* **Decisions taken:** keine. **Suggested:** Tile-Speicher + Quadtree-LOD mit Höhen-Textur statt mehr Chunk-Entities; prozedurale Quelle für Messungen; 2 m Höhen / 4 m Gewichte als Startwert.
* **Next steps:** 1. Entscheidungen des Menschen (siehe Antwort: Raster, Authoring, Zielrelease). 2. Hive-Topic anlegen (0.8.0), Schritt 0 und 1 als eigene Sessions. 3. Schritt 0 messen, bevor Schritt 2 beginnt.
