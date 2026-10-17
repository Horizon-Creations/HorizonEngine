# Instanced Foliage und LOD/Impostor: Bestandsaufnahme und Bauplan (Thema 163, Schritt 1)

Stand 10.10.2026, Zweig `claude/instanced-foliage-und-lod-impostor-fuer-b-ume-gras-felsen`,
Basis `202e14f2` (= `origin/release/0.7.0`). **Nur Doku, kein Produktcode, nichts gebaut, nichts
gelaufen.** Einzige Messung: `sizeof(RenderObject)` per Mini-Compile (280 Byte). Zeilennummern gelten
für diesen Commit. Was ich selbst im Code gegengelesen habe und was nur aus Teilberichten stammt,
steht in Abschnitt 10.

Bezug: `docs/terrain-vegetation-gap-audit-2026-09-26.md` (Thema 80), `docs/gpu-instancing-cross-backend-plan.md`
(06.09.), `docs/perf-audit/step3-cpu-memory-deep-dive-2026-09-27.md` §5, `docs/world-streaming-baseline-2026-10-06.md`,
`docs/entity-cell-streaming-plan-2026-10-10.md` (Thema 164, liegt nur auf
`origin/claude/entities-pro-zelle-streamen-nur-die-umgebung-der-kamera-exis`) und
`docs/render-extractor-shadow-pass-plan.md` (Thema 162, liegt nur auf
`origin/claude/render-extractor-und-schatten-pass-einmal-pro-frame-statt-me`). Die beiden letzten werden hier
zitiert, nicht übernommen.

## 0. Kurz

**Die Annahme im Thema stimmt nur halb.** Dort steht, nicht jede Pflanze dürfe ein Entity sein. Das ist sie
auch heute nicht: Gras, Büsche und Bäume aus dem Landscape-Foliage-Pinsel sind **Daten** in einer
`FoliageComponent` am Terrain (`cachedInstances`, 64 Byte je Instanz, nicht gespeichert, aus Seed und
Maske neu erzeugt). Es gibt Pinsel, Dichtemaske, Wind-Knoten und zwölf Tests. Das Problem liegt **auf
der Renderseite**: `extractFoliage` macht aus jeder Instanz ein volles 280-Byte-`RenderObject`, jedes
Backend refined, culled und sortiert diese Objekte pro Pass neu, und danach werden sie wieder zu
`instanceTransforms` zusammengeklebt. Die Kosten wachsen mit Instanzen mal Pässen, nicht mit Entities.

Fünf Befunde, die den Zuschnitt bestimmen:

1. **Instancing und Wind schließen sich heute aus (Abschnitt 3).** Die mitgelieferte `Foliage`-Vorlage ist
   ein Graph-Material (Wind Sway auf dem WPO-Pin, Masked). Metal schließt Graph-Materialien vom Batch aus
   (Schleife, ein Draw je Instanz), D3D11 schleift mit zwei Map/Unmap je Instanz, D3D12 und Vulkan
   schleifen und **überspringen alles ab Instanz 1024 je Frame**, GL instanziert zwar, **verliert aber das
   Material** (flaches PBR, kein Wind, kein Alpha-Discard). Der Roadmap-Satz „GPU-instanced foliage with wind“
   stimmt auf keinem Backend. Felsen und Bäume mit eingebautem PBR-Material laufen dagegen schon über den
   Instanzpfad.
2. **Eine harte Wand bei 65 536.** `k_maxInstances = 65536` steht in allen vier Backends mit Instanz-Ring. Bei
   Metal und D3D11 gilt sie je Batch (darüber: Schleife, still), bei D3D12 und Vulkan **kumulativ je Frame über
   alle Pässe** (Szene, G-Buffer, jede Kaskade, jeder lokale Schatten-Layer, SSAO, GI). D3D12s Ersatzschleife
   bricht bei `k_maxDraws = 16384` still ab. Ein größerer Ring ist nicht die Antwort; die Antwort ist, dass
   **weniger Instanzen je Pass ankommen** (Abschnitt 7.5).
3. **Kein räumlicher Aufbau, keine LOD-Stufen, keine Impostore.** Foliage hat ein hartes Ende bei
   `drawDistance` (XZ-Kreis), keine Mesh-LODs, keinen Simplifier irgendwo in der Engine, keinen
   Billboard-Modus in einem Mesh-Vertexshader. `LODSystem` kennt weder Hysterese noch Überblendung.
4. **Das Datenmodell trägt keine zweite Schicht.** Eine `FoliageComponent` je Terrain-Entity (entt: eine
   Komponente je Typ), und sie muss auf derselben Entity wie das Terrain liegen. Gras *und* Bäume gehen nur über
   zwei Terrains, und der Landscape-Pinsel greift immer das erste. Thema 180 (Auto-Landscape 2.0, Zweig
   0.8.0) will Meshes je Landschaftsschicht „über den vorhandenen Foliage-Pfad“ und bekommt ohne Schichten
   keinen Platz.
5. **Offene Bugs im Scatter**, noch auf diesem Stand (Abschnitt 1.4): Sculpt-Striche streuen nicht neu,
   die Instanzen sind in Weltkoordinaten eingebacken und ignorieren Drehung, Skalierung und Eltern des Terrains,
   und Floating Origin kennt sie nicht.

Entscheidungen dieses Plans:

| # | Entscheidung | Abschnitt |
|---|---|---|
| D1 | **Zwei Gleise.** Scatter (Gras, Büsche, Steinchen, Waldfläche) bleibt **Terrain-Layer-Instanzen ohne Entity pro Pflanze**; das Eigentum ändert sich nicht, der Datenweg schon. Handplatziertes (Dörfer, Gebäude, einzelne Felsen und Bäume) bleibt **Entity** und fährt auf den Zellen aus Thema 164 und `LODComponent`. Eine ISM-artige Komponente ist eine spätere Erweiterung, für 0.7.0 nicht nötig | 6.1 |
| D2 | **Schichten im Component:** `FoliageComponent` bekommt `layers`, alte Szenen speichern byte-gleich (nur eine Schicht = altes Format). Keine Kind-Entities, keine neue Komponente (spart die sechs Registrierstellen) | 6.2 |
| D3 | **Renderseite: Cluster-Objekte statt Objekt je Instanz.** Pro Terrain, Schicht, LOD-Stufe und Bucket (Raster 32 m) **ein** `RenderObject` mit gültigen Bounds, das erst in den zwei gemeinsamen Vorderseiten-Funktionen (`GeometryPass::execute`, `RenderSorter::batchDepthRuns`) zu Instanzen aufgeklappt wird. Backends bekommen weiter `DrawCall` und `DepthBatchList` | 6.3 |
| D4 | **Instanzen terrain-lokal speichern**, beim Aufklappen mit der Weltmatrix des Terrains verrechnen. Das beseitigt drei Bugs auf einmal (Weltmatrix, Verschieben, Floating Origin) | 6.4 |
| D5 | **Das Instanz-Budget kommt aus LOD, Schattenreichweite und GI-Ausschluss**, nicht aus größeren Ringen | 7.5 |
| D6 | **Graph-Material-Instancing ist der eigentliche Hebel für Wind und Cutout** und bekommt einen eigenen Schritt: Metal und GL zuerst (hier prüfbar), D3D/Vulkan auf dem Windows-Gleis | 3, 8 |
| D7 | **LOD für 0.7.0 = handgemachte Mesh-Stufen je Schicht + Dichte-Ausdünnung + statische Kreuz-Karten als letzte Stufe.** Oktaeder-Impostore, Überblendung per Dither und Auto-LOD beim Import sind eigene, spätere Schritte | 7.6 |
| D8 | **Vertrag mit 162/164/180** in Abschnitt 5: Foliage bleibt Resident und liegt ganz in der Basis; Cluster-Ausgabe lebt in einer eigenen Datei und ist O(Cluster); Thema 180 baut **keinen** zweiten Pfad | 5 |

**Vor der Deadline (heute 23:00) machbar** (Abschnitt 8): Messzeug und Basismessung, der Cluster-Pfad in der
Vorderseite samt Tests und Metal-Zeuge, der GL-Gate-Fix und die zwei kleinen Scatter-Bugs. Graph-Material-Instancing,
LOD-Stufen und alles auf D3D/Vulkan sind nicht heute; Impostor-Baking ist 0.8.0. Das ist eine ehrliche Schätzung,
keine Zusage.

## 1. Wie Vegetation heute entsteht

### 1.1 Drei Wege

| Weg | Wie | Was jedes Stück kostet | Grenze |
|---|---|---|---|
| **E1 Entity pro Objekt** | Mesh+Material(+`LODComponent`) auf einer Entity; per Duplizieren (`duplicateSelectedEntity`), Prefab (`prefab_instantiate`) oder Skript (`spawn`). Eine Instanz-Komponente (ISM/HISM/MultiMesh) **gibt es nicht** | Entity, `propagateTransforms`, 280-Byte-`RenderObject`, Outliner, Undo-Snapshot, Collab-Blob | Skaliert mit der Entity-Zahl. Thema 153 §11.3: 50k Entities kosten nach allen Umstellungen noch 62 ms CPU je Frame, `extract` davon 33 ms. Zellen (164) senken N, ändern nichts am Preis je Entity |
| **E2 Scatter am Terrain** | `FoliageComponent` auf der Terrain-Entity; `FoliageSystem` streut, Pinsel malt die Dichtemaske | Kein Entity, 64 Byte im `cachedInstances`. Danach zahlt jede Instanz die Kette aus 1.2 | Eine Sorte je Terrain, keine LOD, kein Bucket, harte Kante bei `drawDistance` |
| **E3 Das Terrain selbst** | Chunk-Kind-Entities (`TerrainChunk`) mit `MeshComponent`, `LODComponent` (bis 4 Stufen, 65/33/17/9 Vertices) und `MaterialComponent`, nie serialisiert, im Outliner versteckt (`TerrainSystem.cpp:296, 314-323, 546-563`) | Höchstens 16×16 = 256 Chunks | Auflösung bis `kTerrainMaxResolution = 1025`, kein Terrain-Streaming |

Die Gleis-Frage aus dem Thema ist damit schon halb beantwortet: E2 ist, was die Queen „Terrain-Layer-Instanzen
ohne Entity pro Pflanze“ nennt, und es gibt es. Offen ist, ob es **so bleibt** und wie die Renderseite damit
umgeht (Abschnitt 6).

### 1.2 Der Weg einer Foliage-Instanz, Stufe für Stufe

| # | Stufe | Stelle | Was passiert | Je Instanz |
|---|---|---|---|---|
| 1 | Authoring | `TerrainTools.cpp` (Foliage-Tab), `InspectorPanel.cpp:3390-3441` | Pinsel malt `densityMask` (Grow/Erase, Ctrl kehrt um), Inspector setzt Mesh, Material, Dichte, Distanz, Skala, Seed, „Regenerate“ | – |
| 2 | Streuen | `FoliageSystem::update`, aus `SceneSystems::tickWorld` (`SceneSystems.cpp:143`) | Nur bei `dirty`, **synchron, ganze Landschaft**: `count = Fläche × density`, je Kandidat Zufallsposition, Rejection gegen die Maske, Höhe aus `terrainHeightAt` (`FoliageSystem.cpp:59-116`). Warnung ab 200 000, **kein Limit** (`:70`) | O(count) einmal |
| 3 | Speicher | `FoliageComponent::cachedInstances` | `std::vector<glm::mat4>` in **Weltkoordinaten**, Ursprung ist `tf->position` des Terrains, ohne Drehung, Skala, Eltern (`FoliageSystem.cpp:51-54, 111-115`). Nicht serialisiert; Laden setzt `dirty` (`SceneSerializer.cpp:1757-1781`) | 64 Byte |
| 4 | Extrahieren | `extractFoliage` (`RenderExtractor.cpp:621-651`, aufgerufen `:1325`) | **Seriell, ohne `reserve`**, läuft über *alle* `cachedInstances`, nicht nur die in Reichweite; XZ-Abstandstest; je Treffer ein `RenderObject` (`castsShadow`, `contributesAO` bleiben `true`, `lod = 0`, **Bounds absichtlich ungültig**) | **280 Byte** (`sizeof` gemessen) |
| 5 | Pro Pass | alle Backends | Bounds aus dem aufgelösten Mesh nachziehen (ein `resolveMesh`-Hash-Lookup je Objekt, z. B. `D3D11Renderer.cpp:6194-6199`, `OpenGLRenderer.cpp:10651-10653`), `FrustumCuller::cull` (parallel, linear), `RenderSorter::sort` (`std::sort` nach Mesh, dann Abstand), das **je Schatten-Kaskade und je lokalem Layer erneut** (`D3D11:6603-6605`, `MetalRenderer.mm:7781-7803` u. a.) | Lookup + Sortierschlüssel |
| 6 | Batchen | `GeometryPass::execute` (`RenderPass.cpp:14-126`) | Läufe gleicher Mesh+Material, `instanceTransforms.push_back(transform)` | 64 Byte Kopie |
| 7 | Zeichnen | Backend | `{mvp, model}` je Instanz auf der CPU rechnen (`viewProj * t`), 128 Byte schreiben, **je Pass** | 128 Byte |

Zusätzlich, wenn GI an ist: jedes Foliage-Objekt landet in der GI-Instanzliste (Filter dort nur `castsShadow`,
ohne Bounds- oder Distanztest, z. B. `D3D11Renderer.cpp:3159`), mit einem `glm::inverse` je Instanz; die
Software-Kerne schleifen je Strahl über alle Instanzen („TLAS analogue: linear instance loop“,
`shaders/gi_shadow.comp`, `gi_probe.comp`). GI und GI-Reflexionen sind per Default aus.

### 1.3 Speicherung und Zusammenarbeit

- Gespeichert werden Parameter und die Maske (`densityMaskB64`, 128² Byte Standard), **nicht** die Instanzen
  (`SceneSerializer.cpp:851-871`). Eine Szene mit 1 Mio. Instanzen ist deshalb klein; die Instanzen sind
  aber auch **nicht adressierbar** (keine UUID, nichts hängt an einer einzelnen Pflanze).
- Undo-Snapshots serialisieren die ganze Welt als CBOR, Collab schickt den Komponenten-Blob der Entity bei
  jeder Änderung (`EditorUndo.h`, `CollabSession.h`). Würde man Instanzen je Stück speichern, würde jeder Pinselstrich
  diese Blobs wachsen lassen (100k × 64 Byte = 6,4 MB roh, ca. 8,5 MB Base64, Rechnung). Das ist der Grund,
  Instanzen **abgeleitet** zu lassen (Abschnitt 6.5).
- `CellSplit.cpp:25-31` (`movableKey`): `foliage` und `terrain` stehen nicht in der Liste, ein Terrain mit Foliage
  bleibt in der Basis. Das passt zu Thema 164, das Terrain Resident hält und Foliage ausdrücklich Thema 163 überlässt
  (`entity-cell-streaming-plan` §0, §9).

### 1.4 Lücken im Scatter (alle auf `202e14f2` noch offen)

| # | Lücke | Beleg |
|---|---|---|
| F1 | **(behoben in Teil 2b, Abschnitt 12.2)** **Sculpt-Striche streuen nicht neu.** Der Pinselpfad (Raise, Lower, Smooth, Flatten, Ramp, Roughen) setzt nur `regionDirty`; „Reset Sculpting“ und die Änderung von Größe, Auflösung oder Seed im Inspector ebenso nicht. Nur Mountain (`TerrainTools.cpp:701-704`) und Heightmap-Import setzen `fol->dirty` | Audit §3.1/1 stimmte; seit dem Audit ist nur Mountain dazugekommen |
| F2 | **Weltmatrix wird ignoriert:** nur `tf->position`, keine Drehung, Skala, kein Eltern. Terrain-Chunks rendern mit voller Weltmatrix, die Foliage liegt daneben | `FoliageSystem.cpp:51-54` |
| F3 | **Verschieben setzt nichts dirty.** Nichts beobachtet das Transform | – |
| F4 | **Floating Origin** (Projektschalter, Default aus) schiebt Partikel, Trails, Nav-Agenten, Jolt, aber nicht `cachedInstances` | `FloatingOrigin.h:19-28` |
| F5 | **Eine Schicht je Terrain**, und sie muss auf der Terrain-Entity liegen. „Add Component Foliage“ auf einer anderen Entity ist erlaubt und streut still nichts (das Tutorial-Ziel „Foliage“ wird dabei als erfüllt gewertet) | `FoliageSystem.cpp:37`, `InspectorPanel.cpp:3654-3680`, `TutorialSteps.cpp:574-581` |
| F6 | **Der Landscape-Pinsel greift immer das erste Terrain** (`tvw.front()`), das Zwei-Terrain-Notbehelf-Gleis ist im Editor halb kaputt | `TerrainTools.cpp:337, 1146` |
| F7 | **Kein Limit, kein Streaming:** synchrones Streuen der ganzen Landschaft, nur eine Warnung ab 200 000 | `FoliageSystem.cpp:70`, `world-streaming-baseline` §4.4 |
| F8 | **Keine Filterregeln** (Hang, Höhe, Landschafts-Paint-Schicht, Mindestabstand, Normalenausrichtung), kein Kollider | Audit §3.4 |
| F9 | **Kein Foliage-Skript- und MCP-Zugriff:** nur `entity_set_components` mit dem Schlüssel `foliage` und `setVisible` | `McpToolsEntity.cpp:645`, `EntityVisibility.cpp:23, 36` |
| F10 | **Kein Wasser-Ausschluss** (Thema 174 liegt noch nicht auf `release/0.7.0`; ausgehobene Becken verlängern F1) | Teilbericht Editor |

Tests: `tests/test_foliage.cpp` (12 Fälle: Anzahl, Determinismus, Höhe, Serialisierung, Maske) und
`tests/test_terrain_tools_ui.cpp:260`. **Nicht getestet:** der Extractor-Teil (`extractFoliage` hat keinen
Test), Terrain mit Versatz, Drehung oder Skalierung, Sculpt-Dirty, Verschieben.

## 2. Renderer-Bestand

### 2.1 Gemeinsame Vorderseite

- `GeometryPass::execute` (`RenderPass.cpp:14-126`) bricht einen Lauf bei geändertem Mesh, Material, `paramOverride`,
  `instanceTint`, `receivesShadow`, `weightmapTextureId` oder `sections` (`:35-57`). **Keine Instanzobergrenze, keine
  Aufteilung**; ein Lauf mit 200 000 Objekten wird ein `DrawCall`.
