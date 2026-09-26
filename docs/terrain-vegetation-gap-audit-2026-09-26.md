# Terrain & Vegetation: Bestandsaufnahme (26.09.2026, Thema 80, Schritt 1)

Stand: main `152659ff`. Ziel: klären, was der Website-Wert **50 %** konkret bedeutet, was im Code
wirklich fehlt oder kaputt ist, und wo die GI-Probe-Grid-Lücke tatsächlich liegt.

**Konfidenz:** Alles unten ist **code-gelesen** (Datei:Zeile angegeben), nichts laufzeit-geprüft.
Ein Laufzeit-Zeuge für das GI-Grid wurde versucht (headless Editor, `HE_DUMP_LANDSCAPELAYERS=1
HE_DUMP_GI=1`): das deployte `out/deploy/Editor/HorizonEditor` ist ein Debug-Build vom 23.09. und
lieferte bei Rechnerlast ~18 in 450 s kein Bild. Er steht deshalb als erster Folgeschritt unten.

## 1. Was der Roadmap-Eintrag sagt, und was davon stimmt

Eintrag „Terrain & Vegetation" (`advanced`, `in-progress`, 50 %):
„Chunked heightfield terrain with automatic per-chunk LOD and frustum culling, in-editor sculpting
with region-dirty regeneration, and GPU-instanced foliage with wind animation."

Die 50 % lassen sich an der ursprünglichen Phase-2-Liste aus dem Landscape-Plan (Masterplan
Forts. 19) festmachen: *Heightmap-Import, Sculpt-Brushes, Material-Splatting, Chunking/LOD,
Tessellation, Kollision*. Davon ist heute **alles außer Tessellation da** (Nachtrag: Tessellation
ist seit Schritt 2 ebenfalls da, siehe Abschnitt 6). Der Wert ist also zu
niedrig, und die Beschreibung ist gleichzeitig **zu hoch**: „foliage with wind animation" gibt es
nicht (siehe 3.3; Nachtrag: seit Schritt 3 da, siehe Abschnitt 7). Beides gehört korrigiert (eigener Schritt, siehe 5).

## 2. Was vorhanden ist (belegt)

| Bereich | Stand | Beleg |
|---|---|---|
| Heightfield, Chunking, Auto-LOD | C×C Chunk-Kinder mit je 4 LODs (65/33/17/9 Verts), `LODComponent`, Frustum-Cull pro Chunk, Skirts gegen Risse, Master auf 2ⁿ+1 resampelt | `TerrainSystem.cpp:115-170`, `TerrainMeshGenerator.cpp` |
| Region-Dirty-Regeneration | Pinsel setzt `regionDirty`, nur betroffene Chunks werden neu gebaut | `TerrainTools.cpp:484`, `TerrainSculpt.h` |
| Sculpt-Pinsel | Raise, Lower, Smooth, Flatten, Ramp, Roughen (+ Set über MCP), dt-getaktet, ein Undo-Eintrag pro Strich | `TerrainTools.cpp:882-887`, `TerrainSculpt.h` |
| Heightmap-Import | 8/16-bit PNG und `.r16`, aus Textur-Asset oder Datei, eigener Undo-Schritt | `TerrainHeightmap.h`, `TerrainTools.cpp:955ff` (Merge `d653f908`, Thema 40) |
| Layer-Painting / Splatting | 4 Layer (RGBA8-Weightmap, `weightRes`=256), normalisiert, `Landscape Layer Blend`-Knoten im Material-Graph | `TerrainPaint.h`, `TerrainComponent.h:30-54` |
| Kollision | Ein Jolt-`HeightFieldShape` pro Terrain, wird beim Sculpten im Play neu gebaut | `PhysicsWorld.cpp:866-910`, `TerrainSystem.h` (Physics-Overload) |
| Foliage | Scatter über das Terrain, Zufallsrotation um Y, Skalenbereich, `drawDistance`, gemalte Dichtemaske mit Ausschlussbereichen (Grow/Erase-Pinsel) | `FoliageSystem.cpp`, `FoliagePaint.h` |
| Foliage-Instancing | Instanzen als `RenderObject`s, gleiche Mesh-UUID wird vom Batch-Pfad instanziert | `RenderExtractor.cpp:574-606` |
| Serialisierung | Heights + Weights + Dichtemaske inline Base64 | `SceneSerializer.cpp:798ff`, `:1650ff` |
| Runtime | Terrain + Foliage laufen auch im gepackten Spiel (`SceneSystems::tickWorld`) | `GameApplication.cpp:3025`, `SceneSystems.cpp:139` |
| MCP | Terrain-Werkzeuge (Sculpt, Paint, Info) | `McpToolsTerrain.cpp` (Thema 29) |
| GI-Reflexion | Gemalte Landscapes werden in GI-Reflexionen pro Texel eingefärbt | `GiLandscape.h`, `RenderExtractor.cpp:287-371` |
| Tests | `test_terrain` 28, `test_terrain_heightmap` 9, `test_foliage` 12, `test_mcp_tools_terrain` 20, `test_terrain_tools_ui` 1 TEST_CASEs | `tests/` |

