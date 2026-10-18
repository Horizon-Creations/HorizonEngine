# Scene Slice 8 – TerrainSystem, TransformHierarchy, UIInputSystem, UISystem, Water*, Weather* (src/HE_Scene/src)

## Zusammenfassung

Der Slice ist insgesamt sorgfältig gebaut: Transform-Propagation hat Dirty-Scan, Stats, Verify-Modus und Tests; Wasser und Terrain haben eigene Dirty-Rechtecke und Hash-Gates, und Kommentare begründen Entscheidungen. Die Schwachstellen liegen in den Per-Frame-Pfaden, die trotz sauberer Dirty-Logik an anderer Stelle jeden Tick über alle Chunks, Elemente oder Splines laufen und dabei Heap-Container anlegen. Das sind die Terrain-Tessellation, die Chunk-Material-Synchronisation, der UI-Extract und die Wasser-Spline-Synchronisation. Dazu kommen Wiederholungen im Wassercode (FNV-Hash, Punkt-Segment-Distanz, `hasArea` jeweils 3–5-fach) und node-basierte Maps in der Contour-Extraktion. Insgesamt 15 Funde: 2 Hoch, 8 Mittel, 5 Niedrig. Es gibt keine Messdaten; alle Aufwandsschätzungen sind Schätzungen.

---

### SCN-801 – updateTessellation läuft jeden Tick über alle Chunks, auch wenn Tessellation aus ist
- **ID**: SCN-801
- **Kategorie**: D (ECS ohne Dirty-Flag), C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:715-768 (aufgerufen pro Tick aus SceneSystems.cpp:150)
- **Problem**: Vor der Entscheidung, ob ein Terrain überhaupt Tessellation nutzt (`tessFactorOf(tc) <= 1u`, Zeile 779, Default `tessellationFactor = 1`), wird für JEDEN `TerrainChunkComponent` der Welt `HE::worldPositionOf` (Hierarchie-Walk plus Matrixmultiplikationen) berechnet. Außerdem werden pro Chunk ein `unordered_map`-Lookup für den Besitz-Check (750-752) und ein `push_back` in `byTerrain` (`unordered_map<uint32_t, vector<Near>>`, 742) gemacht. Ein 2048er-Terrain hat bis zu 32×32 = 1024 Chunks, und `byTerrain` plus ein Vektor pro Terrain werden jeden Frame neu allokiert. Bei ausgeschalteter Tessellation (Normalfall) ist das reine Verschwendung.
- **Code-Auszug**:
```cpp
for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
{
    ...
    const float d = glm::distance(cameraPos, HE::worldPositionOf(world, ce));
    byTerrain[static_cast<uint32_t>(cc.terrain)].push_back({ ce, d });
}
```
- **Lösung**: (1) Erste Schleife über die Terrains (`view<TerrainComponent>`, wenige Einträge) und für jedes `active = tessFactorOf(tc) > 1 && D > 0` bestimmen. (2) Sind alle Terrains inaktiv, wird nur noch die Chunk-Liste aufgeräumt, die noch `cc.tessActive` oder `tessMeshId != UUID{}` hat. Dafür gibt es `g_tessMeshes` (leer im Normalfall); ist `g_tessMeshes` für die Welt leer und `active` leer, früh `return`. (3) Distanz erst berechnen, wenn das Terrain des Chunks aktiv ist. (4) `byTerrain` nur mit aktiven Terrains füllen (oder `thread_local`/Member-Scratch wiederverwenden).
- **Erwarteter Effekt**: Schätzung: bei Tessellation aus von O(Chunks) mit ~1000 Hierarchie-Walks pro Frame auf O(Terrains) (~0). Bei aktiver Tessellation bleibt der Aufwand, sinkt aber um die Allokationen.
- **Risiko / Verifikation**: Das Aufräumen von Chunks mit Fremd-`tessMeshId` (748-762, „copied component“) muss erhalten bleiben. Dafür die bestehenden Terrain-Tessellationstests in `tests/` laufen lassen (`grep -l tessellation tests/`); ein Test mit kopiertem Chunk ergänzen, falls keiner existiert.

### SCN-802 – Chunk-Material-Sync: O(Terrains × Chunks) jeden Tick, ungated
- **ID**: SCN-802
- **Kategorie**: D, C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:431-469
- **Problem**: Der Kommentar sagt ausdrücklich „Re-synced every tick (NOT gated on tc.dirty)“. Dabei wird pro Terrain (a) das `MaterialComponent` des Terrains per Wert kopiert (`const MaterialComponent src = ...`, inklusive des `paramOverrides`-Vektors mit String-Namen, also Heap-Kopien jeden Tick), (b) die ganze `view<TerrainChunkComponent>` durchlaufen und mit `cc.terrain != te` gefiltert. Das sind T×C Iterationen statt C. Dazu kommen pro Chunk `try_get<MaterialComponent>` und `same()` mit String-Vergleich je Override. Zusätzlich lebt `needMat` als Heap-Vektor pro Terrain und Tick.
- **Code-Auszug**:
```cpp
const MaterialComponent src = reg.get<MaterialComponent>(te);
...
for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
{
    if (cc.terrain != te) continue;
    auto* dst = reg.try_get<MaterialComponent>(ce);
    ...
    if (same(*dst)) continue;
```
- **Lösung**: (1) Das Parent-Material nicht kopieren, sondern per `const MaterialComponent&` lesen. Das nachfolgende `emplace_or_replace` auf Chunks kann die Pool-Referenz invalidieren, deshalb zuerst alle Chunk-Updates (`dst`-Schreibzugriffe laufen auf einem anderen Pool-Eintrag, sind sicher) und erst danach die `needMat`-Emplaces ausführen, oder `src` nur im seltenen `needMat`-Fall kopieren. (2) Eine einzige Chunk-Schleife über die View, in der `cc.terrain` per kleinem Lookup (Vektor `(terrainEntity, const MaterialComponent*)`, T ist klein) auf die Quelle abgebildet wird. (3) Weiter reduzieren: Terrain-Material mit einem Hash/„revision“ der Quelle (materialAssetId + overrides) vergleichen und nur bei Änderung über die Chunks laufen. Dafür braucht das `MaterialComponent` einen Änderungsmarker, den Editor-Schreiber setzen; ohne den bleibt (2) der sichere Schritt.
- **Erwarteter Effekt**: Schätzung: (1)+(2) entfernen den T-fachen Faktor und die Heap-Kopien. Bei 1 Terrain × 1024 Chunks bleiben ~1000 `try_get`+Vergleiche pro Frame; (3) würde das auf ~0 bringen.
- **Risiko / Verifikation**: Das Verhalten „Material-Swap und Param-Overrides erreichen die Chunks ohne Rebuild“ muss erhalten bleiben (bestehender Test zum Landscape-Material suchen: `grep -ln "kDefaultTerrainMaterialId" tests/`). (3) hängt von zuverlässigem Dirty-Setzen aller Material-Schreiber (Inspector, MCP, Skript-API) ab; deshalb erst nach (1)+(2) und mit Messung.