- `RenderSorter::sort` (`RenderSorter.cpp`) sortiert nach Mesh-UUID, dann Abstand; **nicht nach Material**. Zwei
  Materialien auf einem Mesh verschränken sich und brechen Läufe.
- `RenderSorter::batchDepthRuns`/`batchDepthCasters` (Schatten, SSAO, GI-Vorpass) bilden Läufe nach `meshAssetId`
  allein und sind der gemeinsame Einstieg für die Tiefenpässe aller fünf Backends (`GL:10765`, `MTL:7803`, `D11:6605`,
  `D12:10405`, `VK:6670`). Der Aufrufer culled vorher mit der `viewProj` des Layers (`m_culler.cull(world, viewProj, …)`).
- `ShadowPass::execute` (`RenderPass.cpp:188-208`) baut je Caster einen unbatchten `DrawCall`. GL, D3D11 und D3D12 scheinen
  diese Draws nicht zu lesen (sie culled und sortieren je Layer selbst); das wäre tote O(N)-Arbeit. **Nicht gegengeprüft**, beruht
  auf der Abwesenheit des Tokens `cmds` in drei Backend-Abschnitten.
- `FrustumCuller::cull` (`FrustumCuller.cpp:48-66`) ist ein paralleler Linearlauf; **ungültige Bounds gelten als sichtbar**.
  Es gibt keine Rasterstruktur, kein BVH. Der Occlusion-Culler ist per Default aus und nur in GL und Metal verdrahtet.
- Jedes Backend zieht vor dem Culling die Bounds aus dem aufgelösten Mesh nach und **überschreibt** damit die des Extractors
  (siehe 1.2 Stufe 5). Für Cluster-Objekte ist das eine Falle (Abschnitt 6.3).

### 2.2 Instancing je Backend (opake Batches mit eingebautem PBR)

| Backend | Instanzdaten, Upload | Obergrenze und Überlauf |
|---|---|---|
| OpenGL | 64 Byte (roh `mat4`), `glBufferData(GL_STREAM_DRAW)` je Batch und Pass | **keine**; 100k laufen als ein Draw |
| Metal | 128 Byte `{mvp, model}`, Scheibe aus dem `FrameUploadRing` (`FrameUploadRing.h`; die Beschreibung „neuer MTLBuffer je Batch“ im Altplan §M3 ist überholt) | 65 536 **je Batch** (`MetalRenderer.mm:194, 14316-14320`); darüber **still** die Schleife |
| D3D11 | 128 Byte, ein 8-MiB-StructuredBuffer, `Map(WRITE_DISCARD)` je Batch | 65 536 je Batch (`D3D11Renderer.cpp:1335, 7233-7236`); darüber `uploadObject` + `DrawIndexed` je Instanz |
| D3D12 | 128 Byte, Upload-Ring mit 65 536 Slots je Frame in Flight | 65 536 **kumulativ je Frame** über alle Pässe (`D3D12Renderer.cpp:96, 3179, 11013`); darüber `drawOne` mit `k_maxDraws = 16384` (`:94, 10984`), das **still aufhört** |
| Vulkan | 128 Byte, host-coherent, 2 × 8 MiB | 65 536 **kumulativ je Frame** (`VulkanRenderer.h:363`, `.cpp:6537, 8325`); darüber `drawOne` ohne Draw-Grenze (langsam, vollständig) |

Transparente Batches werden nirgends instanziert (Absicht, sie müssen nach Tiefe sortiert werden). **Masked ist nicht
transparent** und läuft im opaken Pass, ist aber ein Graph-Material (3).

Schatten und Tiefen-Vorpässe (SSAO, GI) laufen auf allen fünf Backends instanziert; der Altplan §6.1/6.2, der
„D3D11/D3D12 offen“ sagt, ist überholt (Commit `0fe79036`). A/B-Schalter: `HE_DEPTH_INSTANCING=0`,
Metal `HE_MTL_INSTANCING=0`.

### 2.3 Was es nirgends gibt

- **Indirekte Zeichenbefehle und GPU-Culling:** in keinem der fünf Backends (`ExecuteIndirect`, `DrawIndirect`,
  `MultiDraw`, `MTLIndirectCommandBuffer` nicht gefunden). Compute gibt es nur für GI und Metal-Partikel; macOS-OpenGL ist auf
  4.1 festgelegt, also ohne Compute. GPU-getriebenes Rendering ist damit **kein gemeinsamer Nenner**; der Weg über alle
  Backends ist CPU-Culling auf Bucket-Ebene.
- **Alpha-Test in den Tiefenpässen:** die Schatten- und Tiefen-Vorpässe haben keine Fragmentstufe (`fragmentFunction = nil`,
  `stageCount = 1`, Pixelshader null, leeres `kDepthFS`). Blattkarten werfen volle Rechteckschatten. Die eingebauten Szene- und
  G-Buffer-Shader haben kein `discard`; nur Graph-Materialien mit `Masked` verwerfen (`MaterialGraph.cpp:1825-1827`).
- **WPO in den Tiefenpässen:** alle Tiefen-Vertexshader nehmen nur die Position. Ein wehender Baum wirft einen starren Schatten.
- **Ein instanzierter Graph-Material-Vertex:** `MaterialShaderLibrary::customVertex` (`:914`) hat keine instanzierte Form,
  nur `reflPrepassVertexInstanced`, und der ist kein WPO.
- **Billboard- oder Impostor-Modus im Mesh-Pfad:** nicht gefunden. Es gibt nur Partikel- und Wetter-Billboards (CPU-berechnete
  Matrizen als gewöhnliche `RenderObject`s, `RenderExtractor.cpp:513-610`) und das Standard-Quad `kDefaultQuadMeshId`.

### 2.4 Kostenkette bei 100 000 sichtbaren Instanzen (Rechnung, nicht gemessen)

| Stufe | Rechnung |
|---|---|
| `extractFoliage` | 100 000 × 280 Byte = 28 MB Schreibzugriff je Extract, seriell, ohne `reserve` (Wachstumskopien kommen dazu). Metal: 4 Extracts je Frame, 2 davon Tiefenkopien der ganzen `RenderWorld`; Vulkan: 5 Extracts ohne Wiederverwendung |
| Pro Pass | 100 000 `resolveMesh`-Lookups, ein Cull, ein `std::sort` (rund 1,7 Mio. Vergleiche). Szene + 4 Kaskaden = mindestens 5 Sortierungen, dazu lokale Layer, SSAO, GI |
| Kopien | 6,4 MB `instanceTransforms`, je Pass 12,8 MB `{mvp, model}` |
| D3D12/Vulkan-Budget | Mit 6 Pässen darf jeder höchstens rund 10 900 Instanzen sehen, sonst ist der Ring des Frames leer |

Gemessen wurde nichts davon (`step3-cpu-memory-deep-dive` ab Zeile 324 nennt es ausdrücklich eine Hochrechnung). Es gibt auch
keine Foliage-Szene in der Messleiter (`scripts/perf/gen_reference_world.py` kennt nur Mesh-Entities) und keinen Profiler-Scope in
`extractFoliage`. Abschnitt 9.

## 3. Das Graph-Material-Problem: Instancing oder Wind, nicht beides

Die Vorlage `material_create template:"Foliage"` ist lit, `Masked`, mit einem `Wind Sway`-Knoten auf dem WPO-Pin. Jedes
Backend behandelt eine solche Charge anders (alles aus den Quellen, die Zeilen in 2.2 und Abschnitt 10):

| Backend | Verhalten bei N Instanzen eines Graph-Materials | Folge |
|---|---|---|
| OpenGL | Der Instanzzweig (`OpenGLRenderer.cpp:11525` G-Buffer, `:11749` Forward) steht **vor** dem Zweig für benutzerdefinierte Programme (`:11551`, `:11778`) und nimmt immer das eingebaute Instanzprogramm. Kommentar im Code: „Instanced batches always take the built-in instanced program“ | **Ein Draw, aber falsches Material:** kein Wind, kein Alpha-Discard, flaches PBR. Der Bug aus Altplan §2/§6.4 und Audit §7 Grenze 1 war da; **behoben in Teil 2b** (Abschnitt 12): ein Graph-Material zeichnet je Instanz mit seinem Programm, wie Metal |
| Metal | Ausgeschlossen durch `cMaterialPipeline == nullptr` (`MetalRenderer.mm:14325`, G-Buffer `:16104-16105`); Schleife mit `setVertexBytes` + Draw je Instanz | Richtig, aber N Draws |
| D3D11 | `drawMatInstance` je Instanz (`D3D11Renderer.cpp:7113, 7153`), je zwei Map/Unmap | Richtig, N Draws mit Mapping |
| D3D12 | `drawMatInstance`; der Material-Ring hat `k_matMaxDraws = 1024` Slots je Frame (`:3376`), der Überschuss **wird übersprungen**, einmal Warnung (`:10762-10769`) | Nur die ersten 1024 Instanzen je Frame erscheinen |
| Vulkan | `drawMatInstance`, `k_matMaxDraws = 1024` (`VulkanRenderer.h:438`), je Instanz `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` | wie D3D12, dazu teuer |

Dazu: Der Wind-Knoten wirkt nur dort, wo das Graph-Material wirklich gezeichnet wird, und der Schatten bleibt starr (2.3).
Das **Roadmap-Wort „GPU-instanced foliage with engine-driven wind sway“** (`terrain-vegetation-gap-audit` §9, Vorschlag) ist
auf keinem Backend wahr. Eine Korrektur geht nur mit Bestätigung des Menschen (Website-Deploy) und gehört zu dem Commit, der
Schritt 2e (unten) landet.

Zwei Wege, beide beschrieben, die Wahl liegt bei der Queen:

- **W-A: nur eingebautes PBR instanzieren** (heute schon so). Schnell, deckt Felsen, Steinchen, Bäume mit geometrischem
  Laub und Gras als Geometrie ab. **Kein Wind, kein Cutout** (Blattkarten, Grashalme aus Alpha). Als Zwischenstand sinnvoll,
  als Endstand nicht.
- **W-B: instanzierte Graph-Material-Vertexstufe** (`MaterialShaderLibrary::customVertex` bekommt eine Form mit
  Instanz-Matrix je Backend, der Konstantenring entfällt aus der Draw-Grenze). Das ist der eigentliche Fix und der Weg der
  Material-System-Vision (ein Graph, fünf Codegens). Aufwand L über fünf Backends, hier nur Metal und GL laufzeitprüfbar.
  Empfehlung: **Metal zuerst, GL im selben Zug (zusammen mit dem Gate-Fix), D3D11/D3D12/Vulkan als Windows-Gleis** mit
  mindestens Compile-Prüfung in der CI.

Eine dritte Variante (eingebauter Foliage-Shader mit Wind und Alpha-Test, fest in allen Backends) ist verworfen: sie baut dieselben
fünf Pipelines wie W-B, aber an der Material-Vision vorbei.

## 4. LOD, Culling, Impostor: der Bestand

- **`LODComponent`** (`LODComponent.h:7-27`): Liste `{meshId, maxDistance}` je Entity. `LODSystem::update`
  (`LODSystem.cpp:9-64`) misst den **3D-Abstand zum Entity-Ursprung**, nimmt die erste Stufe mit `dist <= maxDistance`
  und schreibt `mesh.meshAssetId`. **Keine Hysterese, keine Überblendung, keine Bildschirmgröße** (ein Entity auf der Stufengrenze
  wechselt jeden Frame; `test_lod.cpp:108-126` prüft nur Idempotenz). `RenderObject::lod` ist `MeshComponent::lodBias` und wird von
  keinem Backend gelesen.
- **Foliage hat keine LOD** (`FoliageComponent` hat keine Felder dafür). Nur `drawDistance` (Standard 80, Inspector 1-500) als harte
  XZ-Kante.
- **Kein Mesh-Simplifier.** Weder `meshopt` noch Dezimierung noch Quadrik im Repo; Importer erzeugen keine Stufen. Mesh-LODs sind
  **handgemacht** (Inspector „+ Level“ oder Szenen-JSON). Die Terrain-Stufen sind prozedural (65/33/17/9).
- **Kein Billboard-/Impostor-Pfad** (2.3). Docs erwähnen Impostore nur als Lücke (`game-readiness-audit` Z. 601,
  `terrain-vegetation-gap-audit` Z. 113).
- **Kein Distanz-Cull für Meshes**, nur für Lichter und Foliage. Ein Mesh ohne `LODComponent` wird in jeder Entfernung eingereicht.
- **Occlusion** ist aus, nur GL und Metal, und ignoriert Kleines wie Foliage (`OcclusionCuller.cpp:157-159`).

## 5. Zusammenspiel mit den Nachbarthemen

**Thema 164 (Zellen).** Foliage ist kein Entity, also betrifft `CellStreamer` sie nicht. Das Terrain bleibt Resident, das Foliage
ebenso: es gibt keinen Konflikt, solange `foliage` und `terrain` nicht in `movableKey`. Zusagen von hier an 164:
(a) Bucket-Größen sind Teiler der Zellgröße (16, 32, 64 m teilen 256 m), damit eine spätere Foliage-Erzeugung je Bucket an
Zellgrenzen anschließt; (b) Foliage nutzt **keine** UUIDs und braucht nichts vom neuen Index; (c) Handplatzierte Einzelobjekte
(E1) streamen mit den Zellen und profitieren von den Zellen *und* von den Verbesserungen an `GeometryPass`/`batchDepthRuns`. Nicht
Teil: Foliage-Streaming (Abschnitt 7.8).

**Thema 162 (Extract einmal pro Frame, persistenter `RenderWorld`).** Die Reibung liegt im `RenderWorld`-Layout und in
`RenderExtractor.cpp`. Vertrag:
1. Die Foliage-Ausgabe lebt in einer **eigenen Datei** (`FoliageExtract.cpp`), der Aufruf in `extract()` bleibt eine Zeile
   (heute `:1325`). Sie ist **O(Cluster)**, nicht O(Instanzen); 162 darf sie auch bei behaltenem `RenderWorld` jeden Frame neu
   laufen lassen (sie hängt von Kamera und Abständen ab, wie der aspektabhängige Schwanz `RenderExtractor.cpp:1292-1309`).
2. Cluster halten Instanzen über `std::shared_ptr<const …>`, nie über rohe Zeiger. Ein behaltener oder tief kopierter
   `RenderWorld` (Metals `FrameScope`-Wiederverwendung kopiert ihn heute komplett, **inklusive** aller Objekte je Instanz) kann nicht
   an einer neu gestreuten Schicht hängen.
3. `FoliageComponent` bekommt einen **Revisionszähler** (hochgezählt bei jedem Neustreuen und jeder Einstellungsänderung), den
   162 im `FrameKey` lesen kann. Gleiches Muster wie `HorizonWorld::structureEpoch` aus Plan 164 §7.2. Ein Zähler je Quelle, nicht zwei
   Zähler für dasselbe.
4. Beide Bearbeiter (`render-extractor-u-1` und der von 164) bekommen diesen Punkt als Erwähnung im Ergebnis-Post.

**Thema 174 (Wasser, offen, Zweig liegt nicht auf `release/0.7.0`).** Es ändert dieselben Dateien: `TerrainTools.cpp` (+349 Zeilen),
`TerrainSystem.cpp`, `SceneSerializer.cpp`, `TerrainChunkComponent.h`, `TerrainSculpt.*`. Schritte, die dort anfassen (Bug F1, Schichten
im Serializer), sollten **nach** dem Merge von 174 oder mit einem Rebase kurz davor landen; die Cluster-Arbeit (Abschnitt 8, Teil 2a) berührt
diese Dateien nicht. Wasser-Ausschluss für Foliage (F10) ist eine Folge für nach 174.

**Thema 180 (Auto-Landscape 2.0, Release 0.8.0).** Es plant Meshes je Landschaftsschicht (Felsen, Büschel, Kiesel) „über den vorhandenen
Instancing- und Foliage-Pfad“ und hat dazu **keinen eigenen Plan** auf dem Zweig (nur Hive-Text). Ohne mehrere Schichten je Terrain
und Regeln je Schicht (Hang, Höhe, Paint-Gewicht) lässt sich das nicht ausdrücken. Dieser Plan liefert den Vertrag: `FoliageLayer`
(6.2) und den Cluster-Pfad (6.3). Thema 180 baut **keinen** zweiten Streu- oder Zeichenpfad; die Regeln (Hang, Höhe, Paint-Schicht) gehören
dem Bearbeiter von 180 und hängen als Felder an `FoliageLayer`.

## 6. Entscheidung und Zielbild

### 6.1 Terrain-Layer-Instanzen oder anders? (D1)

**Ja: Scatter bleibt Terrain-Layer-Instanzen ohne Entity.** Begründung:
- Es **existiert** (Pinsel, Maske, Serialisierung, Wind-Knoten, Tests); der Schaden liegt in der Darstellung, nicht im Eigentum.
- Ein Entity je Pflanze zahlt 280 Byte, `propagateTransforms`, Hierarchie, Outliner, Undo und Collab je Stück. Eine Wiese ist
  1 Instanz/m², also 1 Mio. auf 1 km². Zellen (164) senken N auf rund 11k geladene Entities je Ausschnitt, das reicht für Häuser,
  nicht für Gras.
- Instanzen sind abgeleitet (Seed + Maske + Regeln), also klein im Speicher, im Undo und in der Collab-Nutzlast.

**Handplatziertes bleibt Entity.** Ein Dorf, ein einzelner Fels, ein Baum, den jemand auf den Zentimeter stellt, haben eine Identität (Auswahl,
Skript, Savegame). Sie laufen über E1 mit `LODComponent` und den Zellen aus 164. Für die Sichtweite in Dörfern kommt später ein
Impostor als *letzte Stufe der `LODComponent`* (gleiche Karte wie in 7.6, anderes Authoring).

Eine instanzierte Komponente für Handplatziertes (ISM-Stil, Instanzen je Stück gespeichert) ist **nicht** Teil von 0.7.0: sie bräuchte
ein Speicherformat außerhalb der Szene (eine eigene Datei, sonst wachsen Undo und Collab bei jedem Strich) und Auswahlwerkzeuge.

### 6.2 Schichten im Component (D2)

`FoliageComponent` bekommt `std::vector<FoliageLayer> layers`. Die heutigen Felder (Mesh, Material, Dichte, Seed, Skala, Distanz,
Maske) wandern in `FoliageLayer`; beim Laden eines alten Szenenformats entsteht `layers[0]` daraus. Speichern schreibt bei **einer** Schicht
das alte Format (alte Szenen bleiben byte-gleich, wie bei Tessellation und Wasser), erst ab zwei Schichten das neue Feld `layers`.

