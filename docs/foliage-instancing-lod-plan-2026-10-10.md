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
| F1 | **Sculpt-Striche streuen nicht neu.** Der Pinselpfad (Raise, Lower, Smooth, Flatten, Ramp, Roughen) setzt nur `regionDirty`; „Reset Sculpting“ und die Änderung von Größe, Auflösung oder Seed im Inspector ebenso nicht. Nur Mountain (`TerrainTools.cpp:701-704`) und Heightmap-Import setzen `fol->dirty` | Audit §3.1/1 stimmte; seit dem Audit ist nur Mountain dazugekommen |
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
| OpenGL | Der Instanzzweig (`OpenGLRenderer.cpp:11525` G-Buffer, `:11749` Forward) steht **vor** dem Zweig für benutzerdefinierte Programme (`:11551`, `:11778`) und nimmt immer das eingebaute Instanzprogramm. Kommentar im Code: „Instanced batches always take the built-in instanced program“ | **Ein Draw, aber falsches Material:** kein Wind, kein Alpha-Discard, flaches PBR. Der Bug aus Altplan §2/§6.4 und Audit §7 Grenze 1 ist noch da |
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
1. **Bounds-Refine-Schleifen** (5 Backends; D3D11 `:6194-6199`, GL `:10651-10653`, die übrigen drei nachzulesen). Sie *überschreiben* heute `worldBounds` aus
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

**Stand Teil 2c (10.10.2026):** Scope `ExtractFoliage`, Zeuge `HE_DUMP_FOLIAGETEST=<N>`, `gen_reference_world.py --foliage`, `scripts/perf/foliage_ladder.sh` und `foliage_ladder_table.py` sind gebaut, die Basismessung 10k / 100k / 500k auf Metal (alles in Reichweite und rund 20 %) steht in `docs/perf-audit/foliage-baseline-2026-10-10.md`. Noch offen aus diesem Abschnitt: Zähler für Cluster/Instanzen/Aufklappen (gibt es erst mit 2a), 1 Mio. Instanzen in der Leiter, GL-Läufe.

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
