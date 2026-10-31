# Landscape im Weltmaßstab: Quadtree, Atlas, Laufzeit und Shader-Quelle (Schritt 2, Teil A)

Stand 31.10.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 2 von 8 (Quadtree-Renderer, Vulkan zuerst). Vorher: Messbasis (`landscape-world-scale-measurements-2026-10-31.md`),
Tile-Format und Store (`landscape-world-scale-store-2026-10-31.md`).

**Teil A ist alles, was ohne GPU und ohne Zeitmessung gebaut und getestet werden kann. Teil B (Backend-Anbindung, Pipeline, Bildabnahme, Zahlen) ist nicht gemacht:
auf diesem Mac gibt es keine Vulkan-SDK (`Vulkan backend disabled`), die Abnahme aus Plan §4 verlangt die 4070 (NN-WS03), und es wurde bewusst nichts gemessen.
Kein GPU hat eine Zeile von diesem Code ausgeführt.** Schritt 1520 bleibt deshalb offen.

## 1. Was gebaut ist

| Datei (`src/HE_Scene/{include/HorizonScene,src}`) | Aufgabe |
|---|---|
| `LandscapeQuadtree.*` | CDLOD-Auswahl: welche Patches in welcher Stufe, Quadranten-Masken, Frustum, Kachel-Verfügbarkeit, Morph-Funktion |
| `LandscapePatchMesh.*` | das eine 65 x 65-Gitter für alle Patches, vier Quadranten-Indexbereiche, die zur Triangulierung passende Morph-Regel |
| `LandscapeSource.*` (erweitert) | Pack Version 2 mit **Grobwelt** (32-m-Höhen, 128-m-Gewichte), Höhenskala im Kopf, Version 1 weiter lesbar |
| `LandscapeTile.*` (erweitert) | rechteckige Raster-Codecs für die Grobwelt (`HLG1`) |
| `LandscapeAtlas.*` | welche Kachel in welcher GPU-Schicht liegt, umlaufende Slot-Tabelle |
| `LandscapeGpuLayout.*` | wie Kachel-Mips und Grobwelt in GPU-Bilder gepackt werden |
| `LandscapeRuntime.*`, `Components/LandscapeWorldComponent.h` | besitzt Store + Atlas + Quadtree, folgt der Kamera, baut den `LandscapeFrame` |
| `SceneSerializer.cpp` | Komponente als `"landscapeWorld"` gespeichert und geladen |
| `src/HE_Rendering/include/HorizonRendering/LandscapeShaderSource.h` | `kLandscapeVS`, `kLandscapeFS` (GLSL 4.10) |
| `scripts/validate_embedded_shaders.py` | prüft die beiden Shader mit `glslangValidator` |
| `tests/test_landscape_{quadtree,render,runtime,shader}.cpp` | 10 + 8 + 7 + 4 Testfälle (plus Erweiterungen in `test_landscape_store.cpp`, 18 Fälle) |

## 2. Quadtree (CDLOD)

* Knoten der Stufe L ist `64 · cell0 · 2^L` breit (128 m für Stufe 0 bei 2 m Zelle, 8 km für Stufe 6), wird immer mit demselben 64 x 64-Gitter gezeichnet. Stufe L gilt von `range(L-1)` bis `range(L) = rangeFactor · Knotengröße`
  (6 x). Ein Kind außerhalb seiner Reichweite wird vom Elternknoten als **Viertel** gezeichnet (`quadrants`-Bitmaske, Bit `q = dx + 2·dz`).
* **Eine Entfernung für Auswahl und Morph:** horizontal + Abstand der Kamera zum Höhenfenster `[minY, maxY]` (`landscapeMorphDistance`), **ohne** die Höhe des Vertex. Das ist keine Geschmacksfrage: die Auswahl (Kasten-Entfernung)
  und der Morph (Vertex-Entfernung) müssen dieselbe Metrik sehen, sonst hat ein Vertex auf der Naht eine andere Entfernung als der Knoten angenommen hat. Ein erster Entwurf mit der echten 3D-Entfernung ließ Risse von 3,5 mm stehen
  (der Test fand sie); mit `morphStart = 0,15` (Bedingung `rangeFactor · morphStart ≥ √2` verletzt) wären es 0,55 m. **Der Vertex-Shader darf die Eigenhöhe nie in die Morph-Entfernung nehmen.**