Warum nicht Kind-Entities mit einer neuen Komponente: sechs Registrierstellen (Memory `new-component-registration-places`), jede Schicht hätte
eine Streaming-Klasse für 164 zu bekommen (sonst „unbekannt = Resident“, ohnehin das Richtige), Collab- und Undo-Pfade würden mehr Entities
sehen. Dagegen spricht nur, dass Schichten dann nicht an anderen Flächen als dem Terrain hängen können; das ist heute auch nicht gefordert.

Schichtfelder (Entwurf): `name`, `visible`, Mesh, Material, `density`, `seed`, `minScale/maxScale`, `drawDistance`, `bucketSize` (Standard 32 m),
`castsShadow` (Standard `true`, damit nichts Bestehendes anders aussieht), `contributesAO` (`true`), `giOccluder` (Standard `false`, siehe 7.5),
`shadowDistance` (0 = wie `drawDistance`), `lods`, `maskRes`, `densityMask`. Regeln für Hang, Höhe und Paint-Schicht sind für Thema 180 reserviert.

### 6.3 Renderseite: Cluster-Objekte (D3)

Zwei Formen standen zur Wahl.

**(A) Eigene Liste `RenderWorld::foliageBatches`** nach dem Vorbild von `particleBatches`. Sauber, aber **jeder** Verbraucher von `objects` braucht
einen Haken: Schatten-Layer ×5, SSAO ×5, GI-Instanzbau ×5, Occlusion, Picking. Bäume brauchen Schatten, man kann sich also nicht wie bei Niederschlag
abmelden. Die Menge der Backend-Stellen, die ändern, ist groß, und hier ist nur Metal prüfbar.

**(B) Cluster-`RenderObject`.** Je Terrain × Schicht × LOD-Stufe × Bucket **ein** Objekt: gültige Welt-Bounds (Bucket-Box plus Mesh-Radius mal
`maxScale` plus Wind-Reserve), `meshAssetId`/`materialAssetId` der Stufe, `transform` = die **erste Instanz** (Fail-soft: ein Leser, der Cluster nicht
kennt, zeichnet eine echte Pflanze statt Müll) und ein Verweis `instanceBlock` (Index in eine Seitenliste `RenderWorld::instanceBlocks`, die einen
`shared_ptr` auf den Instanzspeicher, den Bereich und die Elternmatrix trägt; `RenderObject` wächst nicht). Aufgeklappt wird **nur** in zwei
gemeinsamen Funktionen: `GeometryPass::execute` (Instanzliste des Laufs) und `RenderSorter::batchDepthRuns` (Tiefenläufe). Backends sehen weiter
`DrawCall::instanceTransforms` und `DepthBatchList`.

**(B) gewinnt**, entscheidend ist die Zahl der Backend-Stellen. Culling, Sortierung und Refine laufen ab da über Cluster statt Instanzen:
100 000 Instanzen in 32-m-Buckets sind einige hundert bis wenige tausend Objekte. Jede Kaskade und jeder lokale Layer culled mit seiner eigenen
`viewProj` die Cluster-Boxen, **ohne dass ein Backend davon weiß**.

Was (B) trotzdem an Backend-Stellen kostet, alle einzeilig und mechanisch, Guard „Cluster überspringen“:
1. **Bounds-Refine-Schleifen** (5 Backends, **14 Stellen**, nicht fünf: Metal 5, Vulkan 5, GL 2, D3D11 1, D3D12 1, siehe 11.2; D3D11 `:6194-6199`, GL `:10651-10653`). Sie *überschreiben* heute `worldBounds` aus
   `mesh->localBounds.transformed(obj.transform)`; bei einem Cluster würde die Box auf **eine** Pflanze schrumpfen und der Bucket mit ihr verschwinden.
   Das ist die gefährlichste Falle des Entwurfs.
2. **GI-Instanzbau** (6 Stellen: GL `:5703`, D3D11 `:3159`, D3D12 `:6740`, Vulkan `:9930`, Metal HW `:8246`, Metal SW `:8056`). Filter bisher nur
   `castsShadow`; Cluster müssen heraus (Abschnitt 7.5, GI-Ausschluss).
3. **Occlusion-Culler** (`OcclusionCuller.cpp`): ein Cluster mit großer Bucket-Box darf kein Occluder sein.
4. Cap-Aufteilung (`kMaxInstancesPerDraw = 65536`) im Front-End: `GeometryPass` zerlegt lange Läufe in mehrere `DrawCall`s, die Mehrabschnitt-Logik (ein
   `DrawCall` je Section über dieselbe Liste) bleibt. Der Vertrag „bei genau einer Instanz bleibt `instanceTransforms` leer“ muss halten.

Drei der fünf Backends (D3D11, D3D12, Vulkan) sind lokal nicht prüfbar; die Guards sind Teil des **Windows-Gleises** (Abschnitt 8, 2f) mit
CI-Compile als Mindestprobe. Die Metal- und GL-Anteile gehören in Teil 2a.

### 6.4 Terrain-lokale Instanzen (D4)

Der Instanzspeicher hält Matrizen **relativ zum Terrain** (Position und Höhe aus `terrainHeightAt`, ohne Terrain-Versatz). Beim Aufklappen
wird mit der aktuellen Weltmatrix der Terrain-Entity multipliziert (`TransformComponent::worldMatrix`, wie `extractMeshes` es für alles tut); bei reiner
Verschiebung ist das ein Addieren, sonst eine Matrixmultiplikation in derselben Schleife, die ohnehin 64 Byte kopiert. Daraus folgt ohne weitere
Arbeit: gedrehtes, skaliertes, geparentetes und verschobenes Terrain stimmt (F2, F3), und Floating Origin (F4) schiebt die Foliage mit, weil das Terrain ein
Kind der Weltwurzel ist.

### 6.5 Instanzen bleiben abgeleitet

Kein Speichern einzelner Instanzen in der Szene. Die Bucket-Sortierung ist reine Laufzeitstruktur (stabile Sortierung der erzeugten Instanzen nach
Bucket; innerhalb eines Buckets bleibt die Erzeugungsreihenfolge, also zufällig). Das **Layout bleibt gegenüber heute bit-gleich**; ein neues,
bucket-lokales Streuen (für Terrains ab einigen km) ist ein späterer Schritt mit bewusster Layout-Änderung (7.8).

## 7. Bauplan

### 7.1 Datenmodell

```
FoliageComponent { bool visible; std::vector<FoliageLayer> layers; uint32_t revision;  // Laufzeit: stores }
FoliageLayer     { ...Felder aus 6.2...; std::vector<FoliageLod> lods; }
FoliageLod       { HE::UUID meshAssetId, materialAssetId; float maxDistance; }
Laufzeit (nicht serialisiert), je Schicht:
  std::shared_ptr<const FoliageStore> {
      std::vector<glm::mat4>  local;      // terrain-lokal, nach Bucket gruppiert
      std::vector<Bucket>     buckets;    // {uint32 first, count; AABB localBounds}
      int gridX, gridZ; float bucketSize;
  }
```

`FoliageSystem::update` bleibt der einzige Schreiber, streut weiter global und deterministisch (Layout wie heute), sortiert danach nach Bucket und
tauscht den `shared_ptr` aus. Der Dirty-Pfad aus F1 (Sculpt, Reset, Größe, Auflösung, Seed) kommt aus `TerrainSystem::updateTerrains`, dort wo
`regionDirty` verbraucht wird.

### 7.2 Extraktion (Datei `FoliageExtract.cpp`)

Pro Schicht und Frame: Terrain-Weltmatrix holen, Bucket-Boxen nach Welt transformieren und um Mesh-Radius × `maxScale` + Wind-Reserve erweitern (nur wenn das
Mesh resident ist; sonst Bounds ungültig lassen wie heute, damit nichts verschwindet), Bucket-Abstand zur Kamera (AABB-Abstand, nicht Mittelpunkt) gegen
`drawDistance`/`lods`, je Treffer ein Cluster-Objekt. Reihenfolge der Entscheidung je Bucket:
1. außerhalb `drawDistance`: nichts;
2. ganz in **einem** LOD-Band: ein Cluster dieser Stufe;
3. Band-Grenze durch den Bucket: Instanzen einzeln per Abstand auf Stufen verteilen (ein Zwischenpuffer je Frame, O(Instanzen dieses Buckets));
4. letzte Stufe mit Dichte-Ausdünnung: **Präfix des Bucket-Arrays** (die Erzeugungsreihenfolge ist zufällig, ein Präfix mit Anteil f ist eine gleichmäßige Ausdünnung,
   O(1), ohne Hash je Instanz).

Aus `castsShadow`, `contributesAO` und `shadowDistance` der Schicht ergeben sich die Objekt-Flags (Cluster jenseits der Schattenreichweite bekommen
`castsShadow = false`, das ist der billigste Schattenhebel überhaupt).

### 7.3 Aufklappen (gemeinsam)

`GeometryPass::execute` und `RenderSorter::batchDepthRuns` rufen **einen** Helfer `appendInstances(const RenderObject&, const RenderWorld&, std::vector<glm::mat4>&)`:
normales Objekt → `push_back(transform)`, Cluster → Block mit Elternmatrix verrechnet anhängen. Dasselbe Verfahren macht den Cap-Schnitt in `GeometryPass`
einmalig und für alle Backends. Eine Cluster-Instanz hat dieselbe Tiefen-Semantik wie ein Einzelobjekt (`castsShadow`, `contributesAO`, `skipEntity`).

### 7.4 Renderer: was sich ändert und was nicht

In Teil 2a/2b: GL-Gate (Graph-Material nicht ins eingebaute Instanzprogramm; Schleife behält das Material), die Guards aus 6.3 für Metal und GL, kein
neuer Shader, kein neues Pipeline-Layout. Nicht in 0.7.0-Teil 2a: instanzierte Graph-Material-Vertexstufe (3, W-B), Ringgrößen und 64-Byte-Schattenstride auf
D3D12/Vulkan, kompakte Instanzen mit GPU-Matrixaufbau, GPU-Culling.

### 7.5 Die Budget-Kette (D5)

Damit D3D12/Vulkan (kumulative 65 536 je Frame) nicht an Foliage zerbrechen, muss die Zahl, die einen Pass erreicht, klein sein. Die Kette, jede Stufe ein
eigener, kleiner Hebel:

1. **Bucket-Culling:** nur Buckets in Reichweite und Frustum erzeugen Cluster (Szene); für Schatten culled jeder Layer seine Cluster selbst.
2. **`shadowDistance` und `castsShadow` je Schicht:** Gras wirft keinen Schatten oder nur nahe; Bäume bis in die zweite Kaskade. Der Standard bleibt `true`,
   neue Schichten aus Voreinstellungen (Gras, Busch, Baum, Fels) setzen sinnvolle Werte.
3. **`contributesAO = false` für Gras** (SSAO-Vorpass ohne Gras).
4. **GI-Ausschluss:** Cluster gelangen nie in die GI-Instanzliste (Guard aus 6.3); `giOccluder` ist ein Opt-in, begrenzt nach Anzahl. Das **ändert das Aussehen** unter Baumkronen (zu hell im GI), deshalb ausdrücklich angesagt.
5. **LOD-Stufen und Dichte-Ausdünnung:** die Zahl der Instanzen wächst nicht mit der Distanz.
6. **Front-End-Cap-Schnitt** und, später, Backend-Kapazitätsmeldung, damit der Front-End statt still zu verlieren ausdünnt (Windows-Gleis).

### 7.6 LOD und Impostor (D7)

Machbar für 0.7.0 (Teil 3a): `FoliageLayer::lods` mit handgemachten Mesh-Stufen, Cluster je Stufe (7.2), Dichte-Ausdünnung, eine **Hysterese** in der Stufenwahl
(Band-Überlapp von wenigen Metern). Als letzte Stufe eine **statische Kreuz-Karte** (zwei gekreuzte Quads mit einer vorgebackenen Textur, normales Mesh, kein
Shaderaufwand). Sie braucht Cutout, also W-B aus Abschnitt 3 oder eine Textur ohne Alpha (nur für dichte Wipfel brauchbar).

Später (eigene Schritte, nicht 0.7.0):
- **Auto-LOD beim Import** (Simplifier fehlt komplett). Vorschlag meshoptimizer (MIT-Lizenz); eine **neue Abhängigkeit**, Entscheidung beim Menschen.
- **Oktaeder-Impostor** (8×8 Ansichten, Albedo+Alpha und Normal+Tiefe als zwei Texturen, ein Werkzeug „Bake Impostor“ im Editor, Material mit Billboard-WPO
  und Atlas-Auswahl nach Blickrichtung, als Graph-Material, setzt W-B voraus).
- **Dither-Überblendung** zwischen Stufen: braucht einen Überblendwert je Instanz, und im 128-Byte-Layout (`{mvp, model}`) gibt es kein freies Feld. Wird zusammen mit dem
  kompakten Instanzformat entschieden. Bis dahin bleibt es bei Hysterese und kurzem Überlapp.
- **Gebäude-Impostor** als letzte Stufe der `LODComponent` (gleiche Karte, anderes Authoring), HLOD-Verschmelzung ist ein anderes Thema.
- `LODSystem`: Hysterese und Bildschirmgröße (klein, unabhängig, hilft jedem Entity).

### 7.7 Wind, Cutout, Kollision, Autoring

- **Wind:** wirkt, sobald Graph-Material-Instancing steht (3). Der starre Schatten bleibt als dokumentierte Grenze (kein WPO in Tiefenpässen); für Gras irrelevant
  (wirft keinen), für Bäume akzeptiert.
- **Cutout in Tiefenpässen** (Blattkarten werfen sonst volle Rechtecke): braucht einen Alpha-Test-Fragment in den Schatten-/Tiefenpipelines aller fünf Backends für `Masked`-Materialien.
  Eigener Schritt; bis dahin sind Baumschichten mit Karten-Laub eingeschränkt.
- **Kollision:** keine in diesem Thema. Bäume bleiben begehbar (Audit §3.4). Für Stämme käme später ein Näherungs-Collider je Bucket in Spielernähe.
- **Autoring:** Mehrfachschicht-Listenansicht im Landscape-Tab (heute `tvw.front()` und eine Schicht), Voreinstellungen (Gras, Busch, Baum, Fels), Anzeige
  „Instanzen / sichtbar / Cluster“, MCP: `foliage_*`-Werkzeuge oder Aufnahme in `terrain_info`.

### 7.8 Streaming und große Terrains (nicht 0.7.0)

Heute braucht jede Schicht die gesamte Instanzliste im Speicher (64 Byte, 1 Mio. = 64 MB) und streut synchron. Ab einigen km² geht das nicht mehr. Die Lösung (Streuen **je
Bucket, bucket-lokal deterministisch** mit hashbarem Bucket-Seed, gestreamt um die Kamera, mit LRU) **ändert das Layout** gegenüber dem globalen Strom und wird deshalb
mit einem Versionsfeld (`scatterVersion`) eingeführt, damit alte Szenen auf dem alten Generator bleiben. Bucket-Größen als Teiler von 256 m (5) halten das an die Zellen
von 164 anschlussfähig. Kompakte Instanzen (16 Byte statt 64) und GPU-seitiges Culling sind derselbe Ausblick.

## 8. Schritte und was bis zur Deadline geht

Deadline laut Hive: 10.10. 23:00 (zum Zeitpunkt dieses Schreibens rund 10 Stunden). Vollbau lokal rund 35 Minuten, Windows-CI rund 17 Minuten. Der vorhandene
Schnitt der Queen (Schritt 2 „GPU-instanced Rendering“, 3 „LOD/Impostore“, 4 „Verifikation“) ist für die Größe zu grob. **Vorschlag zur Umbenennung:**

| Teil | Inhalt | Dateien (Schwerpunkt) | Aufwand | Prüfbar hier | Bis 23:00? |
|---|---|---|---|---|---|
| **2c** Messzeug zuerst | Zeuge `HE_DUMP_FOLIAGETEST=<N>` (Terrain + Schicht mit N Instanzen, eingebautes Mesh), Foliage-Option in `gen_reference_world.py`, Profiler-Scope in `extractFoliage`, Basismessung 10k / 100k / 500k auf Metal | `EditorApplication.cpp` (Dump), `scripts/perf/*`, `RenderExtractor.cpp` (1 Scope) | 1-2 h | ja (Metal) | **ja, zuerst** (ohne Basis ist „schneller“ nicht belegbar) |
| **2a** Cluster-Pfad | `FoliageStore` + Buckets (Layout bit-gleich), terrain-lokal + Elternmatrix, `FoliageExtract.cpp`, `instanceBlocks`, Aufklappen in `GeometryPass`/`batchDepthRuns` samt Cap-Schnitt, Guards (Metal, GL, Occlusion; die D3D/Vulkan-Guards als reiner Textpatch mit CI-Compile), `giOccluder`/`castsShadow`/`contributesAO`/`shadowDistance` (eine Schicht, Format bleibt), Revisionszähler, `he_tests` (Cluster-Aufklappen = Einzelobjekte, Bounds, Cap, Weltmatrix gedreht/skaliert/geparentet), Metal-Zeuge | `FoliageComponent.h`, `FoliageSystem.*`, neu `FoliageExtract.cpp`, `RenderObject.h`, `RenderWorld.h`, `RenderPass.cpp`, `RenderSorter.*`, 5× Refine-Schleife, 6× GI-Filter, `OcclusionCuller.cpp`, `tests/test_foliage.cpp`, `tests/test_culling.cpp` | 4-6 h inkl. Vollbau und `ctest` | Metal + GL + Unit | **ja, wenn 2c und 2a noch in der nächsten Stunde starten** (es ist 12:33, bis 23:00 bleiben rund 10 Stunden) |
| **2b** GL-Gate und Scatter-Bugs | GL: Graph-Material nicht ins eingebaute Instanzprogramm, Schleife behält Material; F1 (Dirty bei Sculpt/Reset/Größe), Test | `OpenGLRenderer.cpp`, `TerrainSystem.cpp`, `TerrainTools.cpp`, `tests/test_foliage.cpp` | 1-2 h | GL headless | ja, parallel zu 2a (nur `test_foliage.cpp` und `FoliageSystem` überschneiden sich; 2b zuerst oder mit Rebase). `TerrainTools/TerrainSystem` kollidieren mit 174 |
| **2d** Schichten | `layers`-Vektor, Migration, Serializer, Inspector, Pinsel pro Schicht, Voreinstellungen | `FoliageComponent.h`, `SceneSerializer.cpp`, `InspectorPanel.cpp`, `TerrainTools.cpp`, `EngineApi`/MCP | 3-4 h | ja | möglich, aber **nach 2a und besser nach Merge von 174**; Voraussetzung für Thema 180 |
| **3a** LOD-Stufen | `lods`, Cluster je Stufe, Grenz-Aufteilung, Ausdünnung, Hysterese, Kreuz-Karte als Datei | `FoliageExtract.cpp`, Inspector | 3-4 h | ja | nur wenn 2a früh fertig, sonst morgen |
| **2e** Graph-Material-Instancing | W-B: instanzierter `customVertex` je Backend, GL zuerst mitgezogen, Material-Ring unabhängig von der Draw-Grenze | `MaterialShaderLibrary.cpp`, 5 Backends | L | Metal + GL | **nein** (morgen, Windows-Gleis separat) |
| **2f** Windows-Gleis | D3D11/D3D12/Vulkan: Guards aus 2a laufzeitprüfen, Instanz-Budget (Ring, 64-Byte-Schattenstride, Kapazitätsmeldung), Graph-Material-Instancing (2e) | `*Renderer.cpp` | M-L | **nein** (Windows-Gerät NN-WS03) | nein; Entscheidung der Queen, ob und wann (Memory *keine D3D/Vulkan-Arbeit auf dem Mac*) |
| **3b** Auto-LOD | meshoptimizer-Integration | `HE_Tools` | M + Abhängigkeits-Entscheidung | ja | nein |
| **3c** Impostor | Kreuz-Karten-Bake (Editor-Aktion), später Oktaeder | `HE_Tools`, Editor, Material | L | teils | nein, 0.8.0 |
| **4** Verifikation | Messleiter (9), Bild-Vergleich Metal, `ctest` mit `shaderc ON`, CI-Lauf | – | 1-2 h | Metal/GL | Teilmessung nach 2a (Metal), vollständig erst nach 2e/2f |