### SCN-803 – updateTerrains: pro Dirty-Frame 4-MB-Höhenfeld-Kopie, Vektor-von-Vektoren und volle Chunk-View
- **ID**: SCN-803
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:504-535, 821-825; TerrainMeshGenerator.cpp:72-94 (computeTerrainHeightField)
- **Problem**: (a) Bei gesculptetem Terrain kopiert `computeTerrainHeightField` das gesamte `sculptHeights` (`heights = tc.sculptHeights`), bei 1025² sind das ~4 MB pro Brush-Frame, obwohl `field` nur gelesen wird (`const std::vector<float> field`). Dasselbe geschieht in `updateTessellation` (823). (b) `byCoord` ist ein `vector<vector<entity>>` mit `chunksPerSide²` (bis 1024) einzelnen Vektoren, die pro Dirty-Terrain und Frame allokiert werden, obwohl nur der erste Eintrag je Zelle benutzt wird (`byCoord[key].front()`). (c) Für jedes Dirty-Terrain läuft die komplette `view<TerrainChunkComponent>` (O(alle Chunks aller Terrains)). (d) Beim Löschen/Neubau werden alle LODs jedes Chunks generiert (`numLODs`× `generateTerrainChunkMesh`), das ist hier nötig, aber der Schritt (a) ist es nicht.
- **Code-Auszug**:
```cpp
const std::vector<float> field = computeTerrainHeightField(tc);
...
std::vector<std::vector<entt::entity>> byCoord(
    static_cast<size_t>(g.chunksPerSide) * g.chunksPerSide);
```
- **Lösung**: (a) Eine Variante `const std::vector<float>& heightFieldRef(const TerrainComponent&, std::vector<float>& scratch)` die bei `sculptHeights.size() == res*res` eine Referenz auf `tc.sculptHeights` liefert und nur sonst `scratch` füllt. Die Konsumenten nehmen schon `const std::vector<float>&`. Achtung: während `buildChunk` darf `tc.sculptHeights` nicht verändert werden (tut es nicht). (b) `std::vector<entt::entity> byCoord(n, entt::null)`; ein zweiter Treffer derselben Zelle (Duplikat) kommt in `toDestroy`. Das ändert das Verhalten nur bei Duplikaten, die bisher stehen blieben (Duplikat-Chunk wurde nie gelöscht); wenn das erhalten bleiben soll, Duplikate ignorieren statt löschen. (c) Chunks pro Terrain über die Hierarchie (`HierarchyComponent.children` des Terrains) statt View suchen.
- **Erwarteter Effekt**: Schätzung: (a) spart eine 4-MB-memcpy (~0,3–1 ms) pro Brush-Frame bei 1025²; (b) bis zu 1024 Allokationen weniger pro Frame.
- **Risiko / Verifikation**: (a) berührt TerrainMeshGenerator (anderes Modul/Slice). Tests mit Sculpt und Region-Rebuild laufen lassen (`grep -l "regionDirty" tests/`). Bei (b) das Duplikat-Verhalten explizit festlegen.