* **Kachel-Stufen und Grobwelt.** Ein Knoten der Stufe ≥ 4 überspannt 16 und mehr Kacheln, die nie alle im Speicher sind. Daten für Stufen 0..`tileLevels` kommen aus den Kacheln des Stores, höhere aus der immer residenten Grobwelt.
  `tileLevels` ist **nicht** eine Konstante im Quadtree, sondern `coarse->heightLevel - 1` aus dem Pack (Standard 3: Zellen 2, 4, 8, 16 m aus Kacheln, 32 m und gröber aus der Grobwelt).
* **Fehlende Kacheln.** `nodeAvailable` verlangt für Stufen ≤ `tileLevels` alle Kacheln unter dem Knoten; sonst zeichnet der Elternknoten das Viertel gröber. Das Ergebnis deckt die Welt immer lückenlos (Test).
* **Wie weit der Store laden muss:** `rangeOf(tileLevels) + √2 · patchSize(tileLevels)` = 6144 + 1448 = **7592 m** (`fitsStore`); der Standard-Ladeabstand 8000 m passt. Mit einem 3-km-Store springen benachbarte Stufen
  um 2 statt höchstens 1 (gemessen im Test, `MESSAGE`); ein dauerhaft `Failed`-er Kachel bleibt am Rand eine Stufe stehen. `LandscapeRuntime::begin` verweigert den Start unter dieser Schwelle.
* **Weltrand.** Der oberste Knoten (131 km) ist größer als die Welt (100,35 km). Knoten, deren Ecke außerhalb liegt, werden übersprungen; ragt einer darüber hinaus, liest der Rand die letzte Zeile/Spalte
  und das Fragment wird verworfen, sobald absolutes `x > worldMaxX` oder `z > worldMaxZ` (Shader).
* **Patch-Anzahl** (nur Auswahl, **nicht gemessen**): Kamera in der Weltmitte, 70° Blickfeld: 286 von 820 Patches ohne Culling, im Plan-Band 300 bis 600 knapp darunter. Was das Zeichnen kostet, sagt das nicht.

## 3. Pack Version 2 und die Grobwelt

Kopf 64 Bytes (Version 1 hatte 32 und bleibt lesbar). Neu: Byte 25/26 `coarseHeightLevel` / `coarseWeightLevel` (255 = keine Grobwelt), 32 `u64 coarseOffset`, 40/44 `u32` Größen der beiden Raster-Blöcke,
48/52 `f32 heightMinM` / `heightScaleM`. Der Index folgt direkt hinter dem Kopf, die Kacheln danach, am Ende die Grobwelt (zwei `HLG1`-Blöcke, siehe `LandscapeTile.h`, "Grids").

| | Inhalt | 100 km |
|---|---|---|
| Höhen Raster 0 | die **eigenen Mip-4-Stützstellen der Kacheln** nebeneinander (= Kachel-Ebene 0, jede 16. Stelle), 32 m | 3137 x 3137 uint16 = 19,7 MB |
| Höhen Raster 1 ... | je punktabgetastet um 2: 64, 128, 256 m ... bis wenige Stellen, **eines je Quadtree-Stufe** | gesamt ≈ 26 MB |
| Gewichte | die gröbste Mip der Kacheln (128 m, 4 x 4 Texel je Kachel) nebeneinander | 784 x 784 x 12 = 7,4 MB |

* **32 m statt der 128 m aus dem Plan** (§3.2): die Stufen 4 und 5 (32/64 m Zellen, 6 bis 25 km Entfernung) hätten sonst keine Datenquelle. Aus denselben Stützstellen wie die Kachel-Mips gebildet, damit die Naht zwischen
  Kachel-Stufe 3 und Grob-Stufe 4 **exakt** ist (Test: Raster 0 gleich Kachel-Mip 4 gleich jede 16. Stelle von Ebene 0, auch an Kachelrändern).
* Der Schreiber füllt die Grobwelt beim Hinzufügen jeder Kachel (`add(key, blob, coarseSample)`; ohne Probe dekodiert er den Blob), nicht erst am Ende. Reihenfolge der Kacheln egal (Test).
* Die **Höhenskala** (Meter für 0 und für 65535) steht im Pack, nicht in der Szene: zwei Szenen können ein Pack nicht mit verschiedenen Skalen lesen. Die Laufzeit nimmt die des Packs, wenn es eine nennt (Skala > 0), sonst die der Komponente.