Dateisperren (`hive_claim`) für 2a: die oben genannten; Konflikte mit anderen Themen: `RenderExtractor.cpp` und `RenderWorld.h` (162, durch die eigene Datei
und die zwei Felder klein gehalten), `SceneSerializer.cpp` (164, 174; erst 2d berührt es), `TerrainTools.cpp`/`TerrainSystem.cpp` (174; erst 2b/2d).

**Abnahme 2a:** (1) `he_tests` grün, darunter ein Test „Cluster aufgeklappt == dieselben Einzelobjekte“ (Anzahl, Matrizen, Reihenfolge-unabhängig) und ein Test, dass
`worldBounds` eines Clusters nach der Refine-Schleife **nicht** schrumpft; (2) Metal-Zeuge mit 100k Instanzen: `draws` im Profiler, Bild vorher/nachher pixelgleich
(Layout bit-gleich, sonst Abweichung erklären), A/B mit `HE_MTL_INSTANCING=0` und `HE_DEPTH_INSTANCING=0`; (3) `RenderExtractor::extract`-Anteil der Foliage unabhängig von der Gesamtzahl der
Instanzen, nur noch von den sichtbaren Buckets abhängig.

## 9. Verifikation: was fehlt und was gebraucht wird

Vorhanden: `scripts/he_perf_capture.py` (nur Metal; `--scene`, `--set`, `--cam`, `--frames`, `--detailed`), `scripts/perf/world_streaming_ladder.sh`, `ladder_table.py`,
`step3_frames.py`, `xctrace_timeprofile.py`, `scripts/he_shot.py` (`HE_DUMP_*`, ein Bild in 1280×720), `alloc_probe`, die Profiler-Scopes `RenderExtractor::extract`,
`RenderExtractor::reuse`, `ExtractMeshes`, `FrustumCuller::cull`, `Foliage` (`SceneSystems.cpp:143`), `LOD`. Die Messleiter aus Thema 153 (1k/10k/50k/100k/200k Entities) lässt sich wiederverwenden.

Fehlt (Teil 2c): eine Foliage-Szene (kein 100k-Benchmark; `test_culling.cpp:198` testet 256 Objekte), ein Profiler-Scope **in** `extractFoliage` (die Zahlen dort gehen heute im
`Foliage`-Scope des Systems oder im Extract unter), eine Foliage-Option in `gen_reference_world.py` (sie erzeugt nur Mesh-Entities; `FoliageComponent` braucht ein `TerrainComponent` auf derselben Entity),
und Zähler für Cluster/Instanzen/Aufklappen. Alle 100k-Ergebnisse auf Metal; GL über `HE_DUMP_*`-Läufe; D3D/Vulkan nur auf dem Windows-Gleis.

**Stand Teil 2c (10.10.2026):** Scope `ExtractFoliage`, Zeuge `HE_DUMP_FOLIAGETEST=<N>`, `gen_reference_world.py --foliage`, `scripts/perf/foliage_ladder.sh` und `foliage_ladder_table.py` sind gebaut, die Basismessung 10k / 100k / 500k auf Metal (alles in Reichweite und rund 20 %) steht in `docs/perf-audit/foliage-baseline-2026-10-10.md`. Noch offen aus diesem Abschnitt: Zähler für Cluster/Instanzen/Aufklappen (gibt es seit 2a, Abschnitt 11), 1 Mio. Instanzen in der Leiter, GL-Läufe.

**Stand Teil 2a (10.10.2026):** Cluster-Pfad gebaut, geprüft auf Metal und GL, Befunde und Zahlen in Abschnitt 11.

**Stand Verifikation (Schritt 5, 10.10.2026):** die Leiter ist auf Metal vorher gegen nachher gefahren (10k, 100k, 500k, 1 Mio., "alle" und rund 20 %, abwechselnd, unter Fremdlast), Bildvergleich Metal und voller `ctest` stehen, Befund und
Zielabgleich in Abschnitt 13, Zahlen in `perf-audit/foliage-verification-2026-10-10.md`. Weiter offen: GL-Zeiten, D3D/Vulkan, 1k/50k/200k, Kamera am Rand.

Messleiter (Vorschlag): 10k, 100k, 500k, 1 Mio. Instanzen, jeweils bei sichtbarem Anteil 100 % und 20 % (Kamera mitten drin, Kamera am Rand). Kennzahlen: `RenderExtractor::extract` p50, `FrustumCull`, Sortierung,
`GeometryPass`, CPU/Frame p50, `draws`/`tris`, GPU-Zeit (`gpu_time_by_process.py`), RSS. Alles vor und nach 2a, mit `--no-counters` für FPS-Läufe (Memory *engine-profiler*). **Zielwerte** (nicht gemessen,
nur Richtung): Extract-Kosten der Foliage unabhängig von der Gesamtzahl, 100k platziert mit 20 % sichtbar unter 0,5 ms; `draws` gleich Anzahl der Mesh-Läufe, nicht Instanzzahl.

## 10. Nicht geprüft, Annahmen, Risiken, Provenienz

**Nicht gebaut, nicht gelaufen.** Alles ist Code- oder Doku-Lektüre. Keine Laufzeit-Zeugen, keine Messung außer `sizeof(RenderObject) = 280` (Mini-Compile gegen die echten Header, Clang, Standard-Flags).

**Von mir im Code gegengelesen** (Quelle des Textes oben, Stichproben): `FoliageComponent.h`, `FoliageSystem.cpp`, `extractFoliage` und Umgebung, `RenderObject.h`, `GeometryPass` (`RenderPass.cpp:14-150`),
`FrustumCuller.cpp`, `RenderSorter.cpp`, `RenderSorter.h` (Tiefenhelfer), alle `k_maxInstances`-Stellen (grep), das Metal-`fits`-Gate (`:14316-14327`), das D3D11-Gate (`:7233-7236`),
die GL-Reihenfolge (`:11525` vor `:11551`), D3D12 `k_maxDraws`, `k_matMaxDraws` und die Ringvoll-Warnung, Vulkan `k_matMaxDraws`, D3D11 Bounds-Refine, GL Bounds-Refine, D3D11-GI-Filter,
die Schatten-Aufrufstellen D3D11 und Metal, `FloatingOrigin.h`, `TerrainTools.cpp:684-713`, das Fehlen von `discard` in `shaders/*.frag`, `TerrainComponent.h`,
die Docs 164/162 (komplett), `gpu-instancing-cross-backend-plan.md`, `terrain-vegetation-gap-audit`, `step3-cpu-memory-deep-dive` (Foliage-Absätze).

**Nur aus Teilberichten (Explore-Läufe), nicht einzeln gegengelesen:** die Zeilennummern der übrigen Backends in 2.2/3 (Vulkan, D3D12-Ringe, Metal-`FrameUploadRing`), `ShadowPass::execute` ist
auf GL/D3D11/D3D12 tote Arbeit (zwei Läufe unabhängig, beide nur über die Abwesenheit des Tokens), Aussagen zu `LODSystem`-Innenleben und `test_lod.cpp`, Editor-Zeilen (`InspectorPanel`, `TutorialSteps`, `McpToolsEntity`),
`CellSplit.cpp:25-31`, Terrain-Chunk-Zeilen in `TerrainSystem.cpp`, der Stand von Thema 174 und 180 auf ihren Zweigen, Compute-Verfügbarkeit je Backend, die Zahlen aus `world-streaming-baseline`.

**Annahmen:**
- Das Layout bleibt bei der Bucket-Sortierung bit-gleich, weil nur umgeordnet wird. Bild-Gleichheit hängt davon ab, dass die Reihenfolge innerhalb eines Laufs für das Ergebnis egal ist (kein Overdraw mit Alpha, opak mit Tiefentest).
- Cluster-Bounds mit Mesh-Radius × `maxScale` sind konservativ genug; ein Mesh mit Wind-Biegung braucht eine Reserve (aus der Schicht, Standard einige Dezimeter).
- Ein `shared_ptr`-Zähler je Cluster und Extract ist billig (O(Cluster)).

**Risiken:**
1. **Bounds-Überschreiben (6.3 Punkt 1):** übersieht ein Backend den Guard, verschwinden Buckets (Metal/GL sind hier prüfbar, D3D/Vulkan nicht). Abmilderung: Test mit `worldBounds` nach der Refine-Schleife, in der CI-Compile für die übrigen.
2. **Wind plus Cutout auf Schicht 1:** ohne W-B bleibt das Ergebnis für Gras/Bäume mit Karten mager. 2a bringt Felsen und Bäume mit Geometrie-Laub, nicht Wiesen aus Alpha-Halmen.
3. **GI-Ausschluss ändert das Aussehen** (Boden unter Baumkronen zu hell im GI). Opt-in `giOccluder`, angesagt statt nebenbei mitgenommen.
4. **Konflikte mit 162, 174:** durch eigene Datei und kleine Felder begrenzt, Rebase-Aufwand bleibt.
5. **D3D12/Vulkan-Budget:** bis Teil 2f bleibt die kumulative Grenze; Foliage-Szenen dort mit LOD und Schattenreichweite klein halten, sonst stilles Abschneiden (D3D12 ab 16 384 in der Schleife).
6. **Messbarkeit:** ohne Teil 2c ist jeder Gewinn unbelegt. Teil 2c läuft vor 2a.

**Entscheidungen für die Queen / den Menschen:**
1. Schichten im Component (D2) bestätigen.
2. W-A als Zwischenstand und W-B Metal/GL zuerst (3).
3. Windows-Gleis (2f) einplanen und wann; Memory verbietet D3D/Vulkan-Arbeit ohne prüfbares Gerät.
4. meshoptimizer als Abhängigkeit (3b).
5. GI-Ausschluss als neuer Standard für Foliage (7.5).
6. Roadmap-Text „GPU-instanced foliage with wind“ nach 2e korrigieren (braucht die Bestätigung für den Deploy).
7. Reihenfolge 2a/2b vor 174-Merge oder danach.


## 11. Stand Teil 2a (Schritt 3, 10.10.2026)

Zweig `claude/instanced-foliage-und-lod-impostor-fuer-b-ume-gras-felsen`, gebaut auf `08a8938b` (Merge von
`origin/release/0.7.0`, damit sind die fünf roten Tests aus Schritt 2c behoben). Der Cluster-Pfad aus 6.3 und 7.1 bis
7.3 steht auf Metal und GL, geprüft mit `he_tests` und mit Bildern. D3D11, D3D12 und Vulkan sind reiner Textpatch.

### 11.1 Was gebaut ist

| Baustein | Stelle |
|---|---|
| Store und Buckets: terrain-lokale Matrizen, 32-m-Raster, `shared_ptr`, Reihenfolge im Bucket = Erzeugungsreihenfolge | `HorizonScene/FoliageStore.h`, `FoliageSystem.cpp` |
| Komponente: `store`, `revision`, `bucketSize`, `castsShadow`, `contributesAO`, `shadowDistance` (Laufzeitfelder, **nicht serialisiert**, Format unverändert); `cachedInstances` bleibt unverändert (Weltkoordinaten, Erzeugungsreihenfolge) | `Components/FoliageComponent.h` |
| Cluster-Datenmodell: `RenderObject::instanceBlock` (+4 Byte), `RenderWorld::instanceBlocks`, `InstanceBlock` (`shared_ptr` auf den Store, Bereich, Elternmatrix, Art Identität/Verschiebung/allgemein) | `RenderObject.h`, `RenderWorld.h` |
| Extraktion: ein Cluster je Bucket in Reichweite, Bounds aus den Mesh-Bounds, angeschnittene Buckets instanzweise | `FoliageExtract.cpp` (eigene Datei; in `RenderExtractor.cpp` ein Aufruf und ein Guard) |
| Aufklappen und Cap: `GeometryPass`, `RenderSorter::batchDepthRuns`; Läufe über 65 536 Instanzen werden in mehrere Draws geschnitten | `RenderPass.cpp`, `RenderSorter.cpp`, `RenderConstants.h` (`kMaxInstancesPerDraw`) |
| Bounds-Refine aller Backends über **eine** Funktion `RenderObject::refineWorldBounds` | 14 Stellen in 5 Backends |
| Guards: sechs GI-Instanzbauten, Occlusion-Culler (Occluder-Auswahl), Kaskaden-Fit im Extraktor | siehe 11.2 |
| Revisionszähler: bei jedem Neustreuen und jeder Einstellung, die die Extraktion liest | `FoliageSystem.cpp` |
| Tests: 19 Fälle | `tests/test_foliage_cluster.cpp` |
| Schalter: `HE_FOLIAGE_CLUSTERS=0` (ein Objekt je Instanz wie bisher), `=ordered` (Prüfmodus, 11.3), `HE_FOLIAGE_STATS=1` (Zähler im Log) | `FoliageExtract.cpp` |

Das Layout des Scatters ist unverändert: `test_foliage_cluster.cpp` enthält eine Kopie des alten Generators und vergleicht jede Matrix. Auf Apple clang (arm64)
ist das bitgleich (Abweichung 0, auch die Bilder). Auf GCC 13 (x86-64, Linux-CI) unterscheiden sich 150 von 1280 Matrizen in den letzten Bits von der Kopie
im Test (der erste CI-Lauf mit einem Bit-Vergleich war dort rot); der Test vergleicht deshalb Position und Ausrichtung mit einer Toleranz von 1e-4 und 1e-5,
weit über der Rundung und weit unter jeder echten Änderung des Generators. Ob die alte Bibliothek auf GCC dieselben Bits geliefert hätte, ist nicht geprüft. Die Instanzen werden beim Aufklappen mit der Weltmatrix des Terrains verrechnet, ein gedrehtes,
skaliertes, verschobenes oder geparentetes Terrain trägt seine Pflanzen jetzt mit (F2, F3 und F4 aus 1.4 sind damit für
den Renderweg erledigt; der Scatter selbst reagiert weiterhin nur auf `dirty`).

### 11.2 Was der Plan anders sagte, und neue Funde

1. **14 Refine-Stellen, nicht fünf.** Metal: `EncodeShadowMap`, `EnsureGIProbeGrid`, `EncodeSSAO`, Szene, G-Buffer.
   Vulkan: Decal-Tiefe, `EncodeShadowMap`, `DrawScene`, `runGi`, `runSSAO`. GL: `EnsureGIProbeGrid`, `DrawScene`.
   D3D11 und D3D12: `DrawScene`. Alle laufen jetzt über `RenderObject::refineWorldBounds`; ein Cluster behält die Bounds des
   Extraktors. `test_foliage_cluster.cpp` liest die Backend-Quellen und scheitert an jeder handgeschriebenen Aktualisierung
   und an jeder GI-Schleife, die Cluster nicht überspringt. Das fängt auch D3D und Vulkan, die hier nicht laufen.
2. **Weitere Leser von `objects`, die der Plan nicht kannte.** Der TAA-Velocity-Pass jedes Backends zeichnet je sortiertem
   Objekt einmal; Vulkan hat dazu den Decal-Tiefenpass und die SSR-Vorpass-Rückfallschleife. Ein Cluster würde dort nur seine
   erste Pflanze zeichnen. Metal und GL klappen Cluster in der Velocity-Schleife auf (jede Pflanze statisch, Velocity = Kamerabewegung).
   D3D11, D3D12 und Vulkan **überspringen** Cluster dort (Velocity), in der Decal-Tiefe und im SSR-Vorpass (Textpatch, Windows-Gleis).
3. **TAA-Befund (alter Fehler, nicht neu).** Die Velocity-Historie ist je `entityId` geführt; alle Pflanzen einer Schicht tragen die
   Id des Terrains und lasen dieselbe "vorige Pose". Der Cluster-Pfad ist dort richtig, nicht bitgleich: unter AA=3 weichen
   4,3 % (Metal) und 3,7 % (GL) der Pixel ab (20 000 Pflanzen).
4. **Der Kaskaden-Fit sah Foliage nie, ein Cluster hat gültige Bounds.** `fitDirectionalShadow` bildet die Szenenbox aus den
   Objekt-Bounds; ohne Guard hätte jede Foliage-Szene ihre Schatten-Kaskaden verschoben. Cluster bleiben draußen (ein Test
   vergleicht die Kaskadenmatrizen mit und ohne Cluster).
5. **Der eingebaute Würfel hat keine Bounds.** `registerStaticMesh` berechnet sie nicht (`boundsMin == boundsMax == 0`, deshalb
   verlangt `extractMeshes` `b.max != b.min`). Der Extraktor liest dann die Vertices (cooked oder lose), je Mesh einmal.