### SCN-804 – UISystem::extract: Layout jeden Frame, ancestors-Walks dreifach pro Element, mal Canvas-Anzahl
- **ID**: SCN-804
- **Kategorie**: D (kein Dirty-Flag), C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/UISystem.cpp:44-54, 87-128, 130-239; aufgerufen aus RenderExtractor.cpp:1586
- **Problem**: Pro aktivem Canvas wird die gesamte `view<UIElementComponent>` iterativ durchlaufen (161) und für jedes Element `owningCanvas` (Hierarchie-Walk mit `try_get` + `all_of` je Hop), `computeScreenRect` (rekursiv, pro Ebene drei `try_get/all_of`) und `nestingDepth` (nochmals ein Walk) aufgerufen. Das ergibt O(Canvases × Elemente × Tiefe) und bei der Rekursion für jedes Kind das Neuberechnen der kompletten Elternkette. Das Layout ist unabhängig von Element-Änderungen jeden Frame neu. `out` wird ohne `reserve` befüllt, danach `stable_sort` über alle Objekte. Der Text-Pfad erzeugt Glyph-Quads je Zeichen jeden Frame (`emitUITextGlyphs`, außerhalb des Slice, nicht gelesen).
- **Code-Auszug**:
```cpp
for (auto [elemEnt, elem] : reg.view<UIElementComponent>().each())
{
    const entt::entity owner = owningCanvas(reg, elemEnt);
    ...
    if (!computeScreenRect(reg, elemEnt, vpWidth, vpHeight, scaleX, scaleY, screenPos, screenSize))
        continue;
    const int sortLayer = HE::uiSortKey(elem.layer, nestingDepth(reg, elemEnt));
```
- **Lösung**: Stufe 1 (verhaltensgleich): Ein Durchlauf über die Elemente; pro Element einmal die Kette hochgehen und dabei Canvas, Tiefe und Rect gemeinsam bestimmen (z. B. eine `resolve(e)`-Funktion mit kleinem Cache `unordered_map<entity, Resolved>`/Vektor nach Entity-Index pro Aufruf, damit Kinder das Eltern-Rect wiederverwenden); der Canvas-Filter wird zur Zuordnung `owner → Canvas`, nicht zu einer Schleife über alle Canvases. Stufe 2: `out.reserve(letzte Größe)`. Stufe 3 (nur nach Messung): Layout-Cache mit Dirty-Flag auf `UIElementComponent` und Viewport-Größe.
- **Erwarteter Effekt**: Schätzung: Stufe 1 reduziert die Hierarchie-Zugriffe von ~3·Tiefe auf ~1·Tiefe pro Element und entfernt den Canvas-Faktor. Absolut nur spürbar bei UIs mit mehreren hundert Elementen.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe (stabile Sortierung nach `layer`) und das Fallback-Canvas-Verhalten (`owner == null` → erster aktiver Canvas, Element unter inaktivem Canvas zeichnet nirgends) müssen exakt erhalten bleiben. `tests/test_ui_*` bzw. Tests zu mehreren Canvases laufen lassen; ein Test mit zwei Canvases (Doppel-Geometrie-Regression, siehe Kommentar 135-144) sollte existieren.

### SCN-805 – UIInputSystem: pro Frame `unordered_set`, doppelte Hierarchie-Walks, Button-Schleife schreibt immer
- **ID**: SCN-805
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/UIInputSystem.cpp:38-95; UIInputSystem.h:25 (`std::unordered_set<uint32_t> hovered`)
- **Problem**: Höchstens ein Element ist „hovered“ (Kommentar Zeile 63), trotzdem ist der Zustand ein `std::unordered_set<uint32_t>`; pro Frame wird ein zweites Set `nowHovered` gebaut (Heap-Node bei `insert`), dann `std::move`. Für jedes Element mit Button/Script ruft die Schleife `computeScreenRect` (rekursiv) und, bei Treffer, `nestingDepth`. Das passiert in jedem Frame, auch wenn die Maus sich nicht bewegt und sich nichts geändert hat. Die Button-State-Schleife (85-91) läuft über alle Buttons und schreibt `btn.state` unbedingt.
- **Code-Auszug**:
```cpp
std::unordered_set<uint32_t> nowHovered;
if (top != 0) nowHovered.insert(top);
for (uint32_t e : state.hovered)
    if (!nowHovered.count(e))
        outEvents.push_back({ e, PointerEvent::Type::HoverExit });
```
- **Lösung**: `state.hovered` zu `uint32_t hovered = 0;` ändern (0 = keiner, `top == 0` benutzt dieselbe Konvention). Hover-Wechsel: `if (top != state.hovered) { if (state.hovered) push(Exit); if (top) push(Enter); }`. Den Button-Loop gleich behalten, aber nur schreiben, wenn sich der Wert ändert (damit etwaige Änderungs-Beobachter/Undo-Snapshots ruhig bleiben). Optional: die Hit-Test-Schleife nur ausführen, wenn sich Mausposition, Viewport oder Layout geändert haben (heikel, da Layout nicht dirty-markiert ist; deshalb nur nach Messung).
- **Erwarteter Effekt**: Schätzung: eine Heap-Allokation pro Frame weniger, deutlich einfachere Hover-Logik. Hit-Test bleibt O(Elemente × Tiefe).
- **Risiko / Verifikation**: `InputState` ist öffentlich (Header, im Editor/Game verwendet); `grep -rn "\.hovered" src/ tests/` auf Verwender prüfen. Reihenfolge der Events (Exit vor Enter) bleibt erhalten. UI-Input-Tests (`grep -l UIInputSystem tests/`) prüfen.

