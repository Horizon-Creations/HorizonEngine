# Landscape im Weltmaßstab: Foliage und Wasser je Kachel (Schritt 3, Teil b und c)

Stand 31.10.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 3 von 8.
Vorher: `landscape-world-scale-store-2026-10-31.md` (Store, Kachel-Format), `-renderer-` (Quadtree, Laufzeit, Shader-Quelle), `-physics-` (Boden je Kachel).

**Foliage (3b) und Wasser (3c) sind als CPU-Daten und Laufzeit gebaut und getestet. Gezeichnet wird nichts davon (Teil B, braucht Vulkan). Navigation ist offen.
Es wurde nichts gemessen: keine Streuzeit, keine Aufbauzeit der Wasserregion, kein Speicher im Betrieb (Plan-§4-Abnahme auf NN-WS03).** Schritt 1521 bleibt offen.

## 1. Was gebaut ist

| Datei | Aufgabe |
|---|---|
| `FoliageSystem::scatterLayer` (öffentlich) | der Streukern der einen Landschaft, einzeln aufrufbar: Terrain + Ebene -> `FoliageStore` |
| `LandscapeTileTerrain.*` | Kachel -> `TerrainComponent` (`landscapeTileTerrain`), Mitte der Kachel (`landscapeTileCentre`), n x n Kacheln -> ein Terrain samt Wasserfeld (`landscapeRegionTerrain`) |
| `LandscapeFoliage.*` | Streuung je (Kachel, Ebene) auf dem Job-Pool, Entladen mit Hysterese, `draws()` |
| `LandscapeWaterData.*` | Tabelle der Gewässer im Pack (`HLW1`) |
| `LandscapeWater.*` | Wasserregion um die Kamera, `surfaceAt` |
| `LandscapeTile`, `LandscapeSource`, `LandscapeStore` | Wasser-Ströme in der Kachel, Tabelle im Pack, Meer im prozeduralen Quellen (`seaLevelM`), Wasserraster in der Kachelgröße |
| `LandscapeRuntime` | `setFoliageLayers`, `foliageDraws`, `waterRegion`, `waterSurfaceAt`, `water()` |
| `tests/test_landscape_foliage.cpp`, `test_landscape_water.cpp` | 6 und 14 Testfälle |

## 2. Foliage

* **Je (Kachel, Ebene) ein unveränderlicher `FoliageStore`**, kachel-lokal (um die Kachelmitte), dazu `centre` absolut. Eine Ebene braucht eine Kachel, wenn sie näher als `drawDistance` ist, und behält sie bis `drawDistance · 1,25`. 80 m Gras liegt auf einer bis vier Kacheln, eine Baumebene mit 1 km auf einigen Dutzend.
* **Seed je Kachel** = Seed der Ebene, gemischt mit den Kachelkoordinaten. Eine Kachel, die geht und wiederkommt, streut dieselben Pflanzen, in jeder Reihenfolge, in der die Kamera sie trifft.
* **Adressstabilität:** ein Store wird nie hinter dem Renderer neu gebaut; `FoliageLodMemory` schlüsselt auf den Zeiger. Er bleibt dasselbe Objekt, bis die Kachel fällt oder die Ebenen wechseln.
* **Dichtemasken werden ignoriert.** `FoliageLayer::densityMask` ist ein 128 x 128-Raster über 0..1 der Landschaft: über 100 km wären das 780 m je Texel. Masken je Kachel sind Autorendaten und kommen mit dem Editor (Schritt 6). Bis dahin streuen die Ebenen nur nach ihren Regeln (Neigung, Höhe, `weightLayer` gegen die Kanäle der Kachel).
* **Draw-Vertrag (Teil B):** `LandscapeFoliageDraw { tile, centre, layer, store }`. Der Renderer nimmt den Store durch dieselbe Bucket-Logik wie `FoliageExtract.cpp`, mit **`centre − floatingOrigin`** als Terrain-Position.
* **Speicher:** der Job hält Kachel und Ebene nur bis zum Ergebnis. Die Ebene trägt `cachedInstances` (die weltraum-Kopie jeder Instanz, 16 MB bei 262 000 Instanzen je Kachel und Ebene), die niemand liest; sie wird nach dem Streuen freigegeben, nicht erst mit dem Job.