6. **`cachedInstances` bleibt und der Speicher ist doppelt** (64 Byte je Instanz im Store zusätzlich), weil `test_foliage.cpp`,
   `test_terrain_tools_ui.cpp` und die Anzeigen in `InspectorPanel`/`TerrainTools` es lesen und `TerrainTools` Thema 174 gehört.
   Schritt 2d (Schichten) ersetzt es durch den Store.
7. **`drawDistance` ist am Rand exakt.** Ein Bucket, den die Kreislinie schneidet, wird instanzweise gefiltert (derselbe Test auf
   denselben Weltpositionen wie der alte Pfad) und als eigener Cluster über einen Zwischenvektor des Frames ausgegeben.
   Deshalb ist die sichtbare Menge identisch; bei 100k Instanzen und `drawDistance` 100 m sind es 24 Randbuckets mit 15 529 Einzeltests je Frame (19 715 Instanzen sind in Reichweite).
8. **`shadowDistance` ist bucket-genau:** ein Bucket wirft, solange irgendein Teil in Reichweite liegt. `giOccluder` ist **nicht**
   gebaut. GI schließt Cluster auf allen Backends aus (Plan 7.5 Punkt 4), das verändert das Aussehen unter Baumkronen, wenn GI
   an ist (Standard: aus). In keinem Zeugenlauf war GI an.
9. **Picking, Rahmen-Auswahl, Marquee und Weltvorschau sehen einen Cluster als eine Pflanze** (`transform` = erste Instanz). Das
   ist der Entwurf (fail-soft); wer Foliage per Klick auf die Pflanze anwählen will, braucht einen Entfaltungsweg in `ScenePick`.
10. **`HE_DEPTH_INSTANCING` ist ein Schalter für D3D11, D3D12 und Vulkan** und hat auf Metal keine Wirkung. Der A/B auf diesem
    Gerät ist `HE_MTL_INSTANCING=0`.

### 11.3 Prüfung

- **Build und Tests:** Release, `HE_ENABLE_SHADERC=ON`, `ctest -j4 --timeout 1500`: 257 von 257 grün (2 übersprungen: `runtime_size_app_*`).
  Die fünf roten Tests aus 2c sind nach dem Merge von `release/0.7.0` grün. Mutationsprobe: ohne den Kaskaden-Guard, ohne den Cap-Schnitt,
  ohne die Behandlung der angeschnittenen Buckets, ohne den Occluder-Guard und mit zu kleinen Cluster-Boxen scheitert je der passende Fall.
- **Bilder**, Metal und OpenGL 4.1, `HE_DUMP_FOLIAGETEST` (`HE_SKY_TIME=30`, AA aus, Wolken aus), jeweils gegen das Binary
  vom Stand `08a8938b` (Deploy-Klon, rpaths auf `@loader_path`). Wiederholung des Vorher-Laufs und `HE_MTL_INSTANCING=0` sind bitgleich.
  Wiederholbar mit `python3 -I scripts/perf/foliage_pixel_ab.py --before <Klon> --after out/deploy/Editor [--rhi OpenGL]` (rohe BMPs, kein PIL nötig).

| Fall | `HE_FOLIAGE_CLUSTERS=0` (neues Binary) | `=ordered` | Standard (Cluster) |
|---|---|---|---|
| Metal, Vorwärts, 100k alle in Reichweite | bitgleich | bitgleich | 751 Pixel (0,081 %) |
| Metal, Deferred, 100k | bitgleich | bitgleich | 878 (0,095 %) |
| Metal, 100k, `drawDistance` 100 m, Kamera mittendrin | bitgleich | bitgleich | 423 (0,046 %) |
| Metal, 100k, tiefe Sonne (lange Schatten) | bitgleich | bitgleich | 579 (0,063 %) |
| Metal, 100k, Skala 0,8 bis 1,6, tiefe Sonne, `FOLIAGEMESH=sphere` (Mesh-Id nicht registriert, siehe unten) | bitgleich | bitgleich | 571 (0,062 %) |
| Metal, 500k alle in Reichweite (vorher über der Instanz-Cap) | bitgleich | bitgleich | 3 730 (0,405 %) |
| GL, Vorwärts, 100k | bitgleich | bitgleich | 1 196 (0,130 %) |
| GL, 100k, 100 m | bitgleich | bitgleich | 611 (0,066 %) |
| GL, 100k, tiefe Sonne | bitgleich | bitgleich | 808 (0,088 %) |

  **Der Standardpfad ist nicht pixelgleich, und das ist keine Fehlstelle der Geometrie.** Ohne Bloom, SSAO und Schatten sind es 35 einzelne
  Pixel (0,004 %); Bloom verteilt sie auf rund 750. Es sind Schnittkanten, an denen zwei opake Flächen (Würfel an Würfel, Würfel am Boden) auf dieselbe
  Tiefe fallen. Dort entscheidet die Zeichenreihenfolge im Batch: der alte Pfad sortiert jede Pflanze nach Abstand, der Cluster-Pfad zeichnet
  Bucket für Bucket in Erzeugungsreihenfolge. `HE_FOLIAGE_CLUSTERS=ordered` zeichnet dieselben Instanzen in der Reihenfolge des alten Pfads (ein Cluster, nach
  Abstand sortiert, ohne Culling) und ergibt in allen neun Fällen **dasselbe Bild wie vorher, Byte für Byte**. Damit sind Store, Aufklappen, Cap-Schnitt und die Tiefenpässe
  (Schatten, SSAO) auf einem echten Gerät als genau belegt; was bleibt, ist die Reihenfolge. Das Culling der Bucket-Boxen deckt `test_foliage_cluster.cpp` ab
  (keine Pflanze im Bild geht verloren), der Pruefmodus zeichnet ohne Culling.
- **Zur Zeile `FOLIAGEMESH=sphere`:** der Zeuge nennt dafür `{257, 1}`, und diese Id ist in keinem ContentManager registriert (`grep` über `src/`: nur der Zeuge selbst). Die Backends zeichnen
  dafür den Würfel als Rückfall (das Bild zeigt Würfel), der Extraktor findet kein Mesh und lässt die Cluster-Bounds ungültig. Die Zeile belegt also einen zweiten Skalenbereich und den Fall
  "Cluster ohne bekannte Mesh-Bounds" (nie culled, vollständig gezeichnet) auf einem Gerät, **keine Kugel**. Dass die Bounds eines gelesenen Meshes stimmen, prüft `test_foliage_cluster.cpp`.
  (Auch die Basismessung aus 2c hat mit `HE_DUMP_FOLIAGEMESH=sphere` nie eine Kugel gemessen.)
- **Compile:** Metal und GL kompiliert und gelaufen. `VulkanRenderer.cpp` ist mit `clang -fsyntax-only` gegen die MoltenVK-Header geprüft (Negativkontrolle mit
  absichtlichem Tippfehler schlägt an). **D3D11 und D3D12 sind lokal nicht übersetzbar**; ihre Zeilen (zwei Refine-Stellen, zwei GI-Filter, zwei Velocity-Schleifen)
  prüft nur der Windows-Job der CI.

### 11.4 Messung (Metal, vorher gegen nachher, abwechselnd)

Ausführlich in `docs/perf-audit/foliage-clusters-2026-10-10.md`. Vorher-Binary: Deploy-Klon von `08a8938b`. **Die Bedingungen waren schlecht** (Load 7 bis 14,5 durch fremde
Compiler, Akkubetrieb, Bildschirm teils entsperrt, Stromsparmodus aus; die Basismessung lief gesperrt, im Stromsparmodus, bei Load 2 bis 3). Absolute Millisekunden sind deshalb
nicht mit der Basismessung vergleichbar (gleiches Binary, zwei Durchgänge, Faktor 2); die Verhältnisse innerhalb der abwechselnden Paare sind es.

| Fall | `Render` vorher gegen nachher (Median der Läufe, ms) | `ExtractFoliage` | `draws` | `objects` |
|---|---|---|---|---|
| 100 000, alle in Reichweite | 311 gegen 24 (13x); Pass 1: 246 gegen 12 (20x) | 6 bis 10 gegen 0,05 bis 0,08 | 3 gegen 3 | 100 004 gegen 173 |
| 100 000, rund 20 % | 61 gegen 9,3 (6,6x); Pass 1: 25 gegen 3,7 (6,8x) | 3,1 bis 3,5 gegen 0,35 | 3 gegen 3 | 19 719 gegen 47 |
| 500 000, rund 20 % | 414 gegen 28 (15x); Pass 1: 290 gegen 15 (19x) | 9,6 bis 17,5 gegen 1,5 bis 2,7 | 3 gegen 3 | 98 267 gegen 47 |
| 500 000, alle in Reichweite | 3 190 gegen 105 (30x) | 103 bis 111 gegen 0,07 bis 0,11 | **139 794 gegen 5** | 500 004 gegen 173 |

Abnahme 2a (3): der Foliage-Anteil am Extract hängt nicht mehr von der Gesamtzahl ab. Alle in Reichweite ist er bei 100k und 500k gleich (unter 0,1 ms, dieselben 169 Buckets), `test_foliage_cluster.cpp` prüft das
auch ohne Gerät. **Mit begrenzter Sichtweite hängt er von den Pflanzen in den Randbuckets ab** (bei 100k 15 529 Einzeltests, 0,35 ms; bei 500k 76 781, 1,5 bis 2,7 ms), nicht von denen im Rest des Geländes. Das Plan-Ziel
(100k, 20 % sichtbar, unter 0,5 ms) ist erreicht, bei 500k nicht. `triangles` steigt um 27 bis 56 % (ganze Buckets werden eingereicht), die GPU-Zeit nicht.

### 11.5 Offen und Übergabe

- **Windows-Gleis (D3D11, D3D12, Vulkan):** nur die Textpatches sind drin und werden von der CI übersetzt, nichts davon lief auf einem Gerät. Offen: Velocity (TAA), Decal-Tiefe und SSR-Vorpass (Vulkan) entfalten Cluster
  noch nicht; die kumulativen Instanz-Ringe von D3D12 und Vulkan bleiben, der Cap-Schnitt verhindert nur, dass ein einzelner Batch den Ring sprengt.
- **GI:** Cluster sind überall aus der GI-Instanzliste; ein `giOccluder`-Opt-in (begrenzt nach Anzahl) fehlt.
- **Schritt 2b/2d:** `cachedInstances` und der Store doppelt im Speicher; Serialisierung der neuen Felder und die Schichten (`layers`) kommen mit 2d (`SceneSerializer.cpp` war ausdrücklich nicht Teil von 2a).
- **LOD (3a):** der Randbucket-Weg ist die Vorlage für Bänder: eine Stufengrenze durch einen Bucket wird wie die Kreislinie instanzweise aufgeteilt.
- **Picking:** ein Klick auf eine Pflanze trifft nur die erste je Bucket; ein Entfaltungsweg in `ScenePick` und `ViewportPick` fehlt.


## 12. Stand Teil 2b (Schritt 4, 10.10.2026)

Zwei Korrekturen aus 1.4 und Abschnitt 3, beide klein gehalten. Zweig wie in 11, davor `origin/release/0.7.0` erneut gemergt (neun Commits, nur Doku, `.gitignore` und
`LandscapeTexGen`).

### 12.1 GL: ein Graph-Material behält sein Programm

`OpenGLRenderer.cpp`, die beiden Farbpässe (G-Buffer-Schleife und Forward-Schleife). Der Instanzzweig stand vor dem Zweig für benutzerdefinierte Programme und nahm
jeden Batch ins eingebaute Instanzprogramm; ein Foliage-Layer mit Graph-Material wurde deshalb in flachem Grau-PBR gezeichnet. Jetzt gilt dieselbe Regel wie auf Metal
(`cMaterialPipeline == nullptr`):

- Der Instanzzweig nimmt nur Batches **ohne** Material-Programm (`!(gbProg || matProg)` im G-Buffer-Pass, `!matProg` im Forward-Pass). In der Forward-Schleife wird das
  Programm dazu vor der Entscheidung aufgelöst (vorher erst im `else`).
- Die drei Material-Zweige zeichnen den Batch **je Instanz** mit dem Programm des Materials: das G-Buffer-MRT-Programm (`drawGraphInstance`), der Forward-Zweig (ebenso)
  und der Rückfall ohne G-Buffer-Variante (`pushForward`, ein Eintrag je Instanz im `deferredForward`-Replay). Programm, Licht, Parameter und Texturen werden einmal je
  Batch gebunden, nur der Objektblock (`mvp`, `model`, Farbe) wird je Instanz neu geschrieben.
- Ein Material, dessen Programm nicht gebaut werden konnte (`matProg == 0`), fällt wie bisher aufs eingebaute Instanzprogramm zurück: vollständig, nur im Grau.
- Eingebautes PBR ist unberührt. Preis eines Graph-Material-Layers: **ein Draw je sichtbarer Pflanze** (wie Metal, D3D11 bis Vulkan unverändert in ihrem Zustand aus
  Abschnitt 3). Ein Layer mit 100k sichtbaren Pflanzen und Graph-Material braucht den instanzierten Material-Vertex (2e).

### 12.2 Scatter: der Layer folgt dem Boden (F1)

Ein Haken im Rebuild-Tor von `TerrainSystem::updateTerrains` (`TerrainSystem.cpp`, zwei Zeilen und ein Include): nach `tc.dirty = false; tc.regionDirty = false;` wird `FoliageComponent::dirty`
des Terrains gesetzt. Dort enden alle Wege, die den Boden ändern: Pinselstriche (Raise, Lower, Smooth, Flatten, Ramp, Roughen; der Pinsel ruft `updateTerrains` selbst
mit `regionDirty`), „Reset Sculpting“ und Größe, Auflösung oder Seed im Inspector (`dirty`), `TerrainSculpt::apply` aus MCP und Skripten, Mountain und Heightmap-Import.
`TerrainTools.cpp` bleibt unberührt (Thema 174 arbeitet dort). `SceneSystems` läuft Terrain (Zeile 113) vor Foliage (Zeile 143), der Layer wird im selben Tick neu gestreut.

Der Haken sitzt **im** Tor: der Material-Abgleich am Anfang des Ticks (läuft ungated) und ein Tick ohne Änderung streuen nicht neu (Negativkontrolle im Test).

**Kosten, bewusst nicht entschärft:** ein gehaltener Pinsel baut jeden Frame um und streut deshalb jeden Frame neu. Das wächst mit der Instanzzahl des Layers (grob: 6,5k Pflanzen
unter 1 ms, 100k im zweistelligen Millisekundenbereich, 500k um 100 ms; nicht gemessen, geschätzt aus der Scatter-Schleife). Der Nachfolger ist eine Entprellung wie
`g_pendingTerrainColliders` im selben File (neu streuen, wenn der Boden einen Tick ruhig war). Ebenso streuen Inspector-Änderungen neu, die `dirty` setzen, ohne die Höhen zu ändern
(UV-Kachelung, Tessellation, LOD-Abstand): dasselbe Ergebnis, nur die Kosten.

### 12.3 Prüfung

- `he_tests`: vier neue Fälle in `test_foliage.cpp` (Sculpt-Strich, Reset Sculpting, neue Größe, Negativkontrolle für Material-Wechsel und Leerlauf-Tick), dazu ein Quelltext-Guard
  in `test_foliage_cluster.cpp` für das GL-Tor (jeder Zweig, der einen Batch ins eingebaute Instanzprogramm schickt, nennt das Material-Programm in seiner Bedingung; beide Material-Zweige
  schleifen über die Instanzen). **Rot ohne Fix gezeigt:** ohne den Haken fallen die drei Verhaltensfälle (acht Prüfungen), die Negativkontrolle bleibt grün; der Guard fällt gegen den
  `HEAD`-Quelltext von `OpenGLRenderer.cpp` (vier Prüfungen). Voller `ctest` (Release, shaderc ON, `-j4 --timeout 1500`): 257 von 257 grün, 2 übersprungen (`runtime_size_app_*`), 291 s.
- **GL headless** (Zeuge `HE_DUMP_FOLIAGETEST=2000 HE_DUMP_FOLIAGESIZE=100`, neu `HE_DUMP_FOLIAGEMAT=graph`: ein Graph-Material aus einer ConstColor, Kamera `CAMX=0 CAMY=25 CAMZ=60 PITCH=-20`;
  die Kamera muss nach −Z auf das Feld schauen, sonst `visible=0`). Graph-Material, Forward und Deferred: `draws=1941` bzw. `1942` (vorher ein Draw je Batch), Pflanzen **magenta statt grau**,
  `glGetError=0x0`, 0 Fehler im Log. Eingebautes Material: `draws=5` (Forward) und `6` (Deferred), **Roh-BMP bitgleich zum Vorher-Binary** (Stand 2a), Forward und Deferred. Metal mit demselben
  Graph-Zeugen: `draws=1941`, mittlere Abweichung zu GL 0,051/255 (0,012 % der Bytes über 8, `HE_DUMP_SHADOW=0.1`).

### 12.4 Offen

- **Der Vorher-Lauf für das Graph-Material fehlt:** das Vorher-Binary kennt `HE_DUMP_FOLIAGEMAT` nicht. Dass der alte Weg grau gezeichnet hat, folgt aus dem Quelltext und aus dem Bild des
  eingebauten Materials (weiß-grau, dieselben Würfel), nicht aus einem eigenen Bild.
- F2 bis F10 aus 1.4 sind unverändert (F2 bis F4 sind für den Renderweg seit 2a erledigt, der Scatter selbst reagiert weiter nur auf `dirty`).
- D3D11, D3D12 und Vulkan: Graph-Material je Instanz mit ihren Grenzen aus Abschnitt 3 (1024 Material-Slots), nicht angefasst (Windows-Gleis, 2f).
- Entprellung des Neustreuens beim Pinsel (12.2). Die Schätzung dort ("500k um 100 ms") ist inzwischen gemessen: Abschnitt 13.3, das Streuen kostet seit 2a rund doppelt so viel.


## 13. Stand Verifikation (Schritt 5, 10.10.2026)

Zweig auf `00f72717`, `origin/release/0.7.0` enthalten (nichts Neues). Gemessen auf Metal, abwechselnd gegen den Klon von `08a8938b` (vor dem Cluster-Pfad). Zahlen, Bedingungen und Rohdaten:
`perf-audit/foliage-verification-2026-10-10.md` und `perf-audit/raw-foliage-verify/`. **Alle Millisekunden sind Zahlen unter Fremdlast** (Vollbau mit acht Compilern, Load 5 bis 14, in Teilen fremde GPU-Last und entsperrter Bildschirm);
belastbar sind die Verhältnisse je Paar.