### SCN-806 – Wasser-Spline-Sync: jeden Tick Vektor mit UUID-Lookups aller Splines, Byte-FNV über Control-Points, 2× worldMatrixOf je Lake
- **ID**: SCN-806
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterLake.cpp:156-175, 835-883; aufgerufen aus TerrainSystem.cpp:479 (jeder `updateTerrains`-Aufruf, auch die vielen Editor-Aufrufe)
- **Problem**: Gibt es mindestens einen Spline-See, baut `syncSplines` jeden Aufruf `splines` (`vector<pair<UUID, Entity>>`, ohne `reserve`) mit `world.entityId(se)` für JEDE `SplineComponent` der Welt, obwohl nur wenige Splines Lakes sind. Danach wird je See `fingerprint()` gerechnet: zwei `worldMatrixOf` (Hierarchie-Walks) und ein FNV-1a, der BYTEweise über alle Control-Points läuft (`hashBytes`, 1 Multiplikation pro Byte; 12 B pro Punkt). Die lineare Suche nach der Spline über `splines` ist O(Seen × Splines).
- **Code-Auszug**:
```cpp
std::vector<std::pair<HE::UUID, Entity>> splines;
for (auto [se, sc] : reg.view<SplineComponent>().each())
    splines.emplace_back(world.entityId(se), se);
...
h = hashBytes(h, sc.controlPoints.data(), sc.controlPoints.size() * sizeof(glm::vec3));
```
- **Lösung**: (1) Splines nicht vorab sammeln, sondern pro See die Spline über `world`-UUID-Index auflösen, falls `HorizonWorld` einen UUID→Entity-Lookup hat (`grep -n "findEntity\|entityById" HorizonWorld.h`). Sonst `splines` nur mit Einträgen füllen, deren UUID in einer kleinen Menge der `sourceSpline`-Werte vorkommt (Menge aus den Bodies, typischerweise 1–5). (2) FNV auf 64-Bit-Wörter umstellen oder einen schnellen Hash (z. B. 8 Byte pro Schritt, oder `std::hash`-Kombination über Floats); der Hash ist runtime-only (`syncKey`, nicht serialisiert; `sanitize` setzt ihn auf 0), die Hash-Werte dürfen sich also ändern. (3) Die Transform-Matrizen nur dann abfragen, wenn der Spline oder seine Vorfahren dirty/neu propagiert sind (z. B. `TransformComponent` des Splines/Terrains hat `worldMatrix` aus der Propagation, `worldMatrixOf` ist die leichtere Variante des selben Ergebnisses).
- **Erwarteter Effekt**: Schätzung: ohne Seen null Kosten (früher Ausstieg bleibt). Mit einem See und einer großen Welt mit vielen Splines: von O(Splines) Lookups pro Tick auf O(Seen). Hash: ~8× weniger Multiplikationen.
- **Risiko / Verifikation**: Die Semantik „`syncKey == 0` → erster Blick nur merken“ nicht ändern. Tests zum Lake-Sync (`grep -ln "syncSplines\|lake::" tests/`) laufen lassen.

### SCN-807 – Wasseroberfläche: pro Tick Maps/Vektoren auch im Ruhezustand, Hash doppelt berechnet, volle Rasterscans
- **ID**: SCN-807
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterSurface.cpp:189-225, 227, 268-270, 301-348; WaterMesh.cpp:461-473, 495
- **Problem**: (a) Der Ruhezustand („nearly every tick“, Kommentar 214-216) wird erst NACH dem Aufbau von `byBody` (`unordered_map`, 189-195) erkannt, dazu kommen pro Tick `byTerrain` (`unordered_map`), `orphans`, `terrains` und `mine` (Vektoren, 324-345). (b) Beim Rebuild wird `surfaceHash(lattice, ...)` zweimal berechnet: in `Pass::terrain` (270) und erneut in `buildSurface(tc, lattice, ...)` (WaterMesh.cpp:495). Der Hash läuft byteweise FNV über `l.v` und `l.g` (Gitter groß, 1 Multiplikation pro Byte). (c) `allBodyCells(tc)` (227) scannt bei jedem nicht-ruhenden Tick das ganze Raster (`res²`, bis `kMaxRes`), obwohl `dirtyCells` bekannt ist; in einem Brush-Drag ist das jeden Frame der Fall.
- **Code-Auszug**:
```cpp
const uint64_t hash = surfaceHash(lattice, body.level, opt);   // WaterSurface.cpp:270
...
out.hash = surfaceHash(lattice, level, opt);                    // WaterMesh.cpp:495
```
- **Lösung**: (a) Ruhe-Check vor dem Sammeln: wenn `!f.dirty` und für alle `Body`s im Feld schon eine Surface mit passendem `paramsKey`/`level` existiert, früh raus; praktisch: Zähler/Revisions-Vergleich pro Terrain (`WaterSurfaceComponent` je Terrain + `f.revision`) und `WaterSurface::update` erst nach `view<WaterSurfaceComponent>` bei Bedarf. Die Scratch-Container (`byTerrain`, `terrains`) in einen `thread_local`/statischen Scratch auslagern, oder `smallvector`. (b) `buildSurface(...)` bekommt den vorberechneten Hash als Parameter (oder `out.hash` aus dem Aufrufer) – verhaltensgleich. (c) Extents pro Body cachen (z. B. im `Field` mit `revision` als Schlüssel; `WaterIndex` macht das für die Bounds schon) oder nur innerhalb von `dirtyCells ∪ built` neu bestimmen.
- **Erwarteter Effekt**: Schätzung: (a) 2–4 Heap-Allokationen weniger pro Frame; (b) halbiert die Hash-Kosten pro Rebuild (bei 512²-Lattice ~1 MB gehashte Bytes → grob 1–2 ms, **messen**); (c) bei 1024² ~1M Zellen weniger pro Frame beim Brushen.
- **Risiko / Verifikation**: (b) trivial verhaltensgleich. (c) Extent-Cache muss bei jeder Zellenänderung (Delta/Undo, `setResolution`, `decodeCells`) ungültig werden – `revision` wird bei allen diesen Pfaden erhöht (`markRect/markAll/++revision`), aber `sanitize`/`decodeCells` prüfen. Wasser-Tests (`tests/test_water*`) ausführen.

