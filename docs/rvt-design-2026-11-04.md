# Runtime Virtual Textures für Landscape (Thema 333)

Ziel (vom Menschen): wie in Unreal. Das Landscape-Material zeichnet **einmal** top-down in eine Runtime
Virtual Texture, die Landschaft liest danach nur noch. Segmentierbar je Landscape-Tile. Der eigentliche
Material-Shader läuft nur, wenn sich etwas ändert.

Dieses Doc ist Schritt 1 (Entwurf + CPU-Kern, kein GPU-Code). Schritte: Hive-Thema 333, `hive_steps`.

## 1. Bestand, auf dem das aufsetzt

| Was | Wo | Nutzen für RVT |
|---|---|---|
| Drei unlit Ausgabegraphen des Auto-Landscape (`BaseColor`, `Normal` = Weltnormale·0,5+0,5, `Surface` = R AO / G Roughness) | `AutoLandscapeView`, `src/HE_Core/include/MaterialGraph/AutoLandscapeMaterial.h:289` | Das ist schon der Inhalt einer Page. Der Producer braucht keine neue Material-Logik. |
| `HlodBaker` rendert damit top-down ortho, wartet mit `IRenderer::PumpShaderBuilds` auf Pipelines | `src/HE_Scene/include/HorizonScene/LandscapeHlodBaker.h` (Thema 228) | Vorbild für Kamera und Warten. Aber ein ganzer Szenen-Pfad (`RenderSceneImage`): für RVT zu schwer, Schritt 2 baut einen schlanken Offscreen-Pass. |
| Fallen aus 228: Rechteck als Shader-Konstante = ein Shader je Chunk; Graph-Material zeigt sich nicht, solange die Pipeline baut | `docs/landscape-hlod-baked-material-2026-11-02.md` §7 | Page-Rechteck ist ein **Parameter**, alle Pages teilen **einen** Shader. Erste Pages erst, wenn die Pipeline fertig ist. |
| Tile-Landschaft (Quadtree, `LandscapeStore`, `kLandscapeFS`) | Thema 326, PR 214, **nicht** auf `release/0.8.0` | Consumer der Schritte 3/5. Vorher per `hive_post` an 326 abstimmen. |