### 13.1 Ziele aus Abschnitt 9 und 8, erreicht oder verfehlt

| Ziel | Ergebnis |
|---|---|
| Foliage-Extract unabhängig von der Gesamtzahl | **erreicht** für alles in Reichweite: `ExtractFoliage` 0,05 bis 0,08 ms bei 10k bis 1 Mio. (vorher 0,4 bis 74 ms); mit begrenzter Reichweite hängt er vom Randring ab (0,3 bis 1,4 ms) |
| 100k platziert, 20 % sichtbar, unter 0,5 ms | **erreicht** bei 100k (0,11 bis 0,33 ms), **verfehlt** bei 500k (0,6 bis 2,3 ms) und 1 Mio. (1,3 ms) |
| `draws` = Zahl der Mesh-Läufe | **erreicht**: 3 bis 8 statt bis 279 163 |
| Bild vorher gegen nachher | alter Pfad und `ordered` **bitgleich** (6 Fälle, Metal); Standardpfad 0,05 bis 0,4 % der Pixel anders (Tiefengleichstand, 11.3) |
| `Render` je Frame | 5 bis 6 mal niedriger (10k, 100k mit 100 m), 12 mal (100k "alle", 500k mit 100 m), 16 bis 27 mal (1 Mio., 500k "alle") |
| Voller `ctest`, `shaderc` ON | 257: 255 grün, 2 übersprungen (`runtime_size_app_*`), 0 rot; CI-Lauf 38074831163 auf `00f72717` grün (macOS, Windows, Linux, Linux Vulkan) |

### 13.2 Was nicht gemessen ist

OpenGL (nur Bilder aus 2a/2b), D3D11/D3D12/Vulkan (nichts davon lief je auf einem Gerät, nur Textpatches, von der CI übersetzt), Graph-Material-Layer, GI an, Kamera am Feldrand, die Stufen 1k/50k/200k, FPS-Läufe, jede
Messung ohne Fremdlast, die Ursache des Mehrspeichers bei 10k/100k "alle" in diesem Lauf (RSS schwankt zwischen Läufen um Faktor 1,7, nicht belastbar). 1 Mio. ist je ein Paar.

### 13.3 Neuer Befund: das Streuen ist teurer geworden

Zeuge `HE_DUMP_FOLIAGETEST`, Metal, drei abwechselnde Wiederholungen: 500k **69 gegen 139 ms**, 1 Mio. **105 gegen 187 ms** (Mediane vorher gegen nachher). Das ist die einmalige Last je Neustreuen (der Store mit den Buckets kommt zu `cachedInstances` hinzu),
kein Frame-Preis. Seit 2b zählt sie bei jeder Boden-Änderung: ein gehaltener Pinsel streut jeden Frame neu. Keine falsche Funktion, aber ein Preis, den 2a und 2b nicht genannt haben. Nachfolger: Entprellung (12.2) und
billigere Bucket-Sortierung. Nicht geändert.

### 13.4 Was in 0.7 ist und was bewusst nicht

- **In 0.7 (Metal und GL gelaufen):** Messzeug (2c), Cluster-Pfad mit Store, Buckets und Aufklappen (2a), GL behält das Graph-Material und der Scatter folgt dem Boden (2b). Die Hälfte des Themas "ein Batch statt ein Objekt je Pflanze" ist damit erfüllt.
- **Bewusst nicht in 0.7:** 2e (Graph-Material-Instancing, instanzierter Material-Vertex, Wind), 2f (Windows-Gleis: D3D11/D3D12/Vulkan auf einem Gerät, Velocity, Decal-Tiefe, SSR-Vorpass, Instanz-Ringe),
  3a (LOD-Stufen), 3b (Auto-LOD mit meshoptimizer, braucht eine Abhängigkeits-Entscheidung), 3c (Impostor). **Die zweite Hälfte des Themas, LOD und Impostor für ferne Vegetation, ist damit nicht erfüllt.**
- **Auch nicht gebaut, ohne dass es dafür einen Hive-Schritt gab:** 2d (Schichten: `layers`, Serialisierung der neuen Felder, Inspector), `giOccluder`, Entfaltung für Picking, `revision` im FrameKey (Thema 162).
- **Zur Roadmap (Entscheidung 6 in Abschnitt 10):** der Text "GPU-instanced foliage with wind" stimmt für 0.7 nicht (kein Wind, Graph-Material-Layer zeichnen je Pflanze). Die Korrektur braucht die Bestätigung des Menschen für den Deploy; nicht erfolgt.

## 14. Stand Schichten (Thema 182, Schritt 1 = Teil 2d, 17.10.2026)

Gebaut auf NN-WS03 (Windows, MSVC Release, Baum `C:/hw182`), Zweig `claude/foliage-2-wind-instancing-schichten-lod-windows-abnahme`.

### 14.1 Was gebaut ist

- **Datenmodell** (`FoliageComponent.h`): `FoliageLayer` trägt alles je Schicht (Name, eigener `visible`, Mesh, Material, Dichte, Seed, Skala, `drawDistance`, Maske,
  `bucketSize`, `castsShadow`, `contributesAO`, `shadowDistance`, dazu Laufzeit `dirty`, `cachedInstances`, `store`). Die Komponente behält `visible` (Zonen-Ausblenden),
  `dirty` (= alle Schichten, der Boden hat sich bewegt: Sculpt, Mountain, Heightmap; diese Aufrufer sind unverändert), `revision`, `settingsKey` und neu `activeLayer`
  (Editor-Zustand, nicht gespeichert, vor Gebrauch klemmen: `activeIndex()`). `layers` ist nie leer; `kMaxLayers = 32`; `addLayer()` vergibt Namen und einen freien Seed.
- **Streuen** (`FoliageSystem.cpp`): `scatterLayer` ist der alte Körper, je Schicht. Schicht 0 bleibt bitgleich (Test vergleicht gegen eine Einzelschicht).
  `settingsKeyOf` hasht alle Schichten und ihre Anzahl, also zählt Hinzufügen/Entfernen als Revision.
- **Extraktion** (`FoliageExtract.cpp`): eine innere Schleife über sichtbare Schichten, die Terrain-Pose einmal je Komponente. `FoliageExtractStats::layers` zählt jetzt Schichten.
- **Export/Preload** (`SceneSystems.cpp:262`): `collectAssetRefs` nennt Mesh und Material jeder Schicht (sonst fehlten im exportierten Spiel alle außer Schicht 0).
- **Format** (`SceneSerializer.cpp`, `foliageLayerToJson/FromJson`): eine Schicht = die alten flachen Schlüssel, neue Felder nur wenn nicht Standard, der
  Schicht-Schalter flach als `layerVisible` (denn `visible` war immer der Komponenten-Schalter). Ab zwei Schichten `"layers": [...]` mit `visible` je Schicht.
  Alte Szenen speichern damit byte-gleich (Test: Schlüsselmenge einer bemalten Standardschicht == alte Menge). Leere Liste lädt als eine Standardschicht, mehr als 32 werden gekappt.
- **Collab** (`TerrainSync.*`, `CollabSession.h`): Maske der Schicht i reist als Feld `16 + i` (`FoliageMask`, Feld 4 entfällt), Spiegel je Feld, ein Spiegel ohne
  Schicht wird geleert (eine später dort erscheinende Schicht geht ganz). `restoreFoliage` behält die empfangenen Einstellungen und übernimmt je Index nur Maske und
  noch gültige Streuung (vorher `n = move(o)`, das hätte eine fremde `castsShadow`-Änderung zurückgedreht, sobald sie serialisiert wird). Protokoll **17**.
  Reihenfolge: der Settings-Blob legt eine Schicht beim Peer an, also wartet die Maske einer Schicht, die der Peer noch nicht kennt (`Mirror::foliageLayers`), und eine
  geänderte Schichtzahl schickt die Settings außer der Reihe (sonst überholte die 33-ms-Maske den 200-ms-Blob, der Peer verwarf sie, der Spiegel hielt sie für angekommen:
  dauerhaft auseinander, z. B. nach Undo von Remove Layer; Test „a layer that arrives with its mask is not overtaken by the mask“).
- **Editor:** Inspector (`InspectorPanel.cpp`, Abschnitt Foliage): Schichtliste mit Sichtbarkeit, Add/Remove Layer (letzte bleibt), Name, Cast Shadows, Ambient Occlusion,
  Shadow Distance. Landscape-Tab Foliage (`TerrainTools.cpp`): Schicht-Wähler im Stil der Paint-Liste, `Add Foliage Layer##more`; Pinsel, Maskenauflösung, Erase Everywhere
  und Reset Mask wirken auf die aktive Schicht. Hilfe-Einträge in `EditorHelp.cpp`, `editor_help_audit.py --check` grün (1210/1210).

### 14.2 Prüfung

- `he_tests` je Datei (Release, scratch `APPDATA`): test_foliage 16, test_foliage_cluster 20, **test_foliage_layers 8 (neu)**, test_scene_serializer 46,
  test_terrain_sync 14 (4 neu), test_terrain_tools_ui 6 (zweite Schicht per Panel angelegt und bemalt, Schicht 0 unberührt), test_net_collab 114, test_net_lanbeacon 17,
  test_editor_help 19: alle grün.
- Voller `he_tests`-Lauf (Release): 5401 Fälle, 5397 grün, 4 rot, 12 übersprungen. Rot: zwei Fälle in `test_inspector_ui` (Zwischenablage, `GetClipboardText` = null,
  Light/Network-Komponente; auf NN-WS03 bekannt, siehe Memory „Pre-existing Vulkan/D3D/ctest noise“) und zwei in `test_profiler` (im Einzellauf 18/18 grün,
  also Reihenfolge/globaler Profiler-Zustand). Keiner berührt Foliage.
- `HorizonEditor` baut. **Nicht** von Hand im Editor angeklickt, kein Bild; die UI ist nur durch den headless UI-Test belegt.

## 15. Stand Graph-Material-Instancing (Thema 182, Schritt 2 = Teil 2e, 17.10.2026)

Gebaut auf NN-WS03 (Windows, MSVC Release, Baum `C:/hw182`, RTX 4070). W-B aus Abschnitt 3: ein Batch eines Graph-Materials (Foliage-Vorlage, Wind Sway am WPO-Pin)
zeichnet auf GL **einmal** mit dem Fragment des Materials und einem instanzierten Zwilling seines Vertex. Metal ist als Textpatch gebaut, hier weder übersetzt noch gelaufen.

### 15.1 Was gebaut ist

- **Shader-Bibliothek** (`MaterialShaderLibrary.cpp:71` `assembleVertex(..., instanced)`, `standardVertexInstanced` `:1102`, `customVertexInstanced` `:987`): `u.model`/`u.mvp`
  werden zu den Locals `heModel`/`heMvp`, am Anfang von `main()` je Instanz gelesen. Metal: `mat4`-Feld an Vertex-Buffer 5, **{mvp, model} je Instanz** (das `k_instStride`-Layout
  des Szenen-Instanzpfads), also dieselbe Rechnung wie der Einzel-Draw samt Transpose-Trick des WPO. GL/D3D/Vulkan: Modell aus den Attributen 4-7 (Divisor 1, liegt in jedem
  Mesh-VAO), `U.mvp` trägt `viewProj`. U-Block, `HeLighting` und Varyings bleiben die des einfachen Vertex, jedes Material-Fragment passt. Der einfache Text bleibt byte-gleich,
  eigene Cache-Slots.
- **OpenGL** (`OpenGLRenderer.cpp`): `GetOrBuildMaterialProgram(..., instanced)` `:3771` (eigener Programmschlüssel, folgt GLSL 4.10/4.30 wie das Material),
  `DrawGraphBatchInstanced` `:3952`. G-Buffer-Pass `:11815`/`:11964` und Forward-Pass `:12139`/`:12271` bauen den Zwilling nur für einen Batch; 0 lässt die Schleife.
- **Metal** (`MetalRenderer.mm`): `GetOrBuildMaterialPipeline(..., instanced)` `:12662` (Fragment-Cache geteilt, PSO-Slot eigen), Forward `:14675`, G-Buffer `:16516`: der
  Zwilling-PSO entsteht beim **ersten Batch** des Materials, `drawInstance` bekommt PSO, Instanzpuffer und Anzahl und macht alle bisherigen Binds (HeLighting/HeParams an 2/3,
  Texturen, Weightmap) plus Buffer 5 und `instanceCount`.
- **Schalter** `HE_GRAPH_INSTANCING=0` (`RenderSorter::graphInstancingEnabled`): Rückweg auf die Schleife im selben Binary (das A/B), für alle Backends gedacht.
- **Zeuge** `HE_DUMP_FOLIAGEMAT=wind` (`EditorApplication.cpp`, Foliage-Zeuge): Magenta-Graph plus Wind Sway am WPO-Pin. Offset = `windSpeed * 0.1 m` oben an einem 1-m-Biegeband;
  `HE_DUMP_WINDSPEED` und `HE_SKY_TIME` (die Uhr des Graphen) festnageln.
- **A/B-Skript** `scripts/perf/foliage_graph_ab.py` (ein Editor, instanziert gegen `HE_GRAPH_INSTANCING=0`, plus zwei Wind-Kontrollen über die Pflanzenmaske).

### 15.2 Was bewusst weiter schleift

- **Gebackene Pak-Varianten** (`MaterialShaderVariant`, exportiertes Spiel): kein instanzierter Vertex gebacken → `GetOrBuild...(instanced)` gibt 0/nullptr, die Schleife zeichnet.
  Das trifft den **Forward-Pfad**, und Forward ist der Standard des Spiels (`ProjectSettings::renderPath` = 0, `GameApplication.cpp:3574`): **im exportierten Spiel zeichnet
  Graph-Foliage im Standard weiter je Pflanze.** Der Deferred-Pfad instanziert dort dagegen, denn die G-Buffer-Variante wird nie gebacken, sondern immer zur Laufzeit übersetzt
  (außer im Stub-Flavour ohne Cross-Compiler, `app-advanced`/`app-basic`: dort weder G-Buffer-Variante noch Zwilling). Entscheidung bei der Queen (Anfrage 105): (a, empfohlen) `vertexInstanced`
  (+ `vertexInstancedClustered` für GL 4.30) backen wie `uiVertex`, mit HAsset-Tail-Regel und Alt-Pak-Test, passt zu Schritt 4; (b) nur den Vertex zur Laufzeit übersetzen.
- GL-Rückfall ohne G-Buffer-Variante (`deferredForward`-Replay), durchscheinende Batches (sortiert, `pushTP`), Metal-`forwardOpaque`-Umleitung: je Instanz wie bisher.
- Tiefen-/Schattenpässe: kein WPO (starrer Schatten, 7.7), unverändert.
- D3D11/D3D12/Vulkan: die Zwillinge übersetzen (HLSL `TEXCOORD4..7`, SPIR-V), sind aber **nicht verdrahtet**; ihre Input-Layouts brauchen den Instanzstrom (Schritt 4).
- Ein handgeschriebenes Fragment, das `u.model` liest, bekommt auf GL im Batch die Einheitsmatrix (Graph-Fragmente lesen `u.model` nicht).

### 15.3 Prüfung

- `he_tests --source-file=*test_graph_instancing.cpp`: 5 Fälle (Zwilling übersetzt auf Metal, GL 4.10/4.30, ES 3.0, HLSL, SPIR-V mit echtem Wind-Sway-Körper; Metal liest
  `[[buffer(5)]]`/`[[instance_id]]`; einfache Formen unberührt; GL-Blockgleichheit `HeLighting`/`U`; Quelltext-Guards der GL- und Metal-Zeichenstellen; Dump für die Offline-Compiler).
- `scripts/water_shader_offline_check.py` gibt die Zwillinge jetzt mit in `glslangValidator -l` (GL-Paare gelinkt), `spirv-val`, `fxc` und **`xcrun metal` auf dem macOS-CI-Runner**
  (der einzige echte Metal-Compiler, der den Metal-Teil je sieht).
- **GL-Zeuge** (2000 Pflanzen, 100-m-Feld, Kamera `CAMX=0 CAMY=25 CAMZ=60 PITCH=-20`, `SHADOW=0.1`, Wind 20, Uhr 3.0): draws **1941 → 5** (Forward) und **1942 → 6** (Deferred),
  gleiche 58 060 Dreiecke, `glGetError=0`, 0 Warnungen. Bild instanziert gegen Schleife **bitgleich** (0 von 921 600 Pixeln) für Wind und flachen Graph, Forward und Deferred,
  bei Uhr 3.0 und 7.3, mit kaltem (Programm gebaut) und warmem Programm-Cache (Programm aus `glprogcache` geladen). Kontrollen: Wind 0 gegen 20 verschiebt 20 % der
  Pflanzenpixel, Uhr 3.0 gegen 7.3 23 %; der Wind wirkt also und bleibt erhalten.
  `python scripts/perf/foliage_graph_ab.py --editor C:/hw182/deploy/Editor --rhi OpenGL` bestätigt das: WF/WD/GF/GD identisch, draws 5/1941 und 6/1942, beide Kontrollen bewegen die Pflanzen.
- `water_shader_offline_check.py --he-tests C:/hw182/tests/he_tests.exe` hier: 21 bestanden, 0 rot, Metal übersprungen (kein Xcode); die fünf neuen GL-Paare gelinkt, SPIR-V und HLSL der Zwillinge gültig.
- Voller `he_tests`-Lauf (Release, scratch `APPDATA`): 5406 Fälle, 5402 grün, 4 rot, 12 übersprungen; rot sind genau die vier aus 14.2 (`test_inspector_ui` Zwischenablage ×2,
  `test_profiler` ×2), keiner berührt Material oder Foliage.

### 15.4 Nicht geprüft

Metal (weder übersetzt noch gelaufen; Bitgleichheit dort folgt aus der identischen Rechnung, ist aber kein Bild); Zeitmessung (nur `draws`); exportiertes Spiel (nicht gelaufen;
Forward-Schleife und Deferred-Zwilling folgen aus dem Quelltext, 15.2);
der Editor von Hand; Linux-GL (CI übersetzt).

## 16. Stand LOD-Stufen (Thema 182, Schritt 3 = Teil 3a, 17.10.2026)