### SCN-808 – extractContours: node-basierte Maps für dicht indizierte Kanten
- **ID**: SCN-808
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/WaterMesh.cpp:279-373
- **Problem**: `points` und `next` sind `unordered_map<int64_t,…>` und `seen` ein `unordered_set<int64_t>`, obwohl die Schlüssel dicht sind (`(j*w+i)*2 + {0,1}` < `2*w*h`). Pro Crossing ein Heap-Node, danach wird `starts` aus der Map kopiert und sortiert („the same lake always gives the same mesh“). `points[cur]` im Ringaufbau nutzt `operator[]`. Das läuft bei jedem Wasser-Rebuild, bei Lake-Drags jeden Frame, und in `largestPiece`, `keepUnderSheet`/`shapeBank` (indirekt über `buildLattice`+`shoreSegments`).
- **Code-Auszug**:
```cpp
std::unordered_map<int64_t, glm::dvec2> points;
std::unordered_map<int64_t, int64_t>    next;
...
std::sort(starts.begin(), starts.end());
```
- **Lösung**: `std::vector<glm::dvec2> points(2*w*h)`, `std::vector<int32_t> next(2*w*h, -1)` (oder `int64_t`) und `std::vector<uint8_t> seen`. Die Startkanten ergeben sich beim aufsteigenden Index-Durchlauf `for key in 0..2*w*h: if next[key] >= 0`, das ist exakt die sortierte Reihenfolge von `starts`, der Sort entfällt. Speicher: `2*w*h*(16+4+1)` Byte, bei 1000×1000-Lattice ~42 MB, deshalb für große Lattices entweder auf die Bounding-Box der Konturen beschränken oder bei `w*h` über einer Schwelle bei der Map bleiben. **Messen**, welcher der beiden Wege gewinnt.
- **Erwarteter Effekt**: Schätzung: 3–10× schneller bei kleinen/mittleren Seen (kein Node-Alloc pro Crossing); bei sehr großen, dünnen Lattices ggf. mehr Speicher als die Maps.
- **Risiko / Verifikation**: Ergebnisreihenfolge (Ringe und Startpunkte) muss bitgleich bleiben: Golden-Tests im Wasserbereich (`grep -ln "extractContours" tests/`); der Schlüssel→Reihenfolge-Beweis oben gilt, weil `starts` genau die Schlüssel mit `next`-Eintrag aufsteigend sortiert.

### SCN-809 – reshape(): zwei Polygon-Rasterisierungen und zwei volle Raster-Scans pro Drag-Frame
- **ID**: SCN-809
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterLake.cpp:399-482; WaterField.cpp:534-609 (rasterizePolygon)
- **Problem**: Pro Frame, in dem ein Spline-Punkt gezogen wird, rasterisiert `reshape` das neue UND das alte Polygon (`newR`, `oldR`, 404-408; das alte Raster war im vorigen Frame als `newR` schon berechnet). Dazu `seen(n)` (res² Byte, zero-initialisiert) und eine Schleife über alle `n` Zellen nach `owner == id` (424-431), danach zwei volle Schleifen (472-474 nur über `before`, ok). `rasterizePolygon` prüft pro Zeile × 8 Subzeilen ALLE Polygonkanten (`for i in p.size()`), also O(Zeilen·8·Kanten) statt Active-Edge-Tabelle; für 256 Kantenpunkte und 500 Zeilen ~1 Mio. Kantentests je Raster, plus `std::sort` je Subzeile.
- **Code-Auszug**:
```cpp
const Raster oldR = b->polygon.empty() ? Raster{}
                                       : rasterizePolygon(b->polygon, tc.sizeX, tc.sizeZ, f.res);
...
std::vector<uint8_t> seen(n, 0);
for (size_t i = 0; i < n; ++i) { if (f.owner[i] != id) continue; ...
```
- **Lösung**: (1) Active-Edge-Liste in `rasterizePolygon` (Kanten nach min-y sortieren, pro Subzeile nur aktive Kanten); das Ergebnis `xs` bleibt identisch. (2) In `reshape` die Zellsuche auf die Bounding-Box von `oldR ∪ newR ∪ bodyCells` beschränken (bodyCells braucht selbst einen Scan; deshalb ggf. die Extents aus SCN-807 teilen) und `seen` über die Box statt über das ganze Raster führen. (3) Das alte Raster im `Body` (oder in `State` pro Lake) cachen; Speicher = Raster-Rechteck des Sees.
- **Erwarteter Effekt**: Schätzung: (1) 5–20× bei komplexen Polygonen; (2) spart die ~1M-Zellen-Schleife und den 1-MB-Alloc je Drag-Frame bei res 1024.
- **Risiko / Verifikation**: Rasterergebnis muss bit-identisch bleiben (Golden-Test für `rasterizePolygon` mit konkaven Polygonen/Löchern ergänzen, bevor (1) geändert wird). Die Fließkomma-Reihenfolge der `acc`-Addition ist pro Subzeile unverändert, solange `xs` sortiert bleibt.

### SCN-810 – Wasserpinsel: keepUnderSheet/shapeBank bauen Lattice und Distanzfelder pro Dab neu
- **ID**: SCN-810
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterBrush.cpp:57-93, 116-286, 333-381
- **Problem**: Pro Dab (Brush-Frame) mit Dig: `saveRegion` (Heap-Kopie), `keepUnderSheet` mit `while (moved)`-Schleife, die in jeder Iteration die Lattice inklusive `terrainHeightAt`-Sampling neu baut (`buildLattice` 77), dann `shapeBank` mit bis zu 3 Pässen, je Pass wieder `buildLattice`, `shoreSegments`, zwei Vektoren `nearest`/`best` (Heap) über die Box, zwei Sweeps. Die Zahl der `keepUnderSheet`-Iterationen ist nicht beschränkt (nur „ends because it only puts back“). Der Code ist korrekt dokumentiert, aber die Kosten wachsen mit Radius²·Iterationen.
- **Code-Auszug**:
```cpp
for (bool moved = true; moved; )
{
    moved = false;
    Lattice lattice;
    const bool sheet = cells.valid() && buildLattice(tc, body, cells, lattice, clip);
```
- **Lösung**: Zuerst **messen** (Profiler-Scope um `dab()`; Brush-Radius groß, Raster 1024). Wenn `keepUnderSheet` häufig >2 Iterationen braucht: nur die vom Zurücksetzen betroffenen Gitterecken der Lattice neu bewerten (`g`-Werte stehen über `terrainHeightAt`; ein zurückgesetzter Vertex ändert nur die 4 umliegenden Terrain-Zellen), statt alles neu zu bauen. Scratch-Vektoren (`nearest`, `best`) aus der Pass-Schleife heben und mit `assign` wiederverwenden.
- **Erwarteter Effekt**: Unsicher; ohne Messung kein Wert. Die Scratch-Wiederverwendung spart bis zu 6 Allokationen pro Dab.
- **Risiko / Verifikation**: Das Ergebnis der Iteration (Fixpunkt) darf sich nicht ändern. Die Brush-Tests des Wassers (`grep -ln "water::brush" tests/`) sind der Maßstab; ein Test mit bewusst mehreren Iterationen (steiler Hang) sichern.