## 3. Echte Lücken, nach Töpfen

### 3.1 Bugs (klein, zuerst)

1. **Sculpten streut Foliage nicht neu.** `FoliageComponent::dirty` wird nur vom Inspector, vom
   Foliage-Pinsel und vom Heightmap-Import gesetzt (`TerrainTools.cpp:988-989`). Weder der
   interaktive Sculpt-Pfad (`TerrainTools.cpp:484`) noch `TerrainSculpt::apply`
   (MCP, `McpToolsTerrain.cpp:504`) noch `TerrainSystem.cpp` fassen `FoliageComponent` an.
   Folge: nach einem Strich schweben oder versinken Gräser/Bäume, bis jemand „Regenerate" drückt.
   Fix: bei Höhenänderung die Foliage der Entity dirty setzen, idealerweise nur im Dirty-Rect
   neu streuen.
2. **Foliage ignoriert die Welt-Transformation des Terrains.** `FoliageSystem.cpp:54` nimmt
   `tf->position` (lokal) als Ursprung, ohne Rotation und Skalierung, und backt die Instanzen in
   Weltkoordinaten. Das Terrain selbst rendert über Chunk-Kinder mit voller Weltmatrix. Folge:
   unter einem Eltern-Knoten, bei gedrehtem oder skaliertem Terrain liegt die Foliage daneben
   (siehe Memory `worldmatrix-staleness`: `HE::worldPositionOf` bzw. die volle Weltmatrix nehmen).
   Außerdem setzt ein Verschieben des Terrains die Foliage nicht dirty; sie bleibt am alten Ort.