## 3. Wasser: Format

**Kachel** (`HLT1`, Version bleibt 1, Flag Bit 1 = Wasser). Zwei Ströme hängen hinter den Gewichten, beide im Raster der Gewichte Stufe 0 (`wTexels²`, 4 m):

| Strom | Inhalt | mit Prädiktor |
|---|---|---|
| Deckung | `u8` je Zelle, 255 = ganz Wasser (`water::Field`-Deckung) | Zeilendelta wie bei den Gewichten |
| Eigner | `u16` je Zelle (little endian), 0 = kein Gewässer | Ebenen-Prädiktor der Höhen, ein gleichbleibender Eigner ist lauter Nullen |

Angehängt, kein Versionssprung (Regel aus `HAsset.h`): ein alter Leser hört nach den Gewichten auf und sieht das Flag nie; ein Blob ohne Flag liest sich wie vorher. Beide gesetzt oder keiner.

**Tabelle im Pack** (`HLW1`, Version 1, Offset `56` im Kopf von Pack v2; 0 = keine Tabelle, v1-Packs lesen weiter):

| Feld | Bytes |
|---|---|
| `u32` magic, `u32` version, `u32` byteSize (ganze Tabelle) | 12 |
| `u8` clipToGround, `f32` shoreOvershoot, `u8` tessellation, `u32` count | 10 |
| je Gewässer: `u16` id, `f32` level, Physik (`u8` + 6 `f32`), Unterwassernebel (8 `f32`), Kaustik (4 `f32`), Tessellation (2 `f32`) | **87** |

Gesamt 22 + 87 · count. Der Leser verweigert: falsches Magic oder Version, `byteSize` ungleich Blob, ein `count`, den der Blob nicht fasst, Id 0 oder doppelt, einen Pegel, der keine Zahl ist. `level` ist **absolut in Metern**; das Terrain einer Kachel sitzt bei y = 0 mit absoluten Höhen, lokaler und absoluter Pegel sind dieselbe Zahl. Ids sind weltweit (ein See über vier Kacheln ist ein Körper mit einem Pegel), die der einen Landschaft sind je Terrain.

## 4. Wasser: Region

* **Eine Region aus n x n Kacheln ist ein `TerrainComponent` samt `water::Field`, nie eine Kachel einzeln.** Grund (gefunden, nicht vermutet): die Oberfläche je Kachel gibt an jeder Kachelkante eine Naht, weil `buildSurface` die Küstenlinie nur im eigenen Feld kennt. Mit allen n² Kacheln in einem Feld läuft die Fläche über die inneren Kanten.
* **n ≤ 4.** Ein Terrain hält `kTerrainMaxResolution` = 1025 Proben je Seite; 4 · 256 + 1 passen, 5 · 256 + 1 nicht. `update()` begrenzt n auf `(1025 − 1) / hCells` und warnt einmal. Eine Welt mit weniger Kacheln als n bekommt n = Kacheln je Seite.
* **Hysterese:** Ursprung = `clamp(Kamerakachel − n/2, 0, Kacheln − n)`; neu gebaut wird nur, wenn die Kamera die inneren Kacheln `[o+1, o+n−2]` verlässt (bei n = 4 die inneren 2 x 2) und der gewünschte Ursprung vom jetzigen abweicht. Eine fertige Region ersetzt die alte erst, wenn sie fertig ist; eine zu spät fertige wird verworfen (`discarded`).
* **Ein fehlgeschlagener Bau** (`landscapeRegionTerrain` gibt false) zählt `failed`, behält die alte Region und wird für denselben Ursprung nicht jedes Bild wiederholt, sondern erst, wenn die Kamera ein anderes Ziel wählt. Vorher hing `building` für immer.
* **Draw-Vertrag (Teil B):** je Fläche `LandscapeWaterSheet { body, level, surface, params }`. Die Fläche wird an **`centre + (surface.center.x, level, surface.center.y) − floatingOrigin`** gezeichnet; das Netz selbst liegt bei y = 0.
* **`SurfaceOptions::clipToGround` steht standardmäßig auf aus.** Es muss aus dem `Field` übernommen werden (`LandscapeWater` tut das, ebenso `shoreOvershoot`); ohne das liegt die Fläche über dem Ufer.
* **Speicher:** die Region hält ihr ganzes Terrain (1025², mit Gewichten und Feld rund 8 MB), weil `surfaceAt` es braucht. Beim Wechsel halten kurz zwei. Der Job lässt seine n² Kacheln (rund 5 MB) los, sobald das Ergebnis gebaut ist.