### SCN-811 – Dreifach duplizierte Hilfsfunktionen im Wassercode
- **ID**: SCN-811
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: WaterBrush.cpp:17, 136-142; WaterField.cpp:14-17, 91; WaterLake.cpp:20-25, 94-102, 156-161; WaterMesh.cpp:18-21, 135-141, 151-157; WaterSurface.cpp:35-38, 46-52 (alle src/HE_Scene/src)
- **Problem**: `hasArea(tc)` existiert fünfmal (einmal als `terrainHasArea`), `finite(float)` zweimal, der FNV-1a-Hash dreimal (`hashBytes` in WaterLake, `fnv` in WaterMesh, `mix` in WaterSurface; identische Konstanten), die Punkt-zu-Segment-Distanz dreimal (`distance`-Lambda in shapeBank, `distToSegment` in WaterLake, `segmentDistance` in WaterMesh). `Touched` (WaterField.cpp:64) und `Changes` (WaterLake.cpp:45) sind dieselbe Struktur; der Kommentar bestätigt das.
- **Code-Auszug**:
```cpp
uint64_t hashBytes(uint64_t h, const void* data, size_t n) { ... h ^= p[i]; h *= 1099511628211ull; }  // WaterLake
uint64_t fnv(uint64_t h, const void* data, size_t bytes) { ... h ^= p[i]; h *= 1099511628211ull; }     // WaterMesh
uint64_t mix(uint64_t h, const void* data, size_t bytes) { ... h ^= p[i]; h *= 1099511628211ull; }     // WaterSurface
```
- **Lösung**: Ein interner Header `src/HE_Scene/src/WaterDetail.h` (nicht installiert, `namespace HE::water::detail`) mit `inline` `hasArea`, `finite`, `fnv1a`, `segmentDistance`, `CellBox` (= Touched/Changes). Ersetzen der Kopien. Gleichzeitig bietet sich an, den Hash dort einmal zu verbessern (SCN-806/807).
- **Erwarteter Effekt**: Weniger Code (~60 Zeilen), eine Stelle für Hash-Änderungen. Keine Laufzeitänderung.
- **Risiko / Verifikation**: Rein mechanisch. `hasArea` hat in `WaterBrush.cpp:17` eine schwächere Variante (nur `> 0`, ohne `isfinite`); beim Vereinheitlichen prüfen, ob das Verhalten für NaN/Inf-Größen relevant ist (dann bewusst die strengere Variante nehmen und Test ergänzen).

### SCN-812 – 64K-Tabellen auf dem Heap für Body-Ids (pruneEmptyBodies, ensureBounds, sanitize)
- **ID**: SCN-812
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: WaterField.cpp:438 (`std::vector<bool> used(65536, false)`), 973; WaterQuery.cpp:97 (`std::vector<uint8_t> used(65536, 0)`)
- **Problem**: `pruneEmptyBodies` wird nach jedem `removeCircle`/`removePolygon` mit Änderung aufgerufen, also pro Radier-Dab; es legt jedes Mal eine 8-KB-Bitmenge an und scannt danach das ganze Raster. `ensureBounds` legt bei jeder ungültigen Cache-Stelle 64 KB an und nullt sie. Die Id-Obergrenze ist durch `bodies.size()`/`nextBodyId` meist klein.
- **Code-Auszug**:
```cpp
std::vector<bool> used(65536, false);
if (f.allocated())
    for (size_t i = 0; i < f.owner.size(); ++i)
        if (f.coverage[i] > 0) used[f.owner[i]] = true;
```
- **Lösung**: `used` auf die Anzahl der Bodies dimensionieren (Slot-Tabelle wie in `allBodyCells`: `slot[id]`), oder `std::bitset<65536>` auf dem Stack (8 KB) anlegen, damit kein Heap-Alloc entsteht. Beim Radieren zusätzlich nur das Rechteck des Dabs scannen (Bodies, die der Dab nicht berührt hat, sind nicht neu leer geworden).
- **Erwarteter Effekt**: Schätzung: klein; einige µs pro Dab bis 1024². Nur Hot-Path-Hygiene.
- **Risiko / Verifikation**: Das „nur Rechteck scannen“-Argument stimmt nur, wenn vorher keine leeren Bodies existierten (Aufräumen alter leerer Bodies gehört bisher zu dieser Funktion). Deshalb zuerst nur den Container tauschen.