3. **Veralteter Kommentar mit falscher Aussage**: `MetalRenderer.mm:8097` („Terrain-Chunks sind
   nicht in `m_renderWorld.objects`"). Die Chunks sind gewöhnliche `MeshComponent`-Entities und
   laufen durch die Mesh-Schleife des Extractors (`RenderExtractor.cpp:356-371`, `castsShadow`
   Default `true`, `MeshComponent.h:9`). Kommentar erst nach dem Zeugen aus 4. korrigieren.

### 3.2 Hängt an einem anderen Thema: Paint auf D3D12/Vulkan

Die Weightmap wird auf main nur von **GL, Metal und D3D11** pro Draw aufgelöst
(`grep weightmapTextureId`: D3D11 2, Metal 4, GL 6, D3D12 0, Vulkan 0). D3D12 bindet für
`heLandscapeWeights` eine Null-View (`D3D12Renderer.cpp:7231-7233`, „null views for now, their gates stay 0"), der Shader bleibt also auf Layer 0.
Gemalte Landscapes sehen auf D3D12/Vulkan also nicht so aus wie gemalt.

Der Fix existiert: `f704282d` „Bemalte Landschaften erscheinen auf D3D und Vulkan" liegt **nur** auf
`origin/claude/backend-parity-p1` (13 Commits vor main, letzter Stand 24.08.2026, nie gemergt),
auch **nicht** auf dem Zweig von Thema 78 (`claude/d3d-vulkan-parity-remaining-gaps`). Kein eigener
Umsetzungsschritt hier; Entscheidung für den Chefchen, ob der Zweig nach main oder in Thema 78 geht.

### 3.3 Falsche Roadmap-Zusage: Wind

(Nachtrag: Windgröße, Wind-Knoten und Foliage-Vorlage sind seit Schritt 3 da, siehe Abschnitt 7;
die Grenzen unten gelten weiter.)

Im Code gibt es **keinen Foliage-Wind**. „wind" kommt nur bei Wolken (`EnvironmentSettings.h:75`)
und den GPU-Wetterpartikeln (`IRenderer.h:638`) vor. Was es gibt: der Material-Graph hat einen
**World-Position-Offset**-Pin (Vertex-Stage, `MaterialGraph.cpp:37`, Codegen `:1141-1152`,
Vorlage `MaterialShaderLibrary.cpp:120-124`) und einen **Time**-Knoten. Wind ist damit per
Material autorierbar, aber:

- Es gibt keine Engine-Windgröße, die ein Material lesen könnte (Richtung/Stärke aus der
  Environment in den Material-Uniforms), und keinen fertigen Wind-Knoten bzw. keine
  Foliage-Standardmaterialvorlage.
- Die festen Schatten-/Tiefen-Shader der Backends kennen `heWpo` nicht (kein Treffer in
  `src/HE_Rendering/src/Backends`) → ein wehendes Gras würfe einen starren Schatten
  (code-gelesen, nicht geprüft).
- WPO korrigiert die Normale nicht und nutzt die Transpose-Näherung für uniforme Skalierung.

Aufwand: M (Wind-Uniform + Knoten + Vorlage), plus L falls WPO in Schatten-/Depth-Prepass aller
fünf Backends soll.

### 3.4 Autoring-Lücken (Feature-Arbeit)

| Lücke | Heute | Aufwand |
|---|---|---|
| Mehrere Foliage-Schichten pro Terrain | genau eine `FoliageComponent` pro Terrain-Entity (entt: eine Komponente je Typ), also z. B. Gras ODER Bäume | M |
| Scatter-Regeln | nur Dichte + Maske; keine Hang-/Höhen-/Layer-Filter, keine Ausrichtung an der Normalen, kein Mindestabstand | M |
| Foliage-LOD / Impostor | Instanz nutzt nur `meshAssetId`, keine Mesh-LODs, kein Cross-Fade; harter Schnitt an `drawDistance` | M |
| Foliage-Extraktion | jede Instanz wird pro Frame als eigenes `RenderObject` in den Extractor geschoben, Distanztest auf der CPU (`RenderExtractor.cpp:588-605`); skaliert schlecht bei 100k+ | M |
| Foliage-Kollision | keine (Bäume sind begehbar) | M |
| Mehr als 4 Paint-Layer | RGBA8 = 4, harte Grenze | M |
| Heightmap-Export | nur Import | S |
| Terrain-Löcher (Höhlen, Tunneleingänge) | nicht vorhanden | M |
| Erosion / Noise-/Stamp-Pinsel | nur die sechs Grund-Ops | M |
| LOD-Geomorphing | Pop beim LOD-Wechsel, Skirts verdecken nur Risse | M |
| Größe | Auflösung hart auf 1024 Verts/Seite (`TerrainMeshGenerator.cpp:68`), keine Kachelung/Streaming, kein Naht-Abgleich zwischen Nachbar-Terrains | L |
| Tessellation / Displacement | **umgesetzt in Schritt 2** (siehe 6) | – |
| Splines/Straßen, Wasser | nicht vorhanden (Lückenaudit 4.6 nennt sie) | L |

## 4. Die GI-Probe-Grid-Lücke, neu eingeordnet

Memory `ao-gi-roadmap` und der Kommentar `MetalRenderer.mm:8097` sagen: Terrain ist nicht im
Probe-Grid, weil es kein RenderObject ist. **Der Code sagt etwas anderes**: die Chunks sind
RenderObjects (3.1 Punkt 3), und im späteren Nachtrag von `docs/gi-reflections-plan.md` werden
gemalte Landscapes sogar schon per Ray-Treffer in GI-Reflexionen eingefärbt. Die älteren Absätze
desselben Plans („Terrain fehlt in TLAS/BVH", §2 und §8) widersprechen dem Nachtrag; die Doku ist
in sich uneins.

Was den Code tatsächlich begrenzt, und zwar in **allen fünf Backends gleich**:

- `kGIProbeSpacing = 4 m`, `kGIMaxProbesPerAxis = 10` (`MetalRenderer.h:1200-1201`,
  `OpenGLRenderer.h:1161-1162`, `VulkanRenderer.h:938-939`, `D3D11Renderer.cpp:2604-2605`,
  `D3D12Renderer.cpp:5345-5346`). Das Grid deckt damit **höchstens 36 m pro Achse** ab. Ein
  Default-Terrain ist 100 × 100 m.
- Das Grid wird **einmal** gebaut (`m_giProbeGridBuilt`), sobald `objects` nicht leer ist, und
  nie neu eingepasst. Kommt das Terrain nach den ersten Requisiten oder sind seine Chunk-Meshes im
  ersten Frame noch nicht auflösbar (ungültige `worldBounds` fallen aus der Union), bleibt es
  draußen.
- Verankerung ist uneinheitlich: Metal legt den Ursprung auf `bounds.min`
  (`MetalRenderer.mm:8126`), also deckt es die −X/−Z-Ecke der Szene; GL, D3D11, D3D12 und Vulkan
  zentrieren (`OpenGLRenderer.cpp:7915`, `D3D11Renderer.cpp:2935`, `D3D12Renderer.cpp:6141`,
  `VulkanRenderer.cpp:7973`).
- Außerhalb des Grids liefert `sampleDDGIIrradiance` 0 (`shaders/scene.frag:205-207`), es gibt
  keinen Fade zum Sky-Ambient.

Beide Lesarten erklären die alte Beobachtung „Grid blieb bei 2×2×2 trotz großem Terrain": entweder
war das Terrain damals nicht in `objects`, oder das einmalig gebaute Grid hat es verpasst. Welche
heute gilt, entscheidet ein Laufzeit-Zeuge (Log-Zeile aus `EnsureGIProbeGrid` bei einer
Nur-Terrain-Szene: 10×n×10 = Terrain drin, 2×2×2 oder kein Grid = draußen).

**Einordnung:** Das ist ein **DDGI-Skalierungsproblem**, kein Terrain-Problem. Auch ein großes
Gebäude aus Einzelmeshes über 36 m hat dieselbe Lücke. Lösung (Kaskaden oder kamerafolgendes
Grid, Neu-Einpassen bei Szenenänderung, Fade statt 0 am Rand) betrifft fünf Backends und gehört in
ein **eigenes Rendering-Thema**. Auf macOS-OpenGL (4.1, kein Compute) gibt es ohnehin kein DDGI.
Memory `ao-gi-roadmap` erst nach dem Zeugen umschreiben.

## 5. Vorgeschlagene Schritte für dieses Thema

1. **Bugfixes Foliage** (3.1/1+2): Sculpt/MCP/Terrain-Verschieben setzen Foliage dirty; Scatter
   über die volle Weltmatrix des Terrains. Mit Tests (Sculpt → Instanz-Y folgt; gedrehtes/geparentes
   Terrain → Instanzen liegen auf der Oberfläche). Aufwand S.
2. **GI-Grid-Zeuge** (4): Release-Build, Nur-Terrain-Szene, Grid-Logzeile lesen; danach
   veralteten Kommentar + Memory korrigieren und die Grid-Skalierung als eigenes Thema anlegen.
   Aufwand S.
3. **Foliage-Wind** (3.3): Wind aus der Environment als Material-Uniform, ein `Wind`-Knoten
   (oder Funktion) im Material-Graph, Foliage-Standardmaterial; Schatten-WPO als bewusst
   getrennte Entscheidung. Aufwand M.
4. **Mehrere Foliage-Schichten + Scatter-Regeln** (Hang, Höhe, Paint-Layer, Normalenausrichtung).
   Aufwand M.
5. **Roadmap angleichen** (nach 1–3): Beschreibung ohne unbelegte Zusage, Prozentwert neu.
   `roadmap_upsert` + `deploy` nur nach Bestätigung durch den Menschen.

Nicht hier, sondern Querverweis: D3D12/Vulkan-Paint (3.2, Zweig `backend-parity-p1` / Thema 78),
DDGI-Grid-Skalierung (4, neues Rendering-Thema). Die Autoring-Lücken aus 3.4 jenseits von Schritt 4
sind Backlog, keine Voraussetzung für „fertig" im Sinne der Roadmap-Beschreibung.

## 6. Nachtrag Schritt 2: Tessellation (umgesetzt)

**Entscheidung:** CPU-Tessellation als zusätzliche Stufe vor LOD0, **keine** Hardware-Tessellation
(Hull/Domain, TCS/TES, Metal-Tess-Faktoren). Die Terrain-Chunks sind gewöhnliche Meshes durch die
generische Material-Pipeline; ein Patch-Pfad hätte pro Backend eine eigene Pipeline und einen neuen
Codegen-Zweig im Material-System gebraucht, und hier ist nur Metal laufzeitprüfbar. So rendert die
feinere Stufe auf allen fünf Backends ohne Renderer-Änderung.

Was es tut:

- `TerrainComponent`: `tessellationFactor` (1 = aus, Default; 2 oder 4), `tessellationDistance`
  (Kamera → Chunk-Mitte, wie die LOD-Stufen), `displacementTexture`, `displacementStrength`
  (Schwarz→Weiß in Metern, Mittelgrau = 0), `displacementTiling` (0 = folgt `uvTiling`).
  Serialisiert als Gruppe nur, wenn benutzt; alte Szenen speichern byte-gleich.
- `generateTerrainChunkMeshTessellated` (`TerrainMeshGenerator.cpp`): Höhe per Catmull-Rom zwischen
  den Samples (trifft jedes Sample exakt, also stimmt jede LOD0-Ecke), plus Displacement (bilinear,
  wiederholend). Normalen = die LOD0-Normalen, gekippt um den Displacement-Gradienten, dadurch ohne
  Displacement keine Lichtkante zum LOD0-Nachbarn. Skirt um halbe Displacement-Stärke tiefer.
- `TerrainSystem::updateTessellation` (in `SceneSystems::tickWorld` vor `LODSystem`): baut die Stufe
  für Chunks innerhalb 1,25 × Distanz, gibt sie jenseits 1,5 × wieder ab (Hysterese), höchstens
  2 Builds pro Tick, höchstens 16 verfeinerte Chunks pro Terrain (nächste zuerst). Die Stufe hängt
  als `LODComponent::refinedMeshId`/`refinedMaxDistance` am Chunk, LODSystem wählt sie zuerst, wenn
  die Kamera innerhalb der Distanz ist. **Bewusst nicht in `levels`:** Navigation-Bake,
  PhysicsWorld, RenderExtractor und mehrere Editor-Stellen lesen `levels[0]` als „das volle Mesh";
  das bleibt LOD0 statt des verschobenen, bis 16-mal dichteren Laufzeit-Meshes.
  Mesh-UUID einmal pro Chunk registriert, beim Abgeben geleert statt entladen; Meshes
  zerstörter Chunks (Undo, Gitterwechsel, gelöschtes Terrain) werden entladen.
- Sculpten unter der Kamera baut die Stufe im selben Tick an Ort und Stelle neu (kein Rückfall auf
  LOD0 für ein Bild).
- Inspector: Abschnitt „Tessellation" (Faktor, Distanz, Displacement-Slot, Stärke, Tiling) mit
  Hilfetexten; MCP-`terrain_info` meldet Faktor und Stärke.
- Tests: `tests/test_terrain_tessellation.cpp`.

Speicher: Faktor 4 = 257² Vertices pro Chunk (~3,5 MB mit Indizes), Deckel 16 → ~60 MB pro Terrain.

**Bewusste Grenzen:** Kollision (Jolt-Heightfield), Navigation und Foliage-Höhe lesen weiter das
Höhenfeld, liegen also bis zur halben Displacement-Stärke neben der sichtbaren Oberfläche.
Displacement ist Detail, nicht Landform. Die Umschaltung Stufe ↔ LOD0 ist ein harter Wechsel wie
zwischen den übrigen LODs (kein Geomorphing, siehe 3.4). Hot-Reload der Displacement-Textur wird
erst beim nächsten Neubau der Chunks sichtbar. Visuell auf echter Hardware nicht geprüft.

## 7. Nachtrag Schritt 3: Foliage-Wind (umgesetzt)

**Windgröße:** Keine neue Größe. Die Engine hat schon eine: `EnvironmentSettings::windDirection`
(Kompassgrad, 0° = nach −Z, im Uhrzeigersinn) und `windSpeed`, gesetzt aus der
`EnvironmentComponent` und im Wetter vom `WeatherSystem` getrieben. Bisher las sie nur der
Wolken-Shader. Jetzt liest sie auch jedes Graph-Material.

**Transport:** In die drei bisher ungenutzten w-Kanäle des Material-Licht-Präfixes
(`MaterialShaderLibrary::Lighting`): `sunColor.w`/`ambient.w` = Einheitsrichtung x/z (wohin der
Wind weht), `camPos.w` = Stärke (`windSpeed`, ohne den 0,025-Faktor der Wolken, negativ = 0).
Gründe: ein neues Feld hätte alle Offsets dahinter verschoben (vorkompilierte Blobs), und
WPO-Vertex-Blobs von vor diesem Schritt sehen nur diese vier vec4. Gefüllt von einem gemeinsamen Helfer `HE::FillMaterialWind` (`LightPacking.h`) an jeder
Stelle, die auch die Zeit (`sunDir.w`) schreibt: Metal UI/Forward/Resolve (`FillMaterialLighting`)
/G-Buffer, GL UI + `fillMatLight`, D3D11, D3D12, Vulkan. Vorschauen (Zeit 0) bleiben windstill.

**Mitgefixt: WPO-Materialien linkten auf OpenGL nicht.** Die WPO-Vertex-Stufe deklarierte
`HeLighting` als Vier-vec4-Präfix, das Fragment den vollen Block. GL linkt beide Stufen in ein
Programm und verlangt gleichnamige Blöcke identisch: jedes WPO-Material, das `heLight` las (also
schon jedes mit Time), scheiterte mit „Uniform type mismatch '<uniform HeLighting>'" und wurde nie
gezeichnet; Wind Sway liest `heLight` immer. Gefunden im headless GL-Dump, kein Compile-Test sah es.
Jetzt schneidet `wpoLightingBlock()` den Block aus `kLightingPreamble` aus (eine Quelle, nur
`binding = 8`). Alle Backends binden am Vertex-Slot ohnehin den ganzen `Lighting`-Puffer (Metal
`setVertexBytes` in voller Größe, D3D11/D3D12 b8, Vulkan Range `sizeof`). Test „GL links a WPO
material" vergleicht den Block in beiden cross-kompilierten GL/GLES-Stufen; mit dem alten Präfix
schlägt er fehl (Negativkontrolle gelaufen).

**Knoten** (Kategorie „Landscape"):

- `Wind`: Direction (vec3), Strength (float), Vector (= Direction × Strength). Reiner Uniform-Read,
  in Vertex- und Fragment-Stufe gleich.
- `Wind Sway`: fertiger Offset für World Position Offset. Lehnt immer mit dem Wind (atmet zwischen
  55 % und 100 %, schnappt also nie aufrecht zurück), schwingt mit `Frequency` (Hz) und einer
  Phase aus Rauschen über Welt-XZ (Nachbarn nicht im Gleichschritt), Böen als Rauschfeld, das mit
  dem Wind treibt. `Amount` = Meter je Stärke-Einheit (Default 0,1), `Bend Height` = Objekt-Höhe
  in Metern bis zum vollen Ausschlag, quadratisch von der Mesh-Wurzel aus (0 = aus), `Mask` für
  eigene Gewichte (UV, Vertex-Farbe). Die Biegemaske braucht die Objektposition und gibt es nur in
  der Vertex-Stufe (auch innerhalb einer Material-Funktion in der WPO-Kette); im Fragment fällt sie
  auf 1 zurück.

**Vorlage:** `material_create` mit `template: "Foliage"`: lit, Masked, die PBR-Parameter plus
`WindAmount`, `WindFrequency`, `BendHeight` als Parameter vor einem Wind-Sway-Knoten auf dem
WPO-Pin. Über `material_set_param` zur Laufzeit einstellbar wie jeder andere Parameter.

**Tests:** `test_material_graph.cpp` (Kanäle, Vertex/Fragment-Zweig, Funktions-Scope,
`FillMaterialWind` gegen die Wolken-Kompassrichtung, Vertex-Cross-Compile Metal/GL/GLES, Wind-Sway-
WPO im Backend-Sweep für HLSL/SPIR-V), `test_mcp_tools_material.cpp` (Foliage-Vorlage: Parameter,
Vertex-Body im Asset, Fragment + Vertex kompilieren für Metal und GL).

**Bewusste Grenzen** (Entscheidung für den Chefchen):

1. **OpenGL: gebatchte Foliage weht nicht.** Der GL-Szenenpfad schickt Instancing-Batches
   (gleiches Mesh, mehr als eine Instanz, also genau der Foliage-Fall) durch das eingebaute
   Instanz-Programm, **bevor** der Graph-Material-Zweig greift (`OpenGLRenderer.cpp`, Forward
   `if (!dc.instanceTransforms.empty() && m_instancedProgram ...)`, G-Buffer ebenso), und der
   Graph-Zweig zeichnet nur `dc.transform`. Das trifft jedes Graph-Material auf GL, nicht nur
   Wind (der Metal-Kommentar „the GL path's open bug" meint genau das). Metal schließt
   Graph-Materialien vom Batch aus, D3D11/D3D12/Vulkan zeichnen sie pro Instanz
   (`drawMatInstance`), dort wirkt der Wind. Fix: GL-Gate um „kein Graph-Material" ergänzen und
   den Graph-Zweig über `instanceTransforms` laufen lassen, eigener Schritt.
2. **Schatten und Tiefe ohne WPO:** die festen Schatten-/Depth-Shader kennen `heWpo` nicht, ein
   wehendes Gras wirft also einen starren Schatten (Befund aus 3.3, unverändert).
3. **Normalen:** WPO korrigiert die Normale nicht. Für Gras-Karten und kleine Ausschläge
   unauffällig, für große Biegungen sichtbar.
4. **Laufzeit geprüft auf Metal (Forward + Deferred) und OpenGL, eine Instanz.** Zeuge
   `HE_DUMP_MATERIALTEST=wind` (Kugel, Wind Sway auf WPO, Amount 0,6, Bend Height 1) im
   Debug-Editor, jeweils mit `HE_DUMP_SKYTEST=1 TOD=0.5 COVERAGE=0 CLOUDMODE=0`, Zeit über
   `HE_SKY_TIME`, Pixel mit Abweichung > 2/255:

   | Pfad | Wind an, t 0 gegen 0,6 | `WINDSPEED=0`, t 0 gegen 0,6 |
   |---|---|---|
   | Metal Forward | 19 658 px, max 146, nur y 141–323 | 0 px |
   | Metal Deferred (`RENDERPATH=1`, G-Buffer-Füllstelle) | 19 646 px, nur y 141–323 | – |
   | OpenGL (nach dem Link-Fix) | 18 452 px, max 127, nur y 141–323 | 0 px |

   y 141–323 ist die obere Kugelhälfte: Himmel und untere Hälfte (Biegemaske 0) bleiben
   pixelgleich. Vor dem Link-Fix zeigte GL 0 px Bewegung; Metal ist mit und ohne den Fix
   pixelgleich. Wind an gegen aus bei gleicher Zeit unterscheidet sich ebenfalls nur oben.
   D3D11, D3D12, Vulkan: nur kompiliert (Vulkan per MoltenVK-Syntaxcheck mit Negativkontrolle,
   D3D im Windows-CI). Gebatchte Foliage auf GL: siehe Punkt 1, vom Kugel-Zeugen nicht erfasst.
   Ohne `HE_DUMP_SKYTEST` ist der Material-Zeuge auf diesem Stand komplett schwarz, auch der alte
   `switchon`-Modus ohne WPO; das liegt am Aufbau, nicht am Wind.

## 8. Nachtrag Schritt 4: Terrain im DDGI-Probe-Grid (umgesetzt)

**Zeuge zuerst (Release-Build von HEAD `ee323bfa`, NN-WS03, RTX 4070):** Szene
`HE_DUMP_LANDSCAPELAYERS=1 HE_DUMP_GI=1` (nur das 100 × 100 m große Witness-Terrain, bei
y = 300), Draufsicht `SKYTEST CAMY=392 PITCH=-89`, 90 Settle-Frames. GL und D3D11 loggen
**`GI probe grid 10x4x10 (400 probes)`**. Das Terrain war also im Grid, und zwar als
**einziges** Objekt; Abschnitt 4, Lesart 1 („Terrain nicht in `objects`“) ist damit widerlegt.
Die Lücke war die Deckelung: 4 m × 10 Sonden = 36 m in der Terrainmitte. Außerhalb lieferte
`sampleDDGIIrradiance` 0, und weil GI das diffuse Sky-Ambient **ersetzt**, verlor das Terrain
dort das ganze Umgebungslicht, nicht nur den Bounce. Auf GL ist das Quadrat im Bild sichtbar.
Kennzahl (GI-an / GI-aus pro Pixel, das teilt die Albedo heraus; außen / innen direkt über die
alte Grid-Kante, nur roter Untergrund): **links 0,850, rechts 0,678**.

**Umsetzung:** gemeinsame Einpassung `HorizonRendering/GIProbeGrid.h` (header-only), von allen
fünf Backends benutzt statt fünf Kopien:

- `FitGIProbeGrid`: Abstand ab 4 m in 5-%-Schritten vergrößern, bis die ganze Szene-Box mit einer
  Spacing Rand in ein **Budget von 1000 Sonden** passt (= alter Höchstwert 10³, Speicher und
  Kosten pro Frame bleiben gleich), höchstens 32 pro Achse (flaches Terrain bekommt die Sonden
  in die Breite). Immer zentriert, auch Metal (verankerte bisher an `bounds.min`). Szenen, die das
  alte Grid schon abdeckte, fitten **bit-gleich** (gleiche Formel, gleiche Reihenfolge;
  Unit-Test gegen die alte Formel).
- Der Abstand reist wie bisher in `gridOrigin.w`; jeder Verbraucher (scene.frag,
  gi_probe*.comp/.hlsl, Metal-MSL, `MaterialShaderLibrary`, GL/D3D-Strings) liest ihn schon dort.
  **Keine Shader-Änderung.** `kGIProbeSpacing`/`kGIMaxProbesPerAxis` sind aus allen Backends raus,
  ersetzt durch ein Member (`m_giProbeSpacing` bzw. `giProbeSpacing`).
- **Neu-Einpassen statt einmalig:** geprüft wird nur, wenn sich die *Geometrie* ändert:
  Objekt-Signatur (Entity + Mesh, reihenfolgeunabhängig, bewusst blind für Transforms),
  `InvalidateMesh` (Sculpt, LOD-/Tessellationsstufen) oder solange noch Objekte mit ungültigen
  Bounds fehlen (Mesh noch nicht hochgeladen: die zweite Lesart aus 4). Reine Bewegung löst nichts
  aus; ein aus der Welt fallender Körper bläht das Grid nicht auf. Neu eingepasst wird, wenn die
  Box mehr als eine halbe Spacing über das Grid ragt oder ein frischer Fit mindestens doppelt so
  fein wäre (Szene stark geschrumpft). Atlas-Neuanlage: GL/D3D11/Metal direkt (Referenzzählung),
  D3D12 legt die alten Atlanten über `m_retiredTextures` still (Descriptor-Slots erst nach
  `waitForAllFrames`), Vulkan `vkDeviceWaitIdle` + Bindings 5/6 aller Frame-Sets auf die weiße
  Ersatztextur, bis `runGi` sie neu schreibt.
- Vulkan loggt die Grid-Zeile jetzt auch; alle Zeilen nennen die Spacing.

**Nachher, gleiche Szene/Kamera:** alle vier Windows-Backends `GI probe grid 15x4x15 (900 probes),
spacing 8.731492`. GL-Kennzahl über die alte Kante: **links 0,958, rechts 0,955**; Kontrollen an
Stellen ohne alte Kante 0,980 / 0,967, der Rest ist also der natürliche Abfall nach außen, keine
Naht. GI-aus-Bild bit-gleich zur Baseline (1,000). Neu-Einpass-Zeuge **`HE_DUMP_GIREFIT=1`**
(neu, `EditorApplication::dumpFrameHeadless`): nach der Hälfte der Settle-Frames kommt ein zweites
Terrain bei x = 170 dazu → auf GL, D3D11, D3D12 (mit `HE_GPU_DEBUG=1`) und Vulkan zweite Zeile
`22x4x11 (968 probes), spacing 14.222674`, Bild weiter sauber; D3D12-Debug-Layer nur mit dem
bekannten „Ignoring InitialState“-Hinweis, Vulkan-Validierung nach Art und Anzahl identisch zum
Lauf ohne Neu-Einpassen (alles vorbestehend, siehe unten). Unit-Tests `tests/test_gi_probe_grid.cpp`
(5 Fälle), volle Suite Release 3768/3768 grün. **Metal: hier weder gebaut noch gesehen**, nur
macOS-CI; dort ändert sich zusätzlich die Verankerung (zentriert statt `bounds.min`).

**Bounce vom Terrain:** Terrain-Chunks sind `StaticMeshAsset`s mit CPU-Vertices und laufen über
denselben `castsShadow`-Filter in BLAS/TLAS aller Backends; `InvalidateMesh` verwirft den BLAS-Cache
(Sculpt bleibt korrekt, Cache wächst nicht). Sie werfen also GI-Schatten und liefern Bounce-Licht
(mit der flachen Instanzfarbe, siehe `GiLandscape.h`).

**Offen (nicht in diesem Schritt, belegt):**

1. **Graph-Materialien bekommen auf D3D11/D3D12/Vulkan kein DDGI.** Nur GL
   (`OpenGLRenderer.cpp`, `lit.giProbe`) und Metal füllen `giGridOrigin/giGridCounts/giProbe` im
   Material-Lichtpräfix; D3D11/D3D12/Vulkan lassen `giProbe.y = 0`, der Material-Shader nimmt dann
   Sky-Ambient. Jedes **bemalte** Terrain ist ein Graph-Material (`LandscapeLayerBlend`), bekommt
   dort also Sky-Ambient statt Probe-Licht (kein Schwarz, aber kein Bounce). Messbar: D3D11 ist vor
   und nach diesem Schritt pixelgleich im Witness, D3D12 ohne räumlichen GI-Verlauf. Die HLSL-Pins
   für `heGIIrradiance`/`heGIVisibility` (t17/s1, t18/s3) stehen schon in `MaterialShaderLibrary`;
   es fehlen Füllen + Binden (D3D11/D3D12) bzw. das Pipeline-Layout (Vulkan meldet schon auf main
   für Material-Pipelines Bindings 14–18/32/33 als „not declared“, `heGIIrradiance` „invalid“).
   Eigener Schritt, gehört zur Backend-Parität (Thema 78).
2. **GI-Schattenmaske streift auf flachem Terrain** (waagrechte Bildschirmzeilen, GL/D3D11/Vulkan,
   schon auf der Baseline): unabhängig vom Grid, nicht untersucht.
3. **Tessellation verwirft den ganzen GI-BLAS-Cache:** jede neu gebaute verfeinerte Stufe ruft
   `InvalidateMesh`, und das leert in GL/D3D11/D3D12/Vulkan/Metal-SW den *gesamten* konkatenierten
   BLAS-Cache (Neuaufbau aller Meshes beim nächsten GI-Frame). Korrekt, aber bei bewegter Kamera
   über Terrain teuer; per-Mesh-Splice wäre der Fix.
4. **Kein Fade am Grid-Rand:** außerhalb des Grids weiter 0 statt Sky-Ambient. Mit der
   Neu-Einpassung liegt statische Geometrie immer drin; betroffen sind nur Objekte, die sich aus
   dem Grid heraus *bewegen* (bewusst kein Refit auf Bewegung).
5. Eine sehr große Szene (km-Terrain) bekommt grobe Sonden (4 km → ~300 m); Kaskaden bzw. ein
   kamerafolgendes Grid bleiben ein eigenes Rendering-Thema.