Gebaut auf NN-WS03 (Windows, MSVC Release, Baum `C:/hw182`, RTX 4070). D7 aus Abschnitt 6/7.6: handgemachte Stufen je Schicht, Ausdünnung, Hysterese,
Kreuz-Karte als Datei. Kein Backend geändert: eine Stufe ist nur ein weiteres Cluster-Objekt mit anderem Mesh/Material.

### 16.1 Was gebaut ist

- **Datenmodell** (`FoliageComponent.h:18`): `FoliageLod { meshAssetId, materialAssetId, distance, density }`, `FoliageLayer::lods` (höchstens `kMaxLods = 7`, `:71`)
  und `lodHysteresis` (Standard 2 m, `:76`). **Abweichung vom Plan 7.1:** `distance` ist der **Anfang** der Stufe, nicht `maxDistance`; das eigene Mesh der Schicht ist
  Stufe 0 vom Betrachter bis zur ersten Stufe, die letzte reicht bis `drawDistance` (bleibt das harte Ende). Leeres Mesh = das der Schicht (eine Stufe, die nur ausdünnt),
  leeres Material = das der **Schicht** (nicht das des Stufen-Meshs). `density` 0..1, 0 = nichts.
- **Extraktion** (`FoliageExtract.cpp`): `levelsOf` `:253` sortiert die Stufen je Frame (der Inspector darf sie beim Ziehen umordnen), `Levels::plain/choose/whole`
  `:205-250`. Je Bucket: liegt er in Reichweite **und** frei von jedem Band (Grenze ± Hysterese, mit dem `kRangeMargin` der Reichweite), ist er **ein** Cluster seiner Stufe,
  ohne Kopie (Ausdünnung = Präfix). Sonst (Reichweite oder ein Band schneidet ihn) Pflanze für Pflanze, je Stufe ein Cluster über den Scratch (`:560`). Die Box je Cluster
  nimmt die Reichweite des **Stufen-Meshs** (`clusterBounds(b, level)`). Eine Schicht ohne Stufen läuft im alten Code (eigener Zweig, Ausgabe gleich).
- **Ausdünnung** (`keptOf` `:186`): die ersten `round(density · count)` Pflanzen jedes Buckets, **ein** Ausdruck für den Ganz- und den Einzelweg, sonst wechselten Pflanzen,
  sobald ein Bucket den Weg wechselt. Gleichmäßig, weil das Streuen gleichverteilt zufällig zieht (`FoliageSystem.cpp` `scatterLayer`, Test „evenly“).
- **Hysterese** (`FoliageExtract.h:27` `FoliageLodMemory`): **je Ansicht**, weil `RenderExtractor` eine Ansicht ist (Szene, Vorschau): `RenderExtractor.h:332`
  `m_foliageLod`, angelegt in `RenderExtractor.cpp:1424`. Je Store ein Byte je Bucket (Stufe des ganzen Buckets / gemischt / unbekannt), ein Byte je Pflanze erst, wenn eine
  Grenze einen Bucket geteilt hat; wechselt ein Bucket von ganz zu geteilt, erben seine Pflanzen die Stufe des Buckets. Eine Pflanze behält ihre Nachbarstufe, solange sie
  höchstens `lodHysteresis` hinter der Grenze liegt; an `drawDistance` keine Hysterese. Einträge eines Stores, der im Frame nicht extrahiert wurde, fallen am Ende weg
  (`:649`; ein gehaltener Sculpt-Pinsel streut jeden Frame neu). `extractFoliage(..., nullptr)` = ohne Gedächtnis = nach Abstand.
- **PerInstance / Ordered** nehmen dieselbe Stufen- und Ausdünnungsregel nach Abstand (ohne Hysterese); Ordered bildet je Mesh+Material ein Cluster in Sortierreihenfolge.
- **Zähler** (`FoliageExtractStats`): `levelInstances[8]`, `thinnedInstances`, `heldInstances`; die `HE_FOLIAGE_STATS`-Zeile endet jetzt auf `per level a/b/c, N thinned, M held`.
- **Format** (`SceneSerializer.cpp:184`, `:221`): `lods` und `lodHysteresis` nur, wenn gesetzt (eine Schicht ohne Stufen schreibt die alten Schlüssel, byte-gleich), in beiden
  Formen (flach und `layers`). Laden sortiert, kappt bei `kMaxLods`, klemmt `density` auf 0..1 und `distance` auf ≥ 0. `settingsKeyOf` (Revision) und `collectAssetRefs`
  (Export/Preload) nennen Mesh und Material jeder Stufe.
- **Editor** (`InspectorPanel.cpp:3572`): „Detail Levels“ unter Shadow Distance: je Stufe Mesh- und Material-Slot, `Level Distance`, `Level Density`, `Cross Card`
  (setzt die Engine-Karte **und macht sie resident**: per UUID, wenn der Engine-Ordner gescannt ist, sonst per Pfad; sonst eine Warnung), `Remove Level`; `Add Level` (halbe Strecke bis `drawDistance`), `Level Hysteresis`. Hinweis „never drawn“ bei Anfang ≥ `drawDistance`.
  Hilfe in `EditorHelp.cpp`, `editor_help_audit.py --check` 1216/1216.
- **Kreuz-Karte als Datei** (`mesh_gen`, `MeshGen/main.cpp:143`): `EditorDeps/EngineContent/Meshes/CrossCard.hasset`, Eintrag 8 = `kEngineCrossCardMeshId` `0x108`
  (`DefaultAssets.h:104`, `static_assert` im Generator). Zwei aufrechte 1×1-Quads über Kreuz, **beide Seiten** je Ebene (16 Vertices, 8 Dreiecke), Drehpunkt **unten**
  (y 0..1, anders als die zentrierten Primitive), volle UV 0..1 je Fläche. Die Textur (Bild der Pflanze) ist Sache des Materials; das Backen ist Teil 3c.
- **Zeuge** `HE_DUMP_FOLIAGELOD=<ab>[:cube|sphere|card[:dichte]],...` (`EditorApplication.cpp:7565`, im Foliage-Zeugen; lädt Kugel und Karte als Engine-Dateien und loggt,
  ob die Karte mit ihrer UUID kam) und `scripts/perf/foliage_lod_shot.py` (keine Stufen / Stufen / ordered / je Pflanze, ein Binary).

### 16.2 Prüfung

- `tests/test_foliage_lod.cpp`, 10 Fälle: Mesh je Pflanze nach Abstand in allen drei Modi gleich der Testkopie der Regel (2 Grenzen + Reichweite, ganze und geteilte Buckets),
  Ausdünnung gleichmäßig (Anteil und Quadranten), derselbe Bucket ganz vs. bei vier Schnitttiefen geteilt behält dieselben Pflanzen, Hysterese (gehaltene Pflanzen genau die
  im Band, Zähler stimmt, zweite Ansicht ohne Gedächtnis, weit weg wieder nach Abstand), Gedächtnis folgt den Stores (5× neu streuen, verbergen, Stufen weg), Cluster-Box
  hält das Stufen-Mesh an jeder Pflanze, Revision, Format (flach, `layers`, Handarbeit sortiert/geklemmt/gekappt), Export-Refs, die Karten-Datei (UUID, Maße, Wicklung,
  Normalen, UV). **Negativkontrolle:** Hysterese abgeschaltet und Ausdünnung im Einzelweg um eins verschoben → 4 Fälle rot (der Ganz/geteilt-Fall erst, seit er vier
  Schnitttiefen nimmt).
- `test_inspector_ui` „the Foliage section adds a detail level and gives it the cross card“: Add Level und Cross Card per Klick, je ein Undo-Schritt, beide zurück;
  das BMP (`HE_UI_DUMP_DIR`) zeigt die Zeilen sauber; nach dem Klick ist die Karte im ContentManager resident. Der Karten-Datei-Fall prüft außerdem, dass eine
  Szene, die die Karte nur per UUID nennt, sie nach `scanContentDirectory` über `preloadAssetRefs` bekommt (wiedergeöffnete Szene).
- Voller `he_tests` (Release, scratch `APPDATA`): 5417 Fälle, 5413 grün, 4 rot, 12 übersprungen; rot genau die vier aus 14.2/15.3 (`test_inspector_ui` Zwischenablage ×2,
  `test_profiler` ×2), keiner berührt Foliage.
- Je Datei grün: test_foliage 16, test_foliage_cluster 20, test_foliage_layers 8, test_scene_serializer 46, test_terrain_sync 14, test_graph_instancing 5, test_editor_help 19, test_lod 11.
- **Zeuge auf echter Hardware** (6000 Pflanzen, 200-m-Feld, `drawDistance` 90, Stufen `25:sphere,50:card:0.5`, Kamera `CAMX=0 CAMY=6 CAMZ=60 PITCH=-6`): GL, D3D11 und Vulkan
  zählen gleich, **296/840/866** je Stufe, 879 ausgedünnt; 41 Cluster, 7 Draws (ohne Stufen 29 Cluster, 5 Draws); Karte „loaded with its engine UUID“. Bild (nur Boden,
  der Himmel rauscht auf D3D/Vulkan von Lauf zu Lauf): ordered == je Pflanze **pixelgleich** auf allen drei; Wiederholung desselben Laufs 0 Pixel; Cluster gegen ordered
  GL 10 Pixel, D3D11 0,13 %, Vulkan 0,25 % (fast alle ≤ 8 Stufen). **Dieselbe Abweichung besteht ohne Stufen** (D3D11 0,21 %, Vulkan 0,51 %): sie gehört zum Cluster-Pfad
  auf D3D/Vulkan, nicht zu den Stufen, und ist ein Fall für Schritt 4.
- Mesh-Generator: ein Neulauf schreibt auch die acht alten Dateien um, aber nur weil der heutige Schreiber den META-Block 4 Byte länger macht (Geometrie gleich); die alten
  Dateien sind zurückgesetzt, nur `CrossCard.hasset` ist neu.

### 16.3 Nicht gebaut, nicht geprüft

Dither-Überblendung (braucht ein Instanzfeld, 7.6), Bildschirmgröße statt Abstand, Hysterese/Bildschirmgröße im `LODSystem` der Entities, Voreinstellungen
(Gras/Busch/Baum/Fels), das Backen der Kartentextur (3c), Auto-LOD (3b). Cutout der Karte im Schattenpass (7.7) bleibt offen: eine Karte mit Alpha wirft ein volles
Rechteck, also Stufen mit Karte besser ohne Schatten oder mit `shadowDistance` davor. Nicht geprüft: Metal (kein Mac), der Editor von Hand (nur der headless UI-Test
aus 16.2), das exportierte Spiel, Zeitmessung.

## 17. Stand Windows-Gleis (Thema 182, Schritt 4 = Teil 2f, 17.10.2026)

Gebaut und gelaufen auf NN-WS03 (Windows, MSVC Release, Baum `C:/hw182`, RTX 4070), D3D11, D3D12 und Vulkan. Drei Commits: `28845a11` (Graph-Material-Zwilling),
`6154b254` (Instanz-Arena), `da8cd627` (Cluster in Pro-Objekt-Pässen). Metal unberührt.

### 17.1 Was gebaut ist

- **Graph-Material-Instancing (2e auf Windows):** `GetOrBuildMaterialShaders/PSO/Pipeline(..., instanced)` (D3D11 `:4698`, D3D12 `:9117`, Vulkan `:3082`) bauen den
  Zwilling aus `customVertexInstanced`/`standardVertexInstanced`, das Modell an Location 4..7 (`TEXCOORD4..7`) aus Vertexpuffer 1. D3D12/Vulkan binden dafür den
  128-Byte-Instanzring `{mvp, model}` (Modell bei +64), D3D11 einen eigenen dynamischen 64-Byte-Vertexpuffer (`ensureMatInstVB` `:1403`; der `instanceSB` ist ein
  StructuredBuffer und darf kein Vertexpuffer sein). Gebaut nur für einen Batch im opaken und im G-Buffer-Pass (`wantTwin` D3D11 `:7092`, D3D12 `:11119`, Vulkan `:8441`),
  ein U/HeParams-Slot je Batch (`U.mvp = viewProj`). **Eigener Schlüssel in jedem Cache, vor dem ersten Nachschlagen gesalzen:** der Bytecode-Cache von D3D12 und der
  Shader-Cache von D3D11 hängen nur am Shader-Hash, der einfache Vertex hinter dem instanzierten Input-Layout baut ohne Klage und stapelt alle Pflanzen auf einen Fleck.
  Gebackene Pak-Variante, `HE_GRAPH_INSTANCING=0`, durchscheinend oder Ring voll: die Schleife wie bisher.
- **Instanz-Arena (D3D12, Vulkan):** `allocInstances`/`beginInstanceFrame` (D3D12 `:3432`/`:3460`, Vulkan `:6940`/`:6971`). Jeder instanzierte Lauf eines Frames
  (jede Kaskade, SSAO-, GI-Vorpass, Szene) nimmt eigene Plätze; 100k Pflanzen verlangen rund 460k. Bisher fiel alles über 65 536 auf die Pro-Objekt-Schleife zurück. Jetzt
  öffnet ein voller Block einen weiteren, doppelt so großen (lebt bis zum Fence seines Frames; Vulkan altert ihn in `m_instRetired` aus wie die Viewport-Bilder), und der
  nächste Frame desselben Slots ersetzt den Hauptpuffer durch einen der Summengröße (einmal je Slot geloggt: „grows from 65536 to 524288 instances“). Grenze
  `k_instArenaMax` = 1 Mio. Instanzen je Frame-Slot, darüber Schleife plus Meldung. D3D12 meldet außerdem einmal, wenn der Pro-Objekt-Ring (`k_maxDraws` 16 384) endet
  (`noteDrawRingFull` `:3497`), statt den Frame still abzuschneiden.
- **Cluster in Pro-Objekt-Pässen (die 2a-Guards, zur Laufzeit geprüft):** TAA-Velocity auf allen drei zeichnet jede Pflanze statisch (vorher = jetzt), wie GL/Metal
  (D3D11 `:1908`, D3D12 `:11797`, Vulkan `:5723`). D3D12 nimmt dafür Pro-Objekt-Ringplätze, und der durchscheinende Pass zeichnet **danach** aus demselben Ring:
  die Velocity hört bei `k_maxDraws` minus den Plätzen des Blend-Passes (+ 64) auf (`velLimit`), weitere Pflanzen zeigen keine Bewegung (einmal gemeldet). Vulkan-Decal-Tiefe nimmt den Cluster als einen Draw über den instanzierten Schatten-Zwilling (`:3986`). Vulkans
  SSAO/SSR-Reflexionsvorpass (MRT-Weg, läuft sobald SSR an ist) übersprang Cluster ganz; jetzt ein neues, optionales instanziertes Pipeline-Paar aus
  `reflPrepassVertexInstanced` (`:13021`, ohne UV-Attribut, also keine neue Validierungswarnung), sonst je Pflanze. D3D11/D3D12 speisen diesen Vorpass aus den
  `DrawCall`s des GeometryPass und waren nie betroffen.

### 17.2 Prüfung

- **Graph-A/B** (`foliage_graph_ab.py`, neu `--ground/--tol/--plants/--mask-tol`, vergleicht Boden, Pflanzenmaske und Dreiecke; der Himmel rauscht auf D3D/Vulkan):
  800 Pflanzen, draws **774 → 5** (Forward) und **775 → 6** (Deferred) auf allen drei, gleiche Dreiecke; Boden und Maske identisch auf D3D11/D3D12, Vulkan 0-4
  Maskenpixel = sein eigenes Lauf-zu-Lauf-Rauschen desselben Modus. Wind 0 gegen 20 bewegt ~22 % der Pflanzenpixel.
- **Budget, vorher gemessen:** 2000 Graph-Pflanzen zeichneten auf D3D12/Vulkan nur **47 104 von 58 060** Dreiecken (`k_matMaxDraws` = 1024 Graph-Draws je Frame,
  Rest übersprungen); jetzt 58 060 in 5 Draws. **100k eingebaute Pflanzen mit Schatten: D3D12 zeichnete nichts außer Himmel und Grau** (Schattenpass leerte den
  Pro-Objekt-Ring), Vulkan richtig aber mit 56 104 Draws. Jetzt D3D12 vollständig (8 Settle-Frames: Bild nach dem Wachsen = Bild des Blockweges, Boden identisch),
  Vulkan 5 Draws / 708 016 Dreiecke wie D3D11 und GL, sein Bild gleich der alten (vollständigen) Schleife im Rauschen.
- **TAA-Velocity:** 6000 Pflanzen, AA=3, ein Frame Kamerabewegung (`HE_DUMP_MBYAWSTEP=2`) gegen ruhig, Boden um > 24/255: D3D11 2,98 %, D3D12 2,45 %, Vulkan 2,41 %,
  GL 2,0 %. **Negativkontrolle** (Entfaltung im Bau abgeschaltet): D3D11 6,25 %, Vulkan 6,24 %.
- **Velocity-Reserve (D3D12):** 100k Pflanzen, AA=3, Wasser-Zeuge (`HE_DUMP_WATERTEST=1`): Wasser an gegen aus 0,54 % (AA=0: 0,48 %), Meldung „TAA velocity
  stops at 16320“. **Negativkontrolle** ohne Reserve: „the rest of the frame is not drawn“, Wasser an gegen aus nur noch 0,26 % — das Wasser fiel weg.
- **Rauchlauf GI+SSR+TAA** auf allen drei, 6000 Pflanzen, Cluster und je Pflanze: keine neue Warnung, kein Fehler (Vulkan: die eine bekannte Validierungszeile
  „location 2 not consumed“ in beiden Modi). Decal-Zeuge Cluster gegen je Pflanze: 17 px (D3D11/D3D12), 96 px (Vulkan) über Toleranz 8. Reflexionsvorpass-Zeuge:
  „reflection pre-pass instanced (first cluster: 39 plants)“.
- **Tests:** `test_graph_instancing.cpp` 7 Fälle (Quelltext-Guards der drei Zeichenstellen, Salz vor dem ersten Cache-Zugriff, Arena ohne festen Kapazitätstest),
  `test_foliage_cluster.cpp` 21 Fälle (kein Backend überspringt einen Cluster mehr in einem Pro-Objekt-Pass). Jede neue Prüfung mit Negativkontrolle rot gesehen.
  Voller `he_tests` (Release, scratch `APPDATA`): 5421 Fälle, 5417 grün, 4 rot, 12 übersprungen; rot genau die vier aus 14.2-16.2 (`test_inspector_ui`
  Zwischenablage ×2, `test_profiler` ×2).