## 4. Das gemeinsame Gitter und die Morph-Regel

`LandscapePatchMesh::build(64)`: 65² Vertices, adressiert als `(i, j)`; **jedes Quad an der Diagonale `(i,j)–(i+1,j+1)`**, vier Quadranten-Indexbereiche in der Reihenfolge `q = 0..3`, ein ganzer Knoten ist ein Draw
über alle Indizes (24 576). Morph-Regel (`landscapeMorphSource`): Vertex ohne ungerade Indizes bleibt; ungerade i: Mitte von `(i±1, j)`; ungerade j: Mitte von `(i, j±1)`; beide ungerade: Mitte der Diagonalen
`(i-1,j-1)–(i+1,j+1)`. **Mit dieser Diagonale liegt bei vollem Morph jedes feine Dreieck in der Ebene eines groben Dreiecks** (Test mit zufälligen Höhen); also ist ein voll gemorphtes Patch genau das Patch der nächsten Stufe.
Wasserdicht (jede innere Kante in zwei Dreiecken, 256 Randkanten in einem), einheitliche Windung (+y-Normale), kein Dreieck doppelt (Test).

## 5. Atlas und GPU-Bilder

`LandscapeAtlas::sync(store, uploads, releases, tableUpdates, maxUploads)` folgt dem Store: Kachel weg → Slot frei; neu → Slot (höchstens `maxUploads` je Aufruf). Eine Kachel gilt ab dem `sync`, das ihren Upload liefert, als im Atlas;
das ist die Verfügbarkeit, die der Quadtree fragt. Ein Patch der Stufe 3 überspannt 2 x 2 Kacheln, also schlägt der Vertex-Shader den Slot **je Vertex** in einer umlaufenden Tabelle nach
(`(z & (D-1)) * D + (x & (D-1))`, D = 64): beim Wandern ändern sich nur die Einträge der kommenden und gehenden Kacheln. Fenster zu klein (Aliasing) wird gezählt, nie vermischt; `begin` prüft `D ≥ 2·ceil(Haltering/tileM) + 2` (hier 64 gegen 42).

| Bild | Format | Layout | Größe für die Standardkachel |
|---|---|---|---|
| Kachel-Höhen, eine Schicht je Slot | R16UI | Ebene 0 links oben, alle weiteren Ebenen als Spalte rechts davon untereinander (`LandscapeTileImageLayout`) | 386 x 258 = 99 588 Texel = 199 KB (13 % Verschnitt) |
| Kachel-Gewichte, drei Arrays ("Seiten") | RGBA8 | gleiches Mip-Layout, Kanal `4p + c` in Seite `p` Kanal `c` | 192 x 128 je Seite = 98 KB, drei Seiten 295 KB |
| Slot-Tabelle | R16UI | D x D, `65535` = nicht im Atlas | 8 KB |
| Grobwelt-Höhen | R16UI | alle Raster übereinander, Zeilen-Versatz je Raster (`LandscapeCoarseImageLayout`) | 3137 x 6281 ≈ 39 MB |
| Grobwelt-Gewichte | 3 x RGBA8 | das feinste Gewichtsraster | 2,5 MB je Seite |

Pro Slot also 494 KB GPU-Speicher (roh 428 KB). **Bei 8 km Radius (767 Kacheln) ≈ 379 MB, bei vollem Budget (836 Slots) ≈ 413 MB** plus Grobwelt ≈ 45 MB: unter dem 0,5-GB-Ziel des Plans, aber nicht bei 12 km (722 MB roh). Schichtzahl 836 liegt unter dem
2048-Limit aller Backends. Die Packfunktionen (`landscapePackTile`, `landscapePackCoarse`) liefern die Bytes, die ein Backend in die Texturen kopiert.

## 6. Laufzeit und Frame