## 2. Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Page-Größe | 128 Nutztexel + 4 Rand je Seite = 136² physisch | Rand hält Bilinear/Aniso auf der Page; 136 ist durch 4 teilbar (Block-Kompression später). |
| Feinste Auflösung | 0,25 m/Texel (4 Texel/m) → mip-0-Page = 32 m | Entspricht dem, was `planHlodBake` für die Nähe liefert (0,49 m/Texel gedeckelt bei 256). Je Volume einstellbar (`finestMetersPerTexel`). |
| Layer | Layer 0 RGBA8 sRGB: BaseColor.rgb, A frei (Höhe/Blend). Layer 1 RGBA8: Normal.xy, Roughness, AO (z rekonstruiert) | Zwei Texturen wie UE (BaseColor / Normal+Rough+AO). Genau die drei Ansichtsgraphen aus 228, ein Draw in zwei Render-Targets. |
| Pool-Größe | 136²·8 B = 148 KB je Page unkomprimiert; 256 MB ≈ 1770 Pages. Kompression (BC7/BC3 auf der GPU) erst in Schritt 6 | Erst Korrektheit, dann Speicher. Schalter `poolSlots` ist das Budget. |
| Mip-Kette | Seitenzahl wird auf 2er-Potenz aufgerundet, Mips bis **eine** Page für das ganze Volume | Page-Table-Mips halbieren sauber; Fallback-Kette endet immer. |
| Welche Pages werden gebraucht | Mip m deckt das Quadrat mit Halbkante `nearRadius·2^m` um die Kamera (Kanten inklusive). Alle Quadrate sind ineinander geschachtelt | Jeder Mip braucht ungefähr gleich viele Pages (bei 64 m / 32 m-Pages: 25), also ~125 Pages je Volume in Sicht. Die Fallback-Kette hat keine Lücken, weil jede Page einen Vorfahren im größeren Quadrat hat. |
| Reihenfolge beim Füllen | Zuerst Pages ohne irgendeinen Vorfahren (grobe zuerst), dann der Rest nach Abstand zur Kamera; Budget `maxRendersPerFrame` | Die Sicht ist sofort grob abgedeckt, Detail rückt von der Kamera nach außen nach. |
| Eviction | LRU, nie eine Page, die die aktuelle Ansicht braucht, nie eine gepinnte (die `pinnedMips` gröbsten Mips) | Zu kleiner Pool = **starved** (Zähler), kein Flackern durch Verdrängen des Sichtbaren. |
| Änderungen | `invalidateRect(Weltrechteck)` markiert genau die Pages, die es (mit Rand) berühren, über alle Mips; sie bleiben sichtbar (alter Inhalt) bis sie neu gemalt sind. `invalidateVolume` für Parameter-/Switch-/Material-Wechsel | Ein Pinselstrich kostet 1 Page je Mip, nicht das Volume. |
| Stillstand | Eine Frame ohne Änderung und Kamerabewegung gibt eine **leere** Liste zurück und bumpt `tableVersion` nicht | Das ist die Zusage „Shader läuft nur bei Änderung“; getestet. |
| Segmentierung | Ein Volume je Tile (bzw. Region), alle teilen **einen** Pool. `removeVolume`/`dropVolumePages` gibt die Slots sofort zurück | Globales Budget statt Budget je Tile. Id wird wiederverwendet. |
| Wetter, Schnee, Trails | **Statisch** im RVT (nicht einbacken); sie bleiben ein billiger Live-Overlay im Consumer, falls nötig (Entscheidung final in Schritt 3). Nicht umgesetzt, nur vorgeschlagen | Sonst wäre das ganze Volume bei jeder Wetteränderung dirty. |
| Verhältnis zum Export-Bake (228) | RVT ersetzt ihn zur Laufzeit. `HlodBaker`/`LODLevel::materialId` bleiben als Notnagel für Backends ohne RVT und für den Export | Kein Rückbau in diesem Thema. |
| Backend ohne RVT (`supportsRvt == false`, Software) | läuft wie heute: das Material direkt | Wie `supportsRawCapture` aus 228. |

## 3. API (Schritt 1, `HE_Scene`, `HorizonScene/RuntimeVirtualTexture.h`, `namespace HE::rvt`)

* `VolumeLayout`: Geometrie. `pageRect`, `pageAt`, `mipForMetersPerPixel`, `pagesPerSide(mip)`, `mipCount()`.
* `RvtManager(poolSlots, maxRendersPerFrame, pinnedMips)`: `addVolume`, `removeVolume`, `beginFrame()`, `update(volume, ViewDesc) -> vector<PageRender>`,
  `invalidateRect/Volume`, `resolve`, `pageTable(volume, mip)` (Fallback eingetragen; Eintrag = `slot | effektiverMip << 24`, `0xFFFFFFFF` = leer),
  `tableVersion`, `stats()`.
* `PageRender` = was der Producer **diese Frame** malen muss: Schlüssel, Pool-Slot, Weltrechteck mit Rand, `repaint`.
  `update()` nimmt an, dass der Producer die Liste noch in derselben Frame malt.

## 4. Grenzen und Risiken (ehrlich)

* **Gepinnte Mips bei vielen Tiles:** 2 gepinnte Mips = 5 Pages je Volume. Bei einem Volume je 512 m-Tile und ~800 geladenen Tiles (8 km Radius, Thema 326) wären das 4000 Pages Dauerlast.
  Für die Tile-Landschaft muss `pinnedMips` 0 bzw. 1 sein, oder die Ferne kommt aus einem gröberen Regions-Volume (Schritt 5). Offen.