- **Request 109 (Queen):** `scripts/runtime_size.py` Linux-Spielzeile (64, 38) → (67, 42) MB (`21096c3e`), gemessen in CI 39033396153 auf `2f0331ce`:
  60,8 MB gesamt, 38,0 MB ohne Python, nach der Regel der Datei plus 10 %; dieselben Zahlen wie Thema 183 (`d1dbbd3e`), beim Mergen also kein inhaltlicher Konflikt.

### 17.3 Befund: Cluster gegen ordered auf D3D/Vulkan ist der SSAO-Vorpass

Die Abweichung aus 16.2 (D3D11 0,21 %, Vulkan 0,51 % des Bodens, ohne Stufen) verschwindet mit `HE_DUMP_SSAO=0` vollständig (Bloom und Schatten ändern nichts).
Ursache: der SSAO-Positionsvorpass von D3D11, D3D12 und Vulkan schreibt in eine **D16**-Tiefe (GL: 24 Bit). Auf 50-90 m fallen Würfel und Boden auf gleiche
16-Bit-Werte, und die Zeichenreihenfolge entscheidet (die Tiefengleichstand-Lektion, nur viel häufiger). Versuch: D3D11 mit `DXGI_FORMAT_D32_FLOAT` dort → Cluster
gegen ordered **identisch**. Nicht übernommen (ändert das SSAO-Bild jeder Szene auf drei Backends), Vorschlag an die Queen.

### 17.4 Nicht gebaut, nicht geprüft

- 64-Byte-Schattenstride nicht gebaut: die Arena macht ihn fürs Funktionieren unnötig, er würde nur den Speicher halbieren (bei 100k Pflanzen 2 × 64 MiB je Slot).
- `k_matMaxDraws` (1024 Graph-Draws je Frame, D3D12/Vulkan) bleibt für nicht gebatchte Graph-Objekte; mehr als 1024 verschiedene sichtbare Graph-Objekte verlieren
  weiter den Rest (einmal gemeldet). Batches brauchen jetzt einen Slot.
- Die `dump counters` von D3D12 zählen die Schattenkaskaden mit (vorbestehend), Dreiecksvergleiche über Backends deshalb mit `SHADOW=0.1`.
- Zeit nicht gemessen (Schritt 5). Metal nicht berührt. Exportiertes Spiel: gebackene Varianten schleifen weiter (Anfrage 105). SSR-Bild-A/B auf Vulkan nicht
  aussagekräftig (SSR rauscht 1,6 % von Lauf zu Lauf), dort nur der Zeuge. Editor von Hand nicht.

## 18. Stand Verifikation (Thema 182, Schritt 5, 17.10.2026)

Gemessen auf NN-WS03 (RTX 4070) gegen einen Build des Merge-Basis-Commits `1e41c2d2` (vor Thema 182), GL, D3D11, D3D12 und Vulkan. Zahlen, Bedingungen und Bilder:
`perf-audit/foliage-2-verification-2026-10-17.md`, Rohdaten `perf-audit/raw-foliage-2-verify/`. Neu: `he_perf_capture.py --rhi` (Windows), `scripts/perf/foliage_ladder_win.py`
(Leiter, abwechselnd, `--table`), `scripts/perf/foliage_backend_ab.py` (Bild vorher/nachher mit Rauschboden, sechs Fälle).

- **Eingebaute Foliage, Reichweite 100 m und alles bis 100k:** GL, D3D11 unverändert (Zeit, draws, Dreiecke, Bild identisch). D3D12: 100k "alle" und 500k/100 m
  waren vorher **unvollständig** (16 385 Draws, Bild nur Himmel/Grau), jetzt vollständig in 19-20 Draws. Vulkan: 100k "alle" **271,6 → 4,4 ms** `Render` (61x),
  500k/100 m **410,8 → 5,4 ms** (77x); vorher lief die Pro-Objekt-Schleife.
- **Graph-Material (800 Pflanzen):** 774 → 5 Draws auf allen vier, Bild identisch (GL) bzw. Boden und Pflanzenmaske identisch (D3D/Vulkan). Zeit nicht gemessen.
- **Neuer Befund, offen: 500k "alle in Reichweite" sprengt die Arena** (`k_instArenaMax` 1 M je Frame-Slot; Schatten-Kaskaden + SSAO + Szene brauchen mehr):
  D3D12 bleibt unvollständig (Draw-Ring leer, gemeldet), Vulkan fällt auf die Schleife zurück (290 statt 1 600 ms, also nur 5,5x). Kandidaten: Arena-Grenze anheben
  oder je Pass zurücksetzen, 64-Byte-Schattenstride (17.4), Ausdünnung in der Ferne (Stufen aus 3a); nicht gebaut.
- **D3D11 500k "alle":** je Lauf entweder ≈18 oder ≈24 ms, in **beiden** Binaries (5 Läufe je Seite); keine Regression belegbar.
- **Voller ctest:** 297 grün, 2 übersprungen, 1 rot (`test_inspector_ui` Zwischenablage, vorbestehend). Fehlender Temp-Ordner je Test täuschte drei rote vor (Lektion 378).
- **Runtime flavours rot (Lauf 39037311399):** Linux app-advanced/app-basic je 0,1 MB über der Schwelle, allgemeine Drift (Thema 183 ebenso); die Queen hebt die
  Zeilen auf release/0.7.0 an (Anfrage 111), hier nicht geändert.
- **Nicht gemessen:** Metal, Graph-Material-Zeit, GI an, Kamera am Rand, 1 Mio., exportiertes Spiel.

## 19. SSAO-Positionsvorpass D16 → D32 (Thema 182, Schritt 6, Entscheidung 110, 17.10.2026)

Der Positionsvorpass für SSAO (mit SSR an auch der Reflexions-MRT-Vorpass, der dieselbe Tiefe bindet) hatte auf D3D11, D3D12 und Vulkan eine eigene
**D16**-Tiefe (GL 24 Bit, Metal 32). Jetzt D32, je Backend eine Konstante `kSsaoPosDepthFormat`, die jede Stelle nennt (Muster wie Thema 204 im GI-G-Buffer):
D3D11 `D3D11Renderer.cpp:1971` (Textur `:2435`, die DSV erbt das Format), D3D12 `D3D12Renderer.cpp:4540` (Textur + Clear-Wert `:5949`, DSV `:5967`,
`ssaoPosPSO`/`Instanced` `:5598`, `reflPrepassPSO` `:6235`), Vulkan `VulkanRenderer.cpp:12101` (Bild `:12754`, `m_ssaoPosRenderPass` `:12413`,
`m_reflPrepassRP` `:13208`). Die GI-G-Buffer-Stellen (D3D11 `:3441`, D3D12 `:7393/7594`) sind hier absichtlich **nicht** angefasst: Thema 204 (`520f1453`)
stellt sie auf seinem Branch um, beide zusammen auf release/0.7.0 ergeben keinen Konflikt.

Gemessen auf NN-WS03 (RTX 4070), vorher = Kopie des Deploys von `d31aa6a7` (`C:/hw182/deploy_before`), nachher = derselbe Baum mit der Änderung:

| Zeuge (Boden ab Zeile 330) | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| Cluster gegen ordered, ohne Stufen (`foliage_lod_shot.py --levels ""`, Nacht wie 16.2), vorher | 0,209 % | 0,209 % | 0,512 % |
| dasselbe, nachher | **identisch** | **identisch** | **identisch** |
| Cluster gegen ordered, Stufen `25:sphere,50:card:0.5`, vorher | 0,125 % | 0,125 % | 0,254 % |
| dasselbe, nachher | **identisch** | **identisch** | 31 px (0,006 %) |
| Rauschboden (zwei gleiche Läufe) | 0 px | 0 px | 0 px |

Die 31 px auf Vulkan mit Stufen sind deterministisch (Lauf 1 = Lauf 2) und **nicht** der SSAO-Vorpass: mit `HE_DUMP_SSAO=0` bleiben 22 px, vorher wie nachher
gleich. Das ist ein Tiefengleichstand im Szenenpass selbst (Kreuzkarten = sich schneidende Quads, Haupttiefe schon D32; GL hat dort 10 px, 16.2).

**Wie stark ändert sich das SSAO-Bild jeder Szene?** (`scripts/perf/ssao_depth_ab.py`, Tag `TOD=0.4`, AA/SSR/Wolken aus, vorher gegen nachher, nachher zweimal als Rauschboden):

| Fall | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| FC 6000 Pflanzen, Cluster | 2,23 %, Stufe ≤ 9 | 2,23 %, ≤ 9 | 5,43 %, ≤ 24 |
| FO dasselbe, ordered | 2,18 %, ≤ 11 | 2,18 %, ≤ 11 | 5,34 %, ≤ 24 |
| MT Berglandschaft, Kamera 200 m entfernt | 0,06 %, ≤ 1 | 0,16 %, ≤ 2 | 1,60 %, ≤ 5 |
| M0 = MT mit SSAO aus (Kontrolle) | identisch | identisch | identisch |
| SR = FC mit SSR an (Reflexions-MRT-Vorpass) | 2,54 %, ≤ 9 | 2,54 %, ≤ 9 | 5,82 %, ≤ 24 |
| Rauschboden (nachher gegen nachher) | 0 px | 0 px | 0-1 px, ≤ 1 |

Die Änderung liegt nur im fernen Band am Horizont (Differenzbild ×25), nichts nahe der Kamera; auf Vulkan halten sich dunkler (13 250 px) und heller (13 863 px)
die Waage (Mittel +0,85/765), also kein AO-Verlust, sondern genauere Tiefe. Mit dem Auge sind vorher und nachher nicht zu unterscheiden (Ausschnitt angesehen).
Vulkan ändert sich stärker, weil sein D16 schon vorher der gröbere war (0,512 % gegen 0,209 %). Nach Cluster gegen ordered ist FC == FO jetzt auf allen drei.

- **Rauchlauf:** Vulkan-Validierung an („validation layer ENABLED“): vorher wie nachher nur die bekannte Zeile „location 2 not consumed“, der Reflexionsvorpass lief
  („reflection pre-pass instanced“). D3D11/D3D12-Logs ohne ERROR/WARN (außer „No config file“ beim ersten Lauf).
- **Test:** `test_foliage_cluster.cpp` „The SSAO position pre-pass depth is D32 on D3D11, D3D12 and Vulkan, at every site“ (Quelltext-Guard: Definition D32 und
  jede Stelle nennt die Konstante). Negativkontrolle: D3D11-Definition auf D16 und Vulkans `atts[3]` auf das Literal → rot (2 Fehler), zurück → grün.
- **Speicher:** die Vorpasstiefe wächst von 2 auf 4 Byte je Pixel der SSAO-Auflösung (bei 640×360 ~0,5 MB mehr).
- **Nicht geprüft:** Metal und GL (nicht betroffen, nicht angefasst); exportiertes Spiel (gleicher Renderer-Code); GI an; Editor von Hand.

## 20. Instanz-Arena bei 500k Pflanzen in Reichweite (Thema 182, Schritt 8, 17.10.2026)

Ursache des Befunds aus 18: die Arena zählte **Kapazitäten** der sich verdoppelnden Chunks gegen 1 M je Slot, nicht belegte Slots.
64k + 128k + 256k + 512k = 983 040, der nächste verdoppelte Chunk (1 M) passte nie, obwohl der Frame nur rund 1,1 M brauchte (Vulkan zeichnete danach noch ~107k
Pro-Objekt-Draws, D3D12 leerte den `k_maxDraws`-Ring und zeichnete keine Szene). Das Hauptpuffer-Wachstum verdoppelte bis zur selben Grenze, also jeden Frame wieder.

Jetzt: Buchführung in `HorizonRendering/InstanceArena.h` (ohne GPU, Muster `FrameUploadRing.h`), D3D12 (`D3D12Renderer.cpp` `beginInstanceFrame`/`allocInstances`
um `:3409`) und Vulkan (`VulkanRenderer.cpp:6940`, Member in `VulkanRenderer.h:386`) rufen sie, die Puffer bleiben beim Backend.
- Der letzte Chunk wird auf den Rest des Budgets **gekappt** statt abgelehnt; Ablehnung nur, wenn auch der Rest den Lauf nicht fasst.
- Der Hauptpuffer jedes Slots wächst auf das, was der größte Frame **verlangt** hat (genommen + abgelehnt, + 1/8, auf 64k gerundet), nicht auf die
  verdoppelte Kapazitätssumme. Bei 500k "alle" (Nachbau im Test: 3 Kaskaden mit je 12 Buckets, SSAO und Szene mit allen 169) sind das 1 245 184 Instanzen
  = 152 MiB je Slot; danach öffnet kein Frame mehr einen Chunk.
- Budget **512 MiB je Slot** (4 M Paare à 128 B, reicht für etwa 1,5 bis 2 M Pflanzen in Reichweite), `HE_INST_ARENA_MIB=<MiB>` überschreibt es für einen Lauf.
  Speicher folgt dem Bedarf, nicht dem Budget. D3D12 hat 3 Slots: bei 500k "alle" etwa 3 × 152 MiB Upload-Heap im eingeschwungenen Zustand.
- Die Warnung nennt jetzt den ganzen Frame: „instance arena full — a frame asked N instances, R drew per object (budget B …)“ (einmal, beim nächsten
  Frame-Anfang). Wachstum weiter als „instance ring/buffer of frame slot … grows from … to …“.

Geprüft auf dem Mac (kein D3D12-Compiler, keine Windows-GPU): `tests/test_instance_arena.cpp` (5 Fälle; Negativkontrolle = die alte Arithmetik
nachgebaut, sie lehnt bei 983 040 in jedem Frame ab), Guard in `test_graph_instancing.cpp` angepasst (kein `k_instArenaMax`, beide Backends rufen die Arena),
Vulkan `-fsyntax-only` gegen MoltenVK mit Negativkontrolle. D3D12 kompiliert nur der Windows-Job der CI. **Nicht gemessen:** die Leiter 500k "alle" auf
D3D12/Vulkan (NN-WS03); erwartet ~20 Draws, keine „arena full“-Zeile, je Slot eine „grows from 65536 to …“-Zeile, RSS mitschreiben (vorher +720 MB).

Strukturelle Folge, nicht gebaut: jeder Pass schreibt {clip·model, model} für jede Pflanze neu (128 B × Pässe). Ein gemeinsamer 64-B-Modellpuffer je Frame
plus clip je Pass im Konstantenpuffer würde den Bedarf um Faktor 6 bis 12 senken, braucht aber neue instanzierte Shader und PSOs auf D3D12/Vulkan.

## Handoff

- **Topic / step:** Thema 182, Schritt 8 (Arena-Überlauf bei 500k Pflanzen in Reichweite auf D3D12/Vulkan); Schritte 1-6 in 14-19, dieser in Abschnitt 20
- **Branch / commit:** `claude/foliage-2-wind-instancing-schichten-lod-windows-abnahme`, Commit dieses Schritts (siehe `git log`), gepusht
- **Done:** Die Arena zählt belegte statt reservierte Slots, kappt den letzten Chunk auf das Budget und dimensioniert den Hauptpuffer je Slot nach dem
  verlangten Bedarf; Budget 512 MiB je Slot (`HE_INST_ARENA_MIB`). Im Nachbau passt 500k "alle" (1,1 M Slots) ab dem zweiten Frame ohne Chunk.
- **Files touched:** `src/HE_Rendering/include/HorizonRendering/InstanceArena.h` (neu, die Buchführung), `D3D12Renderer.cpp` (Arena-Block um `:3409`),
  `VulkanRenderer.cpp:6940` + `VulkanRenderer.h:386`, `tests/test_instance_arena.cpp` (neu), `tests/test_graph_instancing.cpp` (Guard), Abschnitt 20.
- **Verified:** Mac, Release: `he_tests --source-file=*test_instance_arena.cpp` 5/5 (Negativkontrolle: alte Arithmetik lehnt bei 983 040 jeden Frame ab);
  `test_graph_instancing` 7/7, `test_foliage_cluster` 22/22, `test_frame_upload_ring` 7/7. Vulkan `-fsyntax-only` gegen MoltenVK rc=0, Negativkontrolle
  (`chunkFour`) rc=1. Voller `he_tests`: 5420 Fälle, 5417 grün, 3 rot: `test_profiler` ×2 (unter Last, allein grün) und `test_hpak` „Disk registry“
  (auch mit dem main-Binary vom 15.10. rot, Lektion 381), keiner berührt.
- **Not verified:** D3D12 kompiliert auf dem Mac nicht (nur der Windows-Job der CI); Laufzeit auf D3D12/Vulkan nicht (kein Gerät hier): die Leiter
  500k "alle" auf NN-WS03 fehlt. Metal/GL/D3D11 nicht berührt.
- **Decisions taken:** Buchführung als Header ohne GPU (testbar, beide Backends teilen sie); Hauptpuffer-Ziel global (größter Frame), jeder Slot wächst
  bei seiner nächsten Nutzung; Budget als Bytes statt Instanzkonstante; Warnung beim nächsten Frame-Anfang mit dem ganzen Frame (verlangt/abgelehnt).
- **Decisions suggested, not taken:** gemeinsamer 64-B-Modellpuffer je Frame + clip je Pass (Bedarf ÷6 bis ÷12, neue Shader/PSOs, Abschnitt 20);
  D3D12-Schattenschleife soll den `k_maxDraws`-Ring nicht vor der Szene leeren (greift jetzt erst über dem Budget).
- **Pending:** Windows-Laufzeitprüfung dieses Schritts; Metal-Lauf; Anfrage 105.
- **Traps:** Die Leiter mit 500k "alle" vorher auf Vulkan braucht `--warmup 10 --frames 20` (1,6 s je Frame). Arena-Wachstum zeigt sich auf D3D12 erst ab
  `HE_DUMP_FRAMES=8` (3 Slots). Die Dump-Kamera sieht weniger Pflanzen als die Leiter-Kamera: der Überlauf zeigt sich nur in der Leiter.
- **Next steps:** 1. NN-WS03: `foliage_ladder_win.py` 500k "alle" auf D3D12 und Vulkan, im Log keine „arena full“-Zeile, je Slot eine „grows“-Zeile,
  ~20 Draws, D3D12-Bild vollständig, RSS notieren. 2. Bei Bedarf `HE_INST_ARENA_MIB` probieren (1 M Pflanzen). 3. Metal-Lauf. 4. Anfrage 105.