### SCN-813 – WaterIndex::refresh/WaterIndex::build: pro Aufruf Entry-Vektor, Matrixinverse, quadratische Cache-Suche
- **ID**: SCN-813
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterQuery.cpp:33-86; Aufrufer PhysicsWorld.cpp:3795 (jeder Wasser-Step), RenderExtractor.cpp:1202 (jeder Frame), EngineApi.cpp:1578
- **Problem**: Jeder Aufruf baut `std::vector<Entry> next` (Entry ~ 2×mat4 + mat3 + Cache, ~250 B) neu auf, berechnet `worldMatrixOf` und `glm::inverse(mat4)` je Wasser-Terrain, sucht den alten Eintrag linear (`for old in m_entries`, O(E²)) und sortiert. Pro Frame wird das im RenderExtractor mit einem frisch gebauten Index nochmals gemacht (nur für eine einzige Frage), zusätzlich zum Physik-Step.
- **Code-Auszug**:
```cpp
std::vector<Entry> next;
for (auto [entity, tc] : reg.view<TerrainComponent>().each())
{ ... e.localToWorld = HE::worldMatrixOf(world, entity); ... e.worldToLocal = glm::inverse(e.localToWorld);
```
- **Lösung**: `next` durch In-place-Update von `m_entries` ersetzen (Einträge anhand Entity wiederverwenden, Reihenfolge beibehalten, `reserve` auf alte Größe). Die Matrixinverse nur bei geänderter `localToWorld` neu berechnen (Bitvergleich mit dem alten Eintrag). Für den Underwater-Check einen eigenen leichten Pfad ohne vollständigen Index (nur Terrains mit Wasser, kein Sort, keine Cache-Felder). Wirkung ist klein bei 1–3 Terrains; wichtig wird das nur in Szenen mit sehr vielen Landscapes.
- **Erwarteter Effekt**: Unsicher / messen. Schätzung: <10 µs pro Frame bei 1 Terrain.
- **Risiko / Verifikation**: `Entry`-Cache-Schlüssel (`keyGrid` = Datenzeiger des Raster-Vektors) bleibt unverändert. Tests `test_underwater_camera.cpp`, Wasser-Physik.

### SCN-814 – WeatherAudio::update: String-Kopie und Bus-Lookup jeden Frame, mehrere unabhängige "erstes Weather-Component"-Suchen
- **ID**: SCN-814
- **Kategorie**: C, A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/WeatherAudio.cpp:30-33, 217, 227; WeatherSystem.cpp:83-85; SceneSystems.cpp:140
- **Problem**: `busFor` liefert pro Frame einen `std::string` (Kopie des Bus-Namens, bei längeren Namen Heap-Alloc) und macht einen `unordered_map<string,…>::count` (Hash über den Namen) auf dem Audio-Engine-Impl, obwohl sich `soundBus` selten ändert. `engine.isPlaying(handle)` wird je Layer (4×) und je Thunder-Stimme jeden Frame gerufen (Map-Lookup in `m_impl->sounds`; ob dies gegen den Audio-Thread synchronisiert wird, ist in AudioEngine.cpp zu prüfen, hier unsicher). Dasselbe Muster `for (auto [e, wc] : view<WeatherComponent>().each()) { w = &wc; break; }` steht in WeatherAudio, WeatherSystem und SceneSystems (3×).
- **Code-Auszug**:
```cpp
const std::string bus = busFor(engine, w->soundBus);
...
WeatherComponent* w = nullptr;
for (auto [e, wc] : reg.view<WeatherComponent>().each()) { w = &wc; break; }
```
- **Lösung**: `busFor` zu `const std::string& busFor(...)` mit Rückgabe von `wanted` oder einer `static const std::string kNone` machen (keine Kopie). Bus-Existenz nur prüfen, wenn der Voice-Start ansteht (`bed.handle == 0` oder neue Thunder-Stimme), nicht jeden Frame. Ein Helfer `WeatherComponent* firstWeather(entt::registry&)` (z. B. in WeatherSystem.h) ersetzt die drei Schleifen; er kann `view.front()`/`view.begin()` nutzen.
- **Erwarteter Effekt**: Schätzung: klein (eine String-Kopie und ein Hash-Lookup pro Frame). Vereinfachung ist der Hauptnutzen.
- **Risiko / Verifikation**: Referenz auf `static const std::string` ist thread-sicher ab C++11. `hasBus` darf nicht vor dem ersten Voice-Start zwischengespeichert werden, sonst bleibt ein später erzeugter Bus unbeachtet; deshalb nur bei Start prüfen, nicht cachen.

### SCN-815 – Terrain-Diagnose-Logging im Rebuild-Pfad und globale Welt-Zeiger-Maps
- **ID**: SCN-815
- **Kategorie**: F, A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: TerrainSystem.cpp:126-127, 157, 203-204, 591-615; WaterSurface.cpp:33; TransformHierarchy.cpp:131-140
- **Problem**: (a) Der „Diagnostic“-Block läuft bei jedem `rebuildAll` (also bei jedem vollen Rebuild) und sonst im 1-s-Takt: `std::minmax_element` über das gesamte Höhenfeld (bis 1M Floats) nur für eine INFO-Zeile, die per `std::to_string`-Kette gebaut wird. Außerdem ist `s_lastLog` eine einzige Funktions-Static für alle Terrains. Er ist als Fehlersuch-Diagnose gedacht (Kommentar „A landscape that stays flat after a sculpt shows up here“). (b) Vier Datei-Statics (`g_pendingTerrainColliders`, `g_tessMeshes`, `g_displacementGrey`, `g_owned`) sind nach `const HorizonWorld*` geschlüsselt. Dagegen speichert `TransformHierarchy.cpp` seinen Zustand im `registry.ctx()` (`stateOf`), so verschwindet er mit der Welt. `g_displacementGrey` wächst ohne Räumung mit jeder genutzten Displacement-Textur (Greymap jeder Textur bleibt im Speicher).
- **Code-Auszug**:
```cpp
static auto s_lastLog = std::chrono::steady_clock::time_point{};
...
const auto mm = std::minmax_element(field.begin(), field.end());
```
- **Lösung**: (a) Den Diagnoseblock hinter `HE_LOG_ENABLED(INFO)`/`HE_LOG_DEBUG` legen oder auf DEBUG senken und das Min/Max nur dann berechnen, wenn der Log-Level aktiv ist; `s_lastLog` pro Terrain führen oder entfernen. (b) Die Maps in einen `struct TerrainState` im `reg.ctx()` überführen (wie `TransformHierarchy`); damit entfallen Welt-Zeiger-Schlüssel und das Aufräumen bei Weltende. `g_displacementGrey` an das ContentManager-Ereignis „Asset entladen“ koppeln oder auf wenige Einträge begrenzen.
- **Erwarteter Effekt**: Schätzung: spart eine Feldüberlauf-Schleife (~1M Vergleiche) pro vollem Rebuild; (b) beseitigt die Gefahr, dass eine neue Welt an derselben Adresse alte Einträge erbt.
- **Risiko / Verifikation**: Log-Text ist von Tests/MCP-Diagnose evtl. erwartet (`grep -rn "TerrainSystem: rebuilt" src tests docs`). (b) benötigt Test für „zweite Welt“ (Kommentar 115-117 beschreibt die Anforderung bereits).