`LandscapeRuntime` (wie `CellRuntime`): `begin(source | pack, config)` leitet den Quadtree aus der Geometrie ab und prüft, was zusammenpassen muss (Grobwelt-Raster = erste grobe Stufe, `fitsStore`, Fenster, Kappen des Shaders:
8 Höhen-Mips, 12 Gewichtskanäle, 16 Grobwelt-Raster); `update(world, dt)` findet die erste aktive `LandscapeWorldComponent`, startet/beendet daran, bewegt den Store mit der Kamera (absolute Position = Transform + `world.origin()`,
Geschwindigkeit als Vorausschau, Sprung > 300 m/s zählt nicht); `buildFrame(viewProjRelative, origin, zeroToOne, out)` macht aus der Kamera des eben gerenderten Bildes den `LandscapeFrame`.
Das Frustum wird in den **absoluten** Raum gelegt (`d_abs = d − n·origin`); Positionen im Frame sind ursprungsrelativ.

| Uniform (`LandscapeShaderSource.h`) | kommt aus |
|---|---|
| `uViewProj` | die Kamera, die Anwendung hat sie |
| `uCameraRel` (xyz, w) | Kamera ursprungsrelativ, `frame.slabDistance` |
| `uWorld` (x, y, z, w) | `frame.worldMaxX`, `worldMaxZ`, `tileM`, `heightScaleM` |
| `uHeightInfo` | `frame.heightMinM`, `tilesX`, `tilesZ`, `tableWindow` |
| `uOrigin` | `frame.origin` |
| `uTile` (x, y, z) | Geometrie (`hCells`, `wTexels`), `frame.tileLevels` |
| `uHeightRect[8]` | `LandscapeTileImageLayout::heightRect` |
| `uCoarseGrid[16]` | `LandscapeCoarseImageLayout::grids` (Breite, Höhe, Zeilen-Versatz) |
| `uPatch`, `uPatchPos`, `uMorph` je Draw | `frame.patches[i]`: `level`, Eck-Zelle `ix·64` / `iz·64` (`ix`, `iz`), `coarse`, `x0/z0/size`, `gridIndex`, `morphStart/End` |
| `uTileHeights`, `uSlotTable`, `uCoarseHeights` | die GPU-Bilder aus §5, Slot-Tabelle aus `frame.tableUpdates` |

Der Shader will die **ganzzahlige** Eck-Zelle des Knotens (`ix · 64`, `iz · 64` in der Gitterweite der Stufe), damit die Höhenabfrage exakt auf dem Gitter bleibt und nicht aus ursprungsrelativen Floats rechnet; deshalb trägt `Patch` `ix` und `iz` (Test: `ix · size == x0 + origin.x`).

**Jeder gebaute `LandscapeFrame` muss ausgeführt werden:** Uploads und Tabellen-Änderungen werden aus der Laufzeit herausgenommen, der Atlas zählt sie als erledigt. Ein übersprungenes Bild heißt, dass der Shader Kacheln als "nicht da" liest.

## 7. Shader

* GLSL 4.10 wie `kSkyFS`, lose Uniforms, keine Layout-Qualifier im Rumpf (ein Vulkan-Vorspann in Teil B bildet die Namen auf einen Block ab, `-V` scheitert hier wie beim Himmel an `location`).
* Vertex-Eingabe nur `aGrid = (i, j)`; Vertexpuffer 65², `v → (v % 65, v / 65)`; die `64` steckt im Shader (`/ 64.0`, `uPatch.yz`).
* **Höhenabfrage `heightOfTile`:** Kachel `g / cpt` (begrenzt auf die Welt), Slot aus der umlaufenden Tabelle, Stelle im Bild über `uHeightRect[level]`. Ein Gitterpunkt auf einer Kachelkante gehört zu beiden Kacheln (geteilte Randstelle):
  fehlt die, in der er liegt, werden die Nachbarn probiert, die ihn teilen (bis zu vier an einer Ecke), und fehlt jede, antwortet das **feinste Grobwelt-Raster** (`g >> (tileLevels + 1 − level)`): am Rand der geladenen Scheibe behält das letzte feine Patch
  seine Kante, und eine noch nicht geladene Kachel zeigt gröberen Boden statt eines Lochs. (Dieser Fehler wurde in der Durchsicht gefunden, nicht vom Übersetzer; der Replay-Test deckt ihn jetzt ab und schlägt mit dem alten Lookup an.)