* **Mehrere Volumes in einer Frame:** `update()` je Volume nacheinander. Ein früheres Volume darf Pages eines späteren verdrängen (sie sind älter), bis dieses `update()` läuft und sie neu anfordert. Bei ausreichend großem Pool kein Effekt; bei zu kleinem Thrash. Schritt 5 soll die Ansichten aller Volumes zuerst „berühren“ und dann verteilen.
* **Hysterese fehlt:** Pages außerhalb der Ansicht bleiben, bis der Platz gebraucht wird (LRU). Kein Zeitlimit.
* **Kein GPU-Code:** Pool, Page-Table-Textur, Producer-Pass, Consumer-Shader, Kompression, Debug-Ansicht kommen in den Schritten 2-7.
* `nearRadius` ist hier ein fester Wert in Metern. Die Ableitung aus Auflösung/FOV (wie `planHlodBake`) gehört zum Consumer (Schritt 3), der weiß, welche Texeldichte der Schirm will.

## 5. Verifikation dieses Schritts

* `tests/test_rvt_core.cpp` (9 Fälle, 1002 Prüfungen), eingetragen in `tests/CMakeLists.txt`, `RuntimeVirtualTexture.cpp` in `src/HE_Scene/CMakeLists.txt`.
* Gebaut und gelaufen **eigenständig** (MSVC `cl`, doctest-Main, nur `RuntimeVirtualTexture.cpp` + Test): 9/9 grün.
* Negativkontrolle: ohne das „Page ist in Sicht, also anfassen“ in `update()` schlägt der Eviction-Fall fehl (5 Prüfungen rot). Der erste Anlauf dieses Tests hat die Mutation **nicht** gefangen und wurde deshalb verschärft (geteilte Pages beider Ansichten, `evictions == 15`).

## Handoff

- **Topic / step:** 333, Schritt 1 von 8 (Entwurf + CPU-Kern)
- **Branch / commit:** `claude/runtime-virtual-textures` (von `release/0.8.0`), d3c8d86e, nicht gepusht
- **Done:** Entwurf mit Entscheidungen und rein CPU-seitiger Kern (Layout, Seitenbedarf, Budget, Dirty-Rects, gemeinsamer Pool mit LRU, Page-Table-Daten mit Fallback), getestet.
- **Files touched:** `src/HE_Scene/include/HorizonScene/RuntimeVirtualTexture.h`, `src/HE_Scene/src/RuntimeVirtualTexture.cpp` (neu); `src/HE_Scene/CMakeLists.txt` (+1 Quelle); `tests/test_rvt_core.cpp` (neu), `tests/CMakeLists.txt` (+1); dieses Doc.
- **Verified:** Eigenständiger MSVC-Build + doctest, 9/9, Negativkontrolle (§5); voller `he_tests`-Debug-Build, `--source-file=*test_rvt_core.cpp`: 9/9, 1002 Prüfungen. Build nur mit `-DDEPLOY_DIR=<repo>/out/deploy-rvt`, sonst scheitert/klobbert der Deploy-Schritt den laufenden Editor.
- **Not verified:** Linux/macOS-Compiler und CI (kein Push). Kein GPU-Code, nichts im Editor gesehen.
- **Decisions taken:** alles in §2 mit „Entscheidung“; Wetter/Schnee/Trails statisch ist **nur vorgeschlagen**.
- **Pending (Reihenfolge):** Schritt 2 Producer-Pass auf D3D11 (NN-WS03), 3 Consumer-Shader, 4 Invalidierungs-Hooks, 5 Segmentierung (Pinned-Mips-Problem, §4), 6 übrige Backends, 7 Editor/Serialisierung, 8 Abnahme.
- **Traps:** Hive-Deadline war abgelaufen, `hive_dispatch` lehnt ab (nur der Mensch setzt sie), Schritt 1 lief deshalb in der Session des Menschen. Ein `cmd /c`-Build vom Bash-Tool aus baut nichts (Memory `build-test-cli-workflow`); `HE_API` gehört nicht an HE_Scene-Klassen.
- **Next steps:** 1. `cmake --build out/build/x64-debug --target he_tests` (PowerShell + vcvars) und `he_tests --source-file=*test_rvt_core.cpp`. 2. Schritt 2: `IRenderer`-Vertrag für Pool + Page Table + `RenderRvtPage`, D3D11 zuerst; Material-Shader mit Page-Rechteck als Uniform. 3. An 326 posten, bevor `kLandscapeFS` angefasst wird.