## 5. Fallen

* **Eine Kachel ohne Wasser** in einer Region mit Wasser bekommt trotzdem ihr Feld (sonst fehlt das Wasser im Raster der Nachbarn).
* **Die Küste ist so fein wie eine Wasserzelle** (4 m). Die Sehne der Konturlinie über eine Zelle ist die Genauigkeit des Ufers; der Test mit ebener Böschung misst 0,14 m gegen erwartete 0,25 m (16-bit-Höhenstufe) und die Fläche stimmt auf 1e-3.
* **Kleine prozedurale Welten sind viel rauer als echtes Gelände.** `ProceduralLandscapeSource` normiert fBm über `max(tilesX, tilesZ) · tileM`; eine Welt mit 8 Kacheln zwängt 12 Oktaven in 4 km, und das 16-m-Raster der Tests tastet reines Rauschen ab (kleinste Wellenlänge 0,33 m, Steigungen bis 8,9 m/m). Daran ist die Sehnen-Schranke des fBm-Tests wertlos geworden und nur noch eine Meldung; das Ufer prüft allein der Test mit der ebenen Böschung. Dasselbe Rauschen steckt in den gemessenen 0,2 m Jolt-Packfehler und in den Zählungen der Neigungsregel der Foliage.
* **Windows:** Dateien, die ein Job offen hält, lassen sich nicht löschen. Foliage und Wasser warten in ihren Destruktoren auf laufende Jobs (5 s), Tests geben den Store frei, bevor sie die Datei entfernen.

## 6. Was offen ist

* **Wasser jenseits der Region (2 km):** keine ferne Ebene, keine Wellen, keine Ripples, kein `WaterIndex`, kein Auftrieb, keine `WaterSurface`-Entitäten. `surfaceAt` antwortet ohne Wellen.
* **Foliage ignoriert Wasser** (Pflanzen können im See stehen), und Masken (§2).
* **Navigation:** nicht angefasst. Plan: Recast-Kacheln über das Region-Terrain, CPU-seitig und hier machbar.
* **Teil B (Vulkan, NN-WS03):** Zeichnen von Foliage und Wasser, GPU-Abnahme, alle Messungen.
* **Editor (Schritt 6):** Masken je Kachel, Pinsel und Seenwerkzeug schreiben Felder.

## 7. Verifikation

Verifiziert (`cmake-build-bench`, echte `he_tests`): `test_landscape_*` 106 Testfälle, `test_foliage*` 76, `test_terrain*` 155, `test_water*` 245, `test_physics*` 134, `test_world_scale*` 44, `test_scene_serializer*` 46, `test_cell_*` 34, alle grün; die Wasser-Suite dreimal hintereinander. CI des Foliage-Stands (68e07e61): Linux, Linux Vulkan und macOS grün, Windows beim Schreiben noch offen; die Wasser-Commits sind bis zum Push nicht in der CI.
Mutationsprüfungen früherer Schritte (zu frühes Morphen, Store ohne Verwerfen später Ladungen, alte Randsuche, kein Warten beim Aufräumen) schlugen an.
**Nicht verifiziert:** Zeiten, Speicher im Betrieb, Zeichnen, Navigation, ein Pack mit echtem Gelände (alle Tests laufen auf prozeduralen Quellen), Windows-CI der letzten Wasser-Commits bis zum Push.

## Handoff

1. Wasser (3c) liegt in `LandscapeRuntime` (`waterRegion()`, `waterSurfaceAt()`); Foliage (3b) in `foliageDraws()`. Beides zeichnet der Renderer in Teil B nach §2 und §4.
2. `kGroundOwner`, `holdsAt`, die Regionsgrenze n ≤ 4 und `clipToGround` aus dem Feld sind die Stellen, an denen es leicht falsch geht.
3. Offen in Schritt 3: Navigation (Recast-Kacheln über das Region-Terrain). Alles andere wartet auf NN-WS03.