* **Morph:** ungerade Vertices blenden zur Mitte ihrer Nachbarn **derselben Stufe** (1 bis 2 Zusatzabfragen), `m = clamp((d − morphStart)/(morphEnd − morphStart), 0, 1)`; `morphStart/End` kommen je Draw aus dem Frame, im Shader steht keine Konstante.
* **Fragment:** Weltrand-`discard` in absoluten Koordinaten; Gewichte der eigenen Kachel, nächster Texel, drei RGBA8-Seiten, flache Ebenenfarben `uLayerColor[12]`, Normale aus `dFdx/dFdy` der Weltposition (flach schattiert, **keine Normal-Textur**).
  **Die Grob-Patches werden grau: die Grobwelt-Gewichte sind gepackt, aber der Shader liest sie noch nicht.**

## 8. Entscheidungen (mit Grund)

| Entscheidung | Grund |
|---|---|
| `maxLevels = -1` (ganze Pyramide) | Die obersten Mips (5, 6) sind unter 1 % der Bytes; das, was kostet, sind die Gewichte der Ebene 0 (196 KB von 321 KB). Der Renderer braucht Mip L und L+1 an derselben Stelle; Stufe 0 allein hieße GPU-Mips oder Unterabtastung. **321 MB bei 8 km.** Weitere Einsparung wäre "Gewichte Ebene 0 nur nahe der Kamera", kein Weglassen von Mips (Schritt 5) |
| Normalen aus Ableitungen im Fragment-Shader | keine zusätzlichen Abfragen am Rand der Patches (dort können Kacheln fehlen), keine Normal-Textur, die in Teil B erfunden würde; Preis: Facetten, Normalen springen an Morph-Grenzen |
| Standalone-Fragment mit flachen Farben | `uvTiling` (siehe unten) |
| `LandscapeFrame` nicht im `RenderExtractor` | der Extraktor füllt Draws in parallelen Arbeitern und baut vorher die GI-Landschaftstabelle (`kGiMaxLandscapes = 4`) für `TerrainComponent`; `LandscapeStore`/`Atlas` sind Hauptthread-Objekte. Die Anwendung besitzt die Laufzeit wie `CellRuntime`, die GI-Tabelle bleibt unberührt (auch Schritt 4 ist getrennt) |
| `tileLevels` aus dem Pack, `LandscapeRuntime::begin` prüft Raster, Radius, Fenster, Kappen | zwei Konstanten, die auseinanderlaufen könnten (Quadtree 3, Pack 4), sind ein stiller Riss |
| Höhenskala im Pack | siehe §3 |
| Komponente serialisiert (`landscapeWorld`) | Teil B auf NN-WS03 braucht eine Test-Szene, die man laden kann; alte Szenen laden unverändert (Schlüssel fehlt) |
| Weltrand: `discard` im Fragment-Shader | der oberste Knoten überragt die Welt; kein Clip-Extent je Patch nötig |

**`uvTiling` (Plan §5, geprüft):** die Material-Pfade nehmen **eine** UV über die ganze Landschaft an (`HlslSources.h:1513` "z = uvTiling, w = layer count", `VulkanRenderer.h:1630` "xy = 1/size, z = uvTiling"; die Chunk-Meshes schreiben globale UVs 0..uvTiling).
Bei 100 km bräuchte das Material Welt-Koordinaten-UVs (`uv = world.xz / textureMeters`) und die Gewichte aus dem Atlas statt einer Gewichts-Textur; das ist Material-Graph-Arbeit über alle Backends und gehört nach Teil B, nicht in einen Shader ohne GPU zum Ausprobieren.

## 9. Teil B (nicht gemacht, in dieser Reihenfolge)

1. Vulkan (NN-WS03): `kLandscapeVulkanPrelude` wie beim Himmel (Block statt loser Uniforms), Pipeline, Vertex-/Indexpuffer aus `LandscapePatchMesh`, die Bilder aus §5, ein `IRenderer`-Eintrag, der einen `LandscapeFrame` nimmt und die Uploads **vor** dem Zeichnen ausführt.
2. Zwei Aufrufstellen in der Anwendung: `m_landscape.update(*m_world, gameDt)` neben `updateCellStreaming` (`GameApplication.cpp:3297`) und `buildFrame(viewProj, m_world->origin(), …)` direkt vor dem Renderer-Aufruf; `zeroToOne` je Backend.
3. Grobwelt-Gewichte im Fragment-Shader; Material mit Welt-UVs (§8); Normale glätten.
4. D3D11/D3D12 über SPIRV-Cross wie der Himmel, GL, Metal (MSL-Spiegel, `kLandscape*` ist dort Handarbeit).
5. Abnahme (Plan §4): Terrain-CPU < 0,5 ms, GPU < 1,5 ms, keine Risse, kein Popping über 1 px: **auf NN-WS03, gemessen**.