---

## Cross-Modul-Hinweise
- `src/HE_Scene/src/TerrainMeshGenerator.cpp:72-94` (`computeTerrainHeightField`): kopiert bei Sculpt-Daten das ganze `sculptHeights`; siehe SCN-803.
- `src/HE_Scene/src/SceneSystems.cpp:113,131,140,150`: Reihenfolge der Systeme; `updateTessellation` (SCN-801) läuft jeden Tick; dort steht auch die dritte „erstes WeatherComponent“-Suche (SCN-814).
- `src/HE_Rendering/src/RenderExtractor.cpp:1202`: `WaterIndex::build` pro Frame für den Underwater-Check (SCN-813); `:1586` UISystem::extract pro Frame (SCN-804).
- `src/HE_Scene/include/HorizonScene/UIInputSystem.h:25`: `std::unordered_set<uint32_t> hovered` (SCN-805).
- `src/HE_Scene/src/PhysicsWorld.cpp:3795`: `waterIndex.refresh(world)` in jedem Wasser-Step (SCN-813).
- `src/HE_Editor/EditorApplication.cpp` und `TerrainTools.cpp`: dutzende direkte `updateTerrains`-Aufrufe (u. a. 3897, 6480, 6812, 7214 …); jede Verbesserung des Leerlaufpfads (SCN-802, SCN-807) wirkt dort mehrfach pro Frame.
- `src/HE_Scene/src/CameraRigController.cpp:410,581,645`, `NavigationSystem.cpp:115`: weitere `propagateTransforms`-Aufrufe pro Tick; jeder davon macht den vollen Scan (siehe Unsicher).

## Unsicher / Messung nötig
- `propagateTransforms` (TransformHierarchy.cpp:168-185): der Scan läuft pro Aufruf über ALLE `TransformComponent`s (memcmp von 36 Byte, laut Kommentar 50–200k Komponenten gemessen). Ein Schreib-Hook (Setter/Dirty-Bit) würde das ersparen, ist aber invasiv; Autoren haben das bewusst als Trade-off dokumentiert. Zu messen: Anzahl der `propagateTransforms`-Aufrufe pro Frame (CameraRig 3 Stellen, Navigation, Tick) und der Anteil im Profil `Transforms::scan`. Die Komponente trägt zusätzlich `localCache` (64 B) und den Schlüssel (36 B) neben `worldMatrix`; eine Trennung in eigene Speicher-Pools könnte den Scan cache-freundlicher machen (SoA), ist aber ohne Messung nicht zu empfehlen.
- `propagateFrom` (TransformHierarchy.cpp:68-82) ist rekursiv; Tiefe bis zu 65 536 Ketten sind in `recomputeFlagged` als Grenze behandelt, hier aber nicht (Stack-Overflow bei absurd tiefer Hierarchie). Niedrige Priorität.
- `WeatherSystem` Partikel-Pfad (WeatherSystem.cpp:303-330): O(maxParticles) pro Frame mit `sampleGround` je Partikel; die Obergrenze (`maxRainParticles`) und die GPU-Alternative (`gpuPrecip`) entscheiden; ohne Messung kein Fund.
- `AudioEngine::isPlaying` (AudioEngine.cpp:1086): ob ein Lock/atomarer Zugriff gegen den Audio-Thread nötig ist, nicht geprüft (SCN-814).
- Kosten von `emitUITextGlyphs` (Renderer/UIFont, nicht im Slice) für SCN-804.
- Tatsächliche Iterationszahl von `keepUnderSheet` (SCN-810) und tatsächliche Kosten von `allBodyCells`/`surfaceHash` bei res 1024–4096 (SCN-807).

## Gelesen
Alle 14 Dateien des Slices (TerrainSystem, TransformHierarchy, UIInputSystem, UISystem, WaterBrush, WaterField, WaterLake, WaterMesh, WaterQuery, WaterRipples, WaterSurface, WaterTriangulate, WeatherAudio, WeatherSystem) vollständig gelesen: ja. WaterRipples.cpp und WaterTriangulate.cpp (Earcut-Portierung mit `std::deque`-Pool, einmalig pro Rebuild) ergaben keine Funde. Zusätzlich gezielt per Grep/Teilausschnitt geprüft: SceneSystems.cpp (Aufrufreihenfolge), RenderExtractor.cpp:1190-1215, PhysicsWorld.cpp:3785-3800, AudioEngine.cpp (hasBus/isPlaying), UIInputSystem.h (hovered), TerrainMeshGenerator.cpp (computeTerrainHeightField), WeatherComponent.h (kGrid), TerrainComponent.h (tessellationFactor).