## 10. Verifikation

* Verifiziert (Mac, ohne GPU, ohne Zeitmessung): alle genannten Tests im echten `he_tests` (Quadtree, Gitter, Atlas, Layout, Laufzeit, Shader-Quelle, Store), Nachbarn `test_cell_*`, `test_terrain*`, `test_scene_serializer` grün; `glslangValidator` übersetzt `kLandscapeVS`/`kLandscapeFS`
  (60 Shader im Skript, 0 Fehler). **Der Übersetzer prüft nicht, ob die Fragment-Eingaben vom Vertex-Shader geschrieben werden** (`glslangValidator -l` ließ eine fehlende Eingabe durch); das tut `test_landscape_shader.cpp`.
  Absichtliche Fehler wurden gefunden: Morph zu früh (Risse 0,55 m), alter Randlookup, Typfehler im Shader.
* Nicht verifiziert: jede GPU-Ausführung; Windows (MSVC) und Linux (gcc) für den neuen Code, bis ein CI-Lauf ohne weiteren Push durch ist (Läufe wurden durch Folge-Pushes abgebrochen); Zeiten jeder Art; Verhalten mit echten Heightmaps;
  Grenzfälle der Hysterese bei langer Bewegung über die Weltgrenze; `ProceduralLandscapeSource::coarse()` für die ganze 100-km-Welt (erzeugt jede Kachel, Minuten).

## Handoff

- **Topic / step:** Hive-Topic 326, Schritt 2 von 8, **Teil A** (Teil B offen, Schritt bleibt offen)
- **Branch / commit:** `claude/landscape-weltmassstab-100km` (PR 214 deckt Schritte 0, 1 und 2 A), letzter Code-Commit siehe PR (pushed: ja, nach dem Dokument)
- **Done:** Auswahl (CDLOD), Pack v2 mit Grobwelt, Patch-Gitter und Morph-Regel, Atlas, GPU-Layouts, Laufzeit und Frame, serialisierte Komponente, zwei Shader (übersetzt), Verträge als Tabellen.
- **Files touched:** siehe §1; zusätzlich `src/HE_Scene/CMakeLists.txt`, `tests/CMakeLists.txt`, `src/HE_Scene/src/SceneSerializer.cpp` (Schlüssel `landscapeWorld`), `scripts/validate_embedded_shaders.py`.
- **Verified:** siehe §10.
- **Not verified:** keine GPU, keine Zeiten, MSVC/gcc-CI für den neuen Code offen, echte Heightmaps.
- **Decisions taken:** §8. Dazu: Entfernung = horizontal + Höhenfenster; Grobwelt 32 m aus den Kachel-Stützstellen; Frame nicht im Extraktor.
- **Decisions suggested, not taken:** Gewichte der Ebene 0 nur nahe der Kamera halten (Schritt 5); Vulkan-Block-Layout für die Uniforms; Materialintegration mit Welt-UVs.
- **Pending (Mensch):** Raster/Authoring (von Hand malen oder Generator/Import) weiter offen; Freigabe, Teil B auf NN-WS03 zu machen.
- **Traps:** zwei `ninja`-Läufe im selben Build-Ordner scheitern; in zsh teilen unquotierte Variablen nicht auf (Compiler-Aufrufe über `bash -c`); jeder Push bricht den laufenden CI-Lauf ab, also erst alles committen, dann einmal pushen;
  Windows kann keine Datei löschen, die noch ein Leser offen hält (Tests setzen `pack.reset()` vor `remove`); der Frame muss ein ganzer sein (§6).
- **Next steps:** 1. Teil B auf einem Rechner mit Vulkan-SDK (NN-WS03), Punkt 1 und 2 aus §9.  2. Alternativ, rein CPU und hier testbar: Schritt 3, ein Jolt-`HeightFieldShape` je residenter Kachel (`PhysicsWorld`, Store als Quelle).
