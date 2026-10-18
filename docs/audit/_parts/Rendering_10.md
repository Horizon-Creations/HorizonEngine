# Rendering Slice 10 – Frontend (RenderExtractor, RenderGraph/-Pass, Sorter, OcclusionCuller, ScenePick, MaterialShaderLibrary, Sky/SSAO-Helfer) + `shaders/`

## Zusammenfassung

Der Frontend-Code ist insgesamt sauber strukturiert (Phasen-Funktionen im Extractor, Frame-Cache mit Reuse-Pfad, vorberechnete Sort-Keys, Depth-Run-Batching, Occlusion-Culler mit klaren Konservativitäts-Garantien). Die größten Kosten liegen nicht in der Algorithmik, sondern in **wiederholtem Kopieren großer Strukturen pro Frame** (DrawCall mit `instanceTransforms`, Bone-Matrizen, GLSL-Quelltexte pro Draw) und in **hand-gepflegter Duplikation** (Shader-Blöcke in `MaterialShaderLibrary.cpp`, sieben GI-Kernel-Kopien). Gefunden: **2 Hoch, 13 Mittel, 9 Niedrig** (insgesamt 24). Die Datei-Liste des Slice enthielt `src/HE_Rendering/shaders/` nur unvollständig: es fehlten `gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (6 von 45 Dateien, 1276 Zeilen); die vier GLSL-Kernel wurden zusätzlich vollständig gelesen, die beiden HLSL-Twins nur teilweise (siehe „Gelesen“), und sie sind in den Funden REN-1013/1014/1023 berücksichtigt. Hinweis: `CommandBuffer`/`Culler` liegen als Header/Impl in anderen Slices bzw. wurden nur gezielt (`CommandBuffer.cpp/.h`, `RenderObject.h`, `RenderSorter.h`) gelesen, soweit sie die Funde belegen. Nichts davon wurde gemessen; Aufwandsschätzungen sind Schätzungen.

---

## Funde

### REN-1001 – Voller GLSL-String wird pro Draw und Frame kopiert und gehasht
- **ID**: REN-1001
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952`, `:981-998`, `:1000-1009`; Aufrufer im Draw-Loop: `Backends/D3D11/D3D11Renderer.cpp:7009`, `Backends/D3D12/D3D12Renderer.cpp:10974`, `Backends/Vulkan/VulkanRenderer.cpp:8254` (je pro `DrawCall`), GL/Metal über `resolveMaterialShader` (`OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815`)
- **Problem**: `resolveFragment`/`resolveShaders`/`resolveGBufferShaders` kopieren `customShaderFragGlsl` (und `customShaderVertGlsl`) in Out-Strings und berechnen `std::hash<std::string>` über den **gesamten Quelltext** – bei jedem Draw jedes Frames, nur um danach den Cache-Key `hash` für `fragment()` zu bekommen. Graph-Materialien haben mehrere KB GLSL (Preamble wird erst später injiziert, trotzdem Kopie+Hash des Material-Bodys). Die Deklaration in den Backends (`std::string matFrag, matVertBody;` direkt vor dem Aufruf, D3D11:7008) legt dazu pro Draw zwei Strings auf dem Stack an, die beim Kopieren Heap-Allokationen auslösen.
- **Code-Auszug**:
```cpp
glslOut = mat->customShaderFragGlsl;                 // 1009: Kopie des gesamten Shaders
hashOut = std::hash<std::string>{}(glslOut);         // pro Draw, pro Frame
...
vertBodyOut = mat->customShaderVertGlsl;
hashOut ^= std::hash<std::string>{}(vertBodyOut) * 0x9E3779B97F4A7C15ULL;
```
- **Lösung**: (1) Hash einmal beim Laden/Ändern des Materials berechnen und im `MaterialAsset` ablegen (`customShaderHash`, `customShaderGBufHash`, `customShaderVertHash`; Invalidierung dort, wo die Strings gesetzt werden – Graph-Compile, PSHD-Load). (2) API auf Zeiger/`const std::string&` umstellen: `bool resolveShaders(cm, id, uint64_t& hash, const std::string*& frag, const std::string*& vert)`; Strings werden nur bei Cache-Miss in `fragment()` gebraucht. (3) Backends cachen ihr Ergebnis pro `materialAssetId` für die Dauer eines Frames (kleine flache Map), damit Mehrfachdraws desselben Materials nicht erneut auflösen.
- **Erwarteter Effekt**: Pro Graph-Material-Draw entfallen 1-2 Heap-Kopien und ein Hash über mehrere KB (Schätzung: ~1-5 µs je Draw, bei ein paar tausend Graph-Draws 1-several ms/Frame auf dem Render-Thread). Messen mit Profiler-Scope um den Aufruf.
- **Risiko / Verifikation**: Cache-Key-Wert muss bitgleich bleiben (er ist Schlüssel für `m_fragCache`, Pipelines der Backends und teils Dateicaches) – sonst Pipeline-Rebuilds. Test: `tests/test_material_graph.cpp` (Resolve/Hash-Fälle), zusätzlich Test „Hash vor/nach Refactor identisch für Fixture-Materialien“.

### REN-1002 – DrawCall wird mehrfach tief kopiert (inkl. `instanceTransforms`)
- **ID**: REN-1002
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:78`, `:102`, `:147-168`; `src/HE_Rendering/src/CommandBuffer.cpp:17-20`; `include/HorizonRendering/CommandBuffer.h:62-65`
- **Problem**: (a) `recordDraw(const DrawCall&)` kopiert jeden Aufruf per `push_back(call)`; `DrawCall` enthält `std::vector<glm::mat4> instanceTransforms` und `std::vector<float> paramOverride`. (b) Bei Multi-Section-Meshes macht `recordRun` pro Section `DrawCall sd = d;` – jede Kopie dupliziert die komplette Instanzliste (Section-Anzahl × N × 64 B), danach nochmal die Kopie in `recordDraw`. (c) Im Split-Pfad (`total > kMaxInstancesPerDraw`, z. B. 140k Pflanzen) entstehen `all` (9 MB bei 140k), `chunk = dc`, `assign(...)` und wieder die Kopie in `recordDraw`: jede Instanz-Matrix wird pro Frame 3× kopiert, bevor das Backend sie ein weiteres Mal in den Instance-Buffer hochlädt. (d) `dc.paramOverride = first.paramOverride` (Z. 78) kopiert den 64-Float-Block je Run.
- **Code-Auszug**:
```cpp
DrawCall sd        = d;                       // RenderPass.cpp:102 – kopiert instanceTransforms
...
outCmds.recordDraw(sd);                       // CommandBuffer.cpp:19: m_drawCalls.push_back(call) – nochmals Kopie
```
- **Lösung**: Instanzdaten aus dem `DrawCall` herauslösen, wie es `DepthBatchList` schon tut: `CommandBuffer` besitzt einen flachen `std::vector<glm::mat4> m_instances`; `DrawCall` trägt nur `{uint32_t instanceFirst, instanceCount}`. `GeometryPass` hängt die Matrizen **einmal** per `appendInstances` an und alle Sections/Chunks eines Runs teilen sich die Range (Chunking = Range-Slicing, kein `assign`). `recordDraw(DrawCall&&)`/`emplace_back` für den Rest. Backends lesen `cmds.instances().data() + dc.instanceFirst` statt `dc.instanceTransforms`. Zwischenschritt ohne ABI-Änderung: `recordDraw(DrawCall&&)` anbieten und im Section-Pfad nur die **letzte** Section verschieben; Kopien nur für die ersten n-1.
- **Erwarteter Effekt**: Entfällt (Sections+1)× das Kopieren aller Instanz-Matrizen und die Mini-Allokation pro Draw. Bei 140k Foliage-Instanzen ca. 2× 9 MB weniger Speicherverkehr/Frame; `CommandBuffer::reset()` behält Kapazität → keine Heap-Last nach Warm-up (Schätzung, messen).
- **Risiko / Verifikation**: Alle fünf Backends lesen `dc.instanceTransforms` (Grep nötig) – mechanische Änderung, aber breit. Absichern: `tests/test_rendergraph.cpp` (GeometryPass-Batching, Sections), `tests/test_foliage_cluster.cpp` (Cluster-Entfaltung/Chunking), Bildvergleich je Backend. Beobachtbares Verhalten (Draw-Reihenfolge, Kontrakt „1 Instanz = leere Liste“) bleibt erhalten.

### REN-1003 – Bone-Matrizen werden pro Frame 3-4× kopiert
- **ID**: REN-1003
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:639-641`, `:647`; `src/HE_Rendering/src/RenderPass.cpp:194`, `:209`; `src/HE_Rendering/src/CommandBuffer.cpp:22-25`
- **Problem**: Je Skinned-Entity: `smc.boneMatrices` → `obj.boneMatrices` (Kopie, bis 128×64 B = 8 KB), → `dc.boneMatrices = obj.boneMatrices` (2. Kopie), → bei Multi-Section pro Section `SkinnedDrawCall sd = dc` (n-te Kopie), → `recordSkinnedDraw(const&)` (weitere Kopie). Außerdem wird pro Entity `m->sections` (Z. 647) kopiert. Skinned Objekte werden nicht gecullt (`RenderPass.cpp:174-175`, `worldBounds = kUnitCube…` Z. 636) – die Kopien fallen also auch für nicht sichtbare Charaktere an.
- **Code-Auszug**:
```cpp
obj.boneMatrices = smc.boneMatrices.empty() ? std::vector<glm::mat4>{ glm::mat4(1.0f) } : smc.boneMatrices; // 639
dc.boneMatrices  = obj.boneMatrices;                                                                         // RenderPass.cpp:194
SkinnedDrawCall sd = dc;                                                                                     // :209
```
- **Lösung**: Bone-Daten wie Instanzen in einen flachen Frame-Puffer (`RenderWorld::boneArena`, `std::vector<glm::mat4>`) schreiben; `SkinnedRenderObject`/`SkinnedDrawCall` halten `{boneFirst, boneCount}`. Extractor kopiert **einmal** (oder zeigt direkt auf `smc.boneMatrices`, solange die Welt während des Frames unverändert ist – Lebenszeit prüfen). Section-Kopien tragen nur die Range.
- **Erwarteter Effekt**: Bei z. B. 200 Skinned-Entities × 8 KB × (3 Kopien eingespart) ≈ 5 MB/Frame weniger Kopierverkehr (Schätzung). Zusätzlich kein Heap-Churn pro Entity.
- **Risiko / Verifikation**: Lebensdauer des Puffers (`RenderWorld` wird von Backends über Frames gehalten? – `RenderExtractor::beginFrame/endFrame` regelt Reuse). Test: `tests/test_skeletalmeshcomponent.cpp`, `tests/test_rendergraph.cpp` (Skinned-Fall).

### REN-1004 – `ShadowPass` zeichnet Commands auf, die D3D11/D3D12/GL verwerfen
- **ID**: REN-1004
- **Kategorie**: D / F
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:234-254`, `:266-277`; Verbraucher: `Backends/D3D11/D3D11Renderer.cpp:6268`, `:6600-6604`; `Backends/D3D12/D3D12Renderer.cpp:10261`, `:10611-10614`; `Backends/OpenGL/OpenGLRenderer.cpp:10985-11001`
- **Problem**: D3D11, D3D12 und GL fügen `ShadowPass` in den Graph ein. Ihre Sink-Callbacks benutzen im `kShadowMapTarget`-Zweig aber ausdrücklich **nicht** das aufgezeichnete `cmds` („not the graph's whole-scene ShadowPass set“), sondern culling/sortieren pro Kaskade selbst und nutzen `batchDepthCasters`. Der Pass verursacht trotzdem pro Frame O(N) Objektschleife, Frustum-Test und je Caster `DrawCall`-Konstruktion (~260 B inkl. zweier Vektoren) + Kopie in `recordDraw`. Metal und Vulkan haben den Pass nicht im Graph (verifiziert per Grep). `ShadowPass::describe()` wird nur für `io.output.id`/Größe gebraucht.
- **Code-Auszug**:
```cpp
for (size_t idx = 0; idx < world.objects.size(); ++idx) { ... DrawCall dc; ... outCmds.recordDraw(dc); }  // RenderPass.cpp:240-253
// D3D11: "...not the graph's whole-scene ShadowPass set, which is culled against the legacy single-map frustum"
```
- **Lösung**: `ShadowPass::execute` zu einem No-Op machen (Marker wie `PostProcessPass`), der Pass bleibt als Ziel-Deklaration im Graph (`describe()` liefert weiter `kShadowMapTarget`). Vorher in D3D12/GL prüfen, dass der Zweig `cmds` wirklich nie liest. Danach `recordDraw`-Pfad der Klasse nur noch für Tests; Tests, die `ShadowPass` Draws erwarten, auf den Marker umstellen oder die Draw-Aufzeichnung hinter eine Test-Option legen.
- **Erwarteter Effekt**: Entfällt eine komplette O(N)-Schleife mit Frustum-Test und N DrawCall-Kopien pro Frame in drei Backends (Schätzung: ~0,1-0,5 ms bei 10k Objekten; messen).
- **Risiko / Verifikation**: Falls ein Backend (z. B. D3D12-Pfad im Sink) `cmds.drawCalls()` im Shadow-Zweig doch liest → sichtbare Schattenregression. Absichern mit `tests/test_rendergraph.cpp` (Anzahl Draws des ShadowPass) und Backend-Smoke-Test mit Schatten. **Nicht verifiziert** für D3D12-Sinkcode über Z. 10611 hinaus.

### REN-1005 – Extractor: Heap-Kopien pro Mesh-Entity und Frame
- **ID**: REN-1005
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:208-209`, `:254-264`, `:439-443`, `:449`, `:471-472`
- **Problem**: Für **jede** sichtbare Mesh-Entity wird pro Voll-Extract `std::vector<MeshSection> table = m->sections;` kopiert (enthält `materialPath` als `std::string`), obwohl `resolveEntitySlots` für `sections.size() <= 1` (der Normalfall) nur `sections[0]` liest und nichts nachlädt. Dazu `lod0SectionsOf` (weitere `try_get<LODComponent>` + Kopie via `meshSectionsOf`), `slotOf` (Vektor) und in der Schleife `items.push_back(d)` (Kopie statt Move; `EntityData` hält zwei Vektoren) sowie `obj.sections = d.sections; obj.paramOverride = d.paramOverride;` im `parallel_for` (Kopie statt Move, außerdem Heap-Allokation aus mehreren Threads → Allokator-Kontention).
- **Code-Auszug**:
```cpp
std::vector<MeshSection> table = m->sections;                       // 439: Kopie je Entity
d.sections = resolveEntitySlots(std::move(table), lod0SectionsOf(reg, e, d.meshId, *contentManager), ...);
items.push_back(d);                                                 // 449: Kopie
obj.sections = d.sections;  obj.paramOverride = d.paramOverride;    // 471-472 im parallel_for
```
- **Lösung**: (1) In `resolveEntitySlots` Fast-Path vor jeder Kopie: wenn `m->sections.size() <= 1` und kein Slot-Override/LOD0-Fall, nur `sections[0].materialId/materialPath` lesen (kein Copy der Tabelle). Kopie nur im Multi-Section-Fall, wo `loadAsset` die Pointer ungültig machen kann (das ist der dokumentierte Grund für By-Value). (2) `items.push_back(std::move(d))`. (3) `obj.sections = std::move(items[i].sections)` (items sind danach tot; `EntityData&` statt `const`). (4) `lodSlotMap` nur aufrufen, wenn `lod0Sections` nicht leer.
- **Erwarteter Effekt**: Pro Mesh-Entity 1-2 Heap-Allokationen weniger (Schätzung: 10k Entities → einige Hundert µs/Voll-Extract; nur relevant, wenn `sections` bei den meisten Assets nicht leer ist – **messen**: Anteil Meshes mit `sections.size()==1`).
- **Risiko / Verifikation**: Die By-Value-Regel (Dangling nach `loadAsset`) muss für Multi-Section erhalten bleiben. Tests: `tests/test_foliage_cluster.cpp`, `tests/test_editor_icons.cpp`, `tests/test_light_window_order.cpp` + ein Test mit Mesh mit/ohne Sections und Slot-Override (Precedence unverändert).

### REN-1006 – Sort-Key ohne Material → Batch-Runs fragmentieren
- **ID**: REN-1006
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderSorter.cpp:26-36`; Konsument `src/HE_Rendering/src/RenderPass.cpp:32-61`
- **Problem**: `RenderSorter::sort` ordnet nach Mesh-UUID, dann nach Kamera-Distanz. `GeometryPass` bricht einen Run aber bei jedem Wechsel von `materialAssetId`, `paramOverride`, `instanceTint`, `receivesShadow`, `weightmapTextureId`. Teilen sich viele Objekte ein Mesh (Prefab-Varianten, Bäume mit 2-3 Materialien, Props), werden sie innerhalb der Mesh-Gruppe nach Distanz gemischt → Material wechselt von Objekt zu Objekt → ein DrawCall pro Objekt statt eines instanzierten Draws plus mehr Material-/Pipeline-Wechsel (v. a. bei Graph-Materialien, siehe REN-1001).
- **Code-Auszug**:
```cpp
m_keys.push_back(SortKey{ o.meshAssetId.hi, o.meshAssetId.lo, glm::dot(d, d), i });   // 26
... if (a.meshLo != b.meshLo) return a.meshLo < b.meshLo; return a.distSq < b.distSq;  // 33-35
```
- **Lösung**: Key erweitern: `(mesh, materialAssetId, weightmap/tint-Hash, dist)`. Materialwechsel-Grenze mit einem 64-Bit-Hash aus `materialAssetId` + `receivesShadow` + (`paramOverride.empty()`) im Key; Distanz bleibt als letzte Stufe. Zusätzlich wäre ein einmaliger Radix-/Hash-Gruppierungsschritt statt `std::sort` über 128-Bit-UUID-Vergleiche möglich (Niedrig, messen).
- **Erwarteter Effekt**: Weniger DrawCalls/State-Wechsel bei Szenen mit gleichem Mesh in mehreren Materialien (Schätzung, stark szenenabhängig – Zähler `counters`/DrawCall-Anzahl im Test messen). Preis: Front-to-back innerhalb eines Meshes bleibt, über Materialgrenzen hinweg nicht mehr → minimale Early-Z-Verschlechterung.
- **Risiko / Verifikation**: Ändert die Zeichenreihenfolge (kann Z-Fight/Transparenz-Reihenfolge nicht betreffen, da Transparente später `sortBackToFront` laufen). Tests: `tests/test_rendergraph.cpp` (Batching-Erwartung ggf. anpassen), `tests/test_culling.cpp`.

### REN-1007 – Schatten-PCF vor dem Dämpfungstest in den 8-Licht-Schleifen
- **ID**: REN-1007
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:763-828` (heLitP, Schattenaufruf `:818-822`, Vergleich: Cluster-Schleife prüft `if (atten <= 0.0) continue;` vor dem Schatten `:607`, `:1242`); `src/HE_Rendering/shaders/scene.frag:484-531` (Schatten `:523`; Cluster-Schleife `:570`)
- **Problem**: In der Fenster-Schleife wird für jedes Punkt-/Spotlicht `heLocalShadowFactor`/`localShadowFactor` (9 Texture-Fetches + Matrix-Multiplikation) berechnet, **bevor** bekannt ist, ob das Licht das Fragment überhaupt erreicht (`atten == 0`, außerhalb von `range` oder außerhalb des Spot-Kegels). Das Ergebnis geht mit Faktor 0 in die Summe. Die Cluster-Varianten haben den Early-Out bereits.
- **Code-Auszug**:
```glsl
if (type != 0) { sh = min(sh, heLocalShadowFactor(i, worldPos, n)); localIdx++; }   // MaterialShaderLibrary.cpp:818-822
...
result += (diffuseColor * ndl + specColor * spec) * lc * atten * sh;                // 827 – atten kann 0 sein
```
- **Lösung**: Nach der Dämpfungsberechnung `if (atten <= 0.0) { if (type != 0) localIdx++; continue; }` einfügen (Zählerfortschritt für die GI-Maskenkanäle beibehalten!). Gleiches in `scene.frag` (`giLocalIdx++` erhalten). Prüfen, ob GL `kUnlitFS`, Metal und D3D-HLSL dieselbe Reihenfolge haben (nicht Teil dieses Slice; per Grep `localShadowFactor` in den Backends).
- **Erwarteter Effekt**: Bei Szenen mit mehreren schattenwerfenden Local-Lights spart jedes Fragment außerhalb der Reichweite 9 Fetches + 1 Mat-Mul pro Licht (Schätzung: 10-30 % der Fragment-Zeit in lichtlastigen Szenen; GPU-Profil nötig).
- **Risiko / Verifikation**: Bitgleichheit der Ausgabe: bei `atten <= 0` war der Beitrag `… * 0 * sh` = 0 (außer `sh` = NaN; PCF liefert nie NaN). Drift-Guard `tests/test_culling.cpp` („GI kernels: the constants…“) und `tests/test_material_graph.cpp` (Preamble-String-Vergleiche) müssen angepasst werden; SYNC-Kopien in allen Backends gleichziehen (`MaterialShaderLibrary.cpp:171-198` listet sie).

### REN-1008 – `scene.frag` (Vulkan) wertet `skyColor()` pro Fragment 2-3× analytisch aus
- **ID**: REN-1008
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/scene.frag:255-294` (Definition), `:445`, `:455`, `:590`
- **Problem**: Ambient-Diffus (`skyColor(Nup, …)`), Spekular-IBL (`skyColor(Rrough, …)`) und Fog-Farbe (`skyColor(ray/dist, …)`) rufen pro Fragment die komplette analytische Himmelsfunktion auf (mehrere `smoothstep`, `pow` mit Exponenten 1800/180/22/5/8/2.5, `normalize` …). GL/Metal/Graph-Materialien sampeln dafür einen einmal pro Frame gebackenen Sky-Cubemap (`uSkyEnv` / `heSkyEnv`); der Header des Shaders dokumentiert das als bekannte Drift („This file evaluates skyColor() analytically per fragment“).
- **Code-Auszug**:
```glsl
vec3 ambDiff = skyColor(Nup,    uf.sunDir.xyz) * base * kd;   // 445
vec3 envSpec = skyColor(Rrough, uf.sunDir.xyz);               // 455
vec3  fogCol = skyColor(ray/dist, uf.sunDir.xyz);             // 590
```
- **Lösung**: Vulkan nutzt für Graph-Materialien bereits `heSkyEnv` (gebackener Cubemap-Pfad im Preamble). Dieselbe Textur an `scene.frag` binden (neuer Binding-Slot analog `uSkyEnv` in GL) und `texture(uSkyEnv, dir)` statt `skyColor()` verwenden; damit entfällt zugleich die dokumentierte Drift und der Unterschied zwischen Built-in- und Graph-Material. Als Zwischenschritt: `skyColor` für die beiden IBL-Aufrufe nur in einem Branch, wenn `giParams.y < 0.5` (ambDiff) bzw. nur bei Bedarf auswerten.
- **Erwarteter Effekt**: Deutlich weniger ALU je Fragment (3 × ~30 transzendente Ops → 2-3 Cubemap-Fetches); Schätzung: bei hoher Auflösung einige Prozent bis zweistellig GPU-Zeit im Forward-Pass (Messung nötig, Vulkan-GPU-Timer).
- **Risiko / Verifikation**: **Sichtbare Änderung** der Vulkan-Beleuchtung (Annäherung an GL/Metal – gewollt laut Banner, aber Reviewer-Entscheidung). Deskriptor-Layout (`VulkanRenderer.cpp` Scene-Set), `FrameUBOData` unverändert. Test: Bild-Smoke Vulkan vs. GL; `tests/test_sky_shader.cpp`.

### REN-1009 – Instanced-Vertex-Shader lesen 128 B pro Instanz (MVP + Model), CPU multipliziert pro Pass
- **ID**: REN-1009
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/scene_instanced.vert:17-24`; `gi_gbuf_instanced.vert:12-19`; `ssao_pos_instanced.vert:15-22`; `scene_shadow_instanced.vert:15-18`
- **Problem**: Jede Instanz trägt `mvp` (4 vec4) und `model`/`modelView` (4 vec4) als Instanz-Attribute. Die CPU berechnet pro Instanz **pro Pass** (Szene, SSAO-Prepass, GI-G-Buffer, jede Shadow-Kaskade/Local-Layer) `viewProj * model` und lädt 128 B hoch. Bei Foliage-Runs (bis 65536 Instanzen, `RenderConstants.h:31`) sind das pro Pass bis zu 8 MB Upload + 65k Mat4×Mat4 auf der CPU. Der Shadow-Shader liest nur 64 B (`scene_shadow_instanced.vert` nutzt nur MVP), die übrigen Passes brauchen `model` (bzw. `modelView`), das aus `model` und einem Pass-Uniform ableitbar wäre.
- **Code-Auszug**:
```glsl
layout(location = 3)  in vec4 iMvp0; ... layout(location = 6) in vec4 iMvp3;
layout(location = 7)  in vec4 iModel0; ... layout(location = 10) in vec4 iModel3;   // scene_instanced.vert
```
- **Lösung**: Instanz-Buffer nur mit `model` (64 B, oder 48 B als 3 vec4) füllen und **einmal pro Frame** hochladen; `viewProj` (und `view` für den SSAO-Prepass) per Push-Constant/UBO (64 B) pro Pass. Shader: `gl_Position = pc.viewProj * (model * vec4(aPos,1))`. Alle Passes teilen denselben Instanz-Buffer (Shadow-Kaskaden sparen den Re-Upload komplett).
- **Erwarteter Effekt**: Halbe bis ein Viertel der Instanz-Upload-Bandbreite und der CPU-Matrixmultiplikationen bei Foliage/Instancing (Schätzung; Pass-Anzahl ≥ 5 pro Frame). Vertex-ALU steigt minimal (1 Mat4×Vec4 mehr).
- **Risiko / Verifikation**: Float-Genauigkeit: `viewProj*(model*p)` vs. vorab multipliziertes `(viewProj*model)*p` – leichte Rundungsunterschiede, kann Z-Fighting/Depth-Differenz zwischen Prepass und Scene-Pass erzeugen (derzeit **bitgleich** garantiert: Kommentar in `ssao_pos_instanced.vert:8`). Falls Depth-Konsistenz zwischen Passes nötig ist, `invariant gl_Position` bzw. alle Passes auf dieselbe Rechnung stellen. CPU-Seite liegt in `VulkanRenderer.cpp` (nicht in diesem Slice; vor Umsetzung lesen). Test: Vulkan-Bildvergleich, `tests/test_culling.cpp`.

### REN-1010 – `resolveWorldMaterialScalars` läuft bis zu 3×/Frame über alle Objekte mit Hash-Lookup je Objekt/Section
- **ID**: REN-1010
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/MaterialScalars.cpp:11-23`, `:30`, `:36`, `:40-45`; Aufrufer Vulkan `VulkanRenderer.cpp:3890` (EncodeDecalDepth), `:7838` (DrawScene), `:11495` (runGi); D3D11 `:6253`; D3D12 `:10245`
- **Problem**: Jeder Aufruf löst pro `RenderObject` und pro `RenderSection` über `cm->getMaterial(id)` (UUID→Handle→Dense-Array-Lookup, `ContentManager.cpp:2546`) die Skalare neu auf – auch wenn tausende Objekte dasselbe Material teilen. Vulkan ruft das in drei verschiedenen Pfaden pro Frame auf, jeweils mit unverändertem Ergebnis, solange sich Materialien nicht ändern.
- **Code-Auszug**:
```cpp
for (RenderObject& obj : world.objects) resolveObject(cm, obj);        // 43
const MaterialAsset* mat = cm->getMaterial(materialId);                // 16, pro Objekt/Section
```
- **Lösung**: (1) Idempotenz-Flag im `RenderWorld` (`scalarsEpoch = cm->contentEpoch()` + Frame-Nummer): zweiter/dritter Aufruf im selben Frame kehrt sofort zurück. (2) Innerhalb des Aufrufs ein kleines lokales Memo (`UUID → Skalare`, z. B. letzter Treffer + flache Hash-Map) oder die Auflösung in den Extractor verlagern (er kennt bereits `contentEpoch`), dann entfällt die Schleife für nicht geänderte Materialien ganz. 
- **Erwarteter Effekt**: Von 3 auf 1 Durchlauf in Vulkan, Lookups pro eindeutigem Material statt pro Objekt (Schätzung: 10k Objekte × 2-3 Lookups × ~30-50 ns ≈ 1 ms eingespart/Frame; messen).
- **Risiko / Verifikation**: Materialänderung zwischen den Aufrufen im selben Frame (Hot Reload) → Epoch in den Key. Test: neuer Test „Skalare nach Materialänderung aktualisiert“; vorhandene Material-Tests (`tests/test_material_graph.cpp`).

### REN-1011 – Niederschlag als Einzel-`RenderObject` pro Tropfen
- **ID**: REN-1011
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:514-617` (v. a. `:558-615`)
- **Problem**: Jeder Regen-/Schneetropfen wird zu einem vollwertigen `RenderObject` (~300 B inkl. zweier Vektoren, `mat4`, AABB) mit eigener CPU-Billboard-Matrix (Cross-Products, 2-3 `normalize`, `sqrt`) und läuft danach durch Frustum-Cull, `RenderSorter` (SortKey 32 B je Objekt), `GeometryPass` (Run-Bildung, danach `instanceTransforms`-Kopie 64 B je Tropfen, siehe REN-1002) und Occlusion. Der Kommentar nennt „tens of thousands of drops per frame“. Außerdem wird `out.objects.resize(base + n)` zuerst alle neuen Objekte default-initialisiert und dann überschrieben; im `parallel_for` läuft pro Partikel ein `std::upper_bound` über die Emitter.
- **Code-Auszug**:
```cpp
out.objects.resize(base + pin.size());                                  // 562
const PrecipEmitter& em = *std::upper_bound(emitters.begin(), emitters.end(), i, ...);  // 568 – pro Partikel
```
- **Lösung**: Niederschlag wie die `ParticleBatch`-Pipeline (GPU-Instancing, `ParticleShaderTemplates`) als eigener Batch führen: pro Tropfen nur `{position, velocity}` (24 B) hochladen, Billboard-Basis im Vertex-Shader (kamerabezogen). Alternativ minimal: statt `upper_bound` je Partikel pro Emitter einen eigenen `parallel_for` über dessen Range (Emitterzahl ≈ 1).
- **Erwarteter Effekt**: Bei 20-50k Tropfen: ~300 B → 24 B pro Partikel und keine CPU-Matrix, kein Sort/Cull/Instanz-Kopie (Schätzung: mehrere ms CPU/Frame bei Starkregen; messen).
- **Risiko / Verifikation**: Optik muss bitnah gleich bleiben (Billboard-Maße 0.6/0.02, 0.16); Backend-Arbeit in allen fünf Backends (neuer Draw-Pfad), daher der teuerste Vorschlag – nur nach Messung. Zwischenlösung (Emitter-Loop statt `upper_bound`) ist risikolos. Test: `tests/test_particles.cpp`.

### REN-1012 – Mehrfach duplizierte Shader-Blöcke in `MaterialShaderLibrary.cpp`
- **ID**: REN-1012
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:418-449` (heLocalShadowFactor), `:539-570` (heFwdClusterShadow), `:1158-1189` (heClusterShadow); Weather/Material-Split `:648-671`, `:1198-1212`, `:1807-1818`; HLSL-Pin-Tabellen `:1368-1387` vs. `:2714-2734`; Fresnel/Fog-Faktor in `kSSRCompositeFS` `:1845-1867` vs. heLitP/heApplyFog
- **Problem**: Dieselbe Logik existiert 3× (lokale Schatten-PCF), 3× (Schnee/Nässe-Material-Aufspaltung), 2× (HLSL-Register-Pins) als Textkopien mit „SYNC“-Kommentaren und Drift-Tests (`test_material_graph.cpp`, `test_culling.cpp`), die die Kopien nur „modulo textureLod“ vergleichen. Jede Änderung (z. B. REN-1007) muss an 3-6 Stellen erfolgen.
- **Code-Auszug**:
```glsl
float heLocalShadowFactor(int i, vec3 worldPos, vec3 n) { ... }                    // 418
float heFwdClusterShadow(vec4 posType, vec4 params, vec3 worldPos, vec3 n) { ... }  // 539 – gleicher Körper
float heClusterShadow(vec4 posType, vec4 params, vec3 worldPos, vec3 n) { ... }     // 1158 – gleicher Körper
```
- **Lösung**: Gemeinsame GLSL-Snippets als `constexpr const char*` (oder per `std::string`-Zusammensetzung wie `buildDeferredResolveSource`) – z. B. `kLocalShadowCore` (explizite Licht-Daten) einmal definieren; `heLocalShadowFactor(i,…)` ruft `heLocalShadowAt(lightPosType, params.y, …)`; `textureLod`-Variante per bereits vorhandenem `explicitLodClusterSamples`-Ersetzer. HLSL-Pin-Liste einmal als `kHlslMaterialPins` bauen und `compileResolveVariant` nur die Zusatz-Pins anhängen.
- **Erwarteter Effekt**: ~250 Zeilen weniger Shader-Text, Drift-Risiko sinkt; keine Laufzeitänderung.
- **Risiko / Verifikation**: Der Quelltext gelangt in precompilierte/gecachte Shader (Hash ändert sich → Pipeline-Cache-Neuaufbau, PSHD-Blobs im Pak sind unabhängig, weil sie kompiliert gespeichert sind – prüfen). Die String-Vergleichs-Guards in `tests/test_material_graph.cpp`/`tests/test_culling.cpp` anpassen; HLSL-FXC-Test (Windows) und Metal-Compile-Test laufen lassen.

### REN-1013 – Sieben handgepflegte GI-Kernel-Kopien, obwohl `he::shaderc` Cross-Compile kann
- **ID**: REN-1013
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/shaders/gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (Banner `gi_probe_hw.comp:13-51`, `gi_shadow_hw.comp:11-39`); Parallelstellen: `Backends/D3D_Shared/HlslSources.h` (`kGiTraversalHLSL`, `kGiProbeCSHLSL`, `kGiShadowCSHLSL`), `Backends/OpenGL/OpenGLRenderer.cpp` (`kGiTraversalGLSL`, `kGiProbeCS`, `kGiShadowCS`), `Backends/Metal/MetalRenderer.mm` (`kGISWMSL`, `kGIProbeMSL`, `kGIShadowMSL`)
- **Problem**: DDGI-Probe-Update und GI-Shadow-Kernel existieren je API-Dialekt und je Traversal-Pfad (SW-BVH / HW-RayQuery) – laut Banner nichts generiert, nur ein Guard-Test für drei Dateien, vier String-Kopien „out of reach“. Gleichzeitig kompiliert die Engine Sky (`SkyShaderSource.h`) und Material-Shader bereits aus **einer** GLSL-Quelle per `he::shaderc` für Metal/HLSL/GLSL.
- **Code-Auszug**:
```
// gi_probe_hw.comp:13: "SYNC: seven hand-kept copies of this kernel ... Nothing generates them from a common source"
```
- **Lösung**: Kernel einmal als `.comp`-GLSL mit `#ifdef HE_RAYQUERY` (SW-BVH-Traversal vs. `rayQueryEXT`) pflegen, über `he::shaderc` (SPIRV-Cross) zu HLSL (SM 5/6), MSL und GLSL430 transpilieren (wie `deferredResolve`/`compileResolveVariant`), Binding-Pins wie bei `kHlslMaterialPins`. RayQuery→DXR/MSL-Intersector bleibt ggf. eine kleine backend-spezifische Funktion (`giSceneAnyHit`/`giSceneClosestHit`), der Rest (Kegel-Sampling, Hysterese, Oct-Mapping, Feld-Irradiance) ist identisch.
- **Erwarteter Effekt**: Vier String-Kopien entfallen, Drift-Risiko in einem Subsystem, das laut Banner „nirgends durch Bildtests“ gedeckt ist. Aufwand hoch (mehrere Tage, Hardware-Verifikation auf allen Backends).
- **Risiko / Verifikation**: HW-RayQuery-Pfade (SPIRV-Cross unterstützt `GL_EXT_ray_query` nur eingeschränkt) – ggf. nur die SW-Kopien vereinheitlichen. Absichern: `tests/test_culling.cpp` („GI kernels“), `tests/test_gi_bvh.cpp`, Vergleichsbild pro Backend.

### REN-1014 – SW-Traversierung: lineare Instanzschleife (TLAS) pro Strahl, Closest-Hit ohne Nahe-zuerst-Ordnung
- **ID**: REN-1014
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/gi_shadow.comp:97-108`; `gi_probe.comp:61-98`, `:101-130`
- **Problem**: `giSceneAnyHit`/`giSceneClosestHit` iterieren **alle** Instanzen pro Strahl (`for (i < instCount)`), transformieren Ursprung und Richtung (Mat4×Vec4 + Mat3×Vec3 aus einem 112-B-Struct) und testen erst dann die BLAS-Wurzel. Kosten pro Strahl O(Instanzen); mit der Pixel-Auflösung × `spp` + bis zu 8 Sekundärstrahlen je Probe-Texel skaliert das linear in der Szenenkomplexität (Bäume/Props als eigene Instanzen). `giBlasClosest` pusht beide Kinder ohne Distanz-Ordnung (`:91-95`), was bei Closest-Hit mehr Knoten besucht, als nötig.
- **Code-Auszug**:
```glsl
for (int i = 0; i < instCount; ++i) {
    vec3 oL = (giInsts[i].invTransform * vec4(o, 1.0)).xyz;      // gi_shadow.comp:102
    vec3 dL = mat3(giInsts[i].invTransform) * d;
    if (giBlasAnyHit(...)) return true; }
```
- **Lösung**: Echten TLAS-BVH über die Instanz-Weltboxen auf der CPU bauen (`GiBvh` kann das Prinzip bereits, `GiBvh.h:8` nennt „flat instance array stays renderer-side“), als weitere Knotenliste hochladen und im Kernel zuerst die Welt-AABB testen, bevor `invTransform` angewendet wird; Minimalvariante: Welt-AABB pro Instanz (aus `GiInst`, 1 vec4-Paar) vor der Strahl-Transformation testen. Closest-Hit: Kind mit kleinerem `tN` zuerst absteigen.
- **Erwarteter Effekt**: Für Szenen mit einigen hundert Instanzen Reduktion der pro Strahl durchlaufenen Instanzen um 1-2 Größenordnungen (Schätzung; GI-Pass-GPU-Zeit messen, `HE_GPU_TIMERS`/`GpuPassScope`).
- **Risiko / Verifikation**: Ergebnis muss identisch bleiben (Treffer-Menge, `t`-Werte): `tests/test_gi_bvh.cpp` (CPU-Referenz `giBvhIntersect`) erweitern. Gleiche Änderung in den HLSL/GL/MSL-Kopien nötig (siehe REN-1013).

### REN-1015 – Sky-Noise: 64 MiB 3D-Textur, CPU-Bake bei jedem Renderer-Start
- **ID**: REN-1015
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/SkyNoise3D.cpp:57`, `:63-81`; Aufrufer: `OpenGLRenderer.cpp:4407-4411`, `VulkanRenderer.cpp:9141-9145`, `D3D11Renderer.cpp:5464-5468`, `D3D12Renderer.cpp:2704-2708`, `MetalRenderer.mm:7592-7593` (Release: `kNoiseN = 256`)
- **Problem**: `n = 256` ergibt 256³ × 2 Kanäle × 2 B = **64 MiB** CPU-Vektor, danach als 3D-Textur (64 MiB VRAM) pro Renderer-Instanz. Der Bake (16,7 M Voxel × 27 Worley-Zellen, bis 8 `std::thread`) läuft synchron im Renderer-Init (Kommentar nennt früher 10,7 s, jetzt mit Jitter-Tabelle schneller, Restzeit unbekannt). Rohe `std::thread`s statt `JobSystem`. Der `.r`-Kanal ist nur ein `starHash` pro Voxel (kein Gewinn gegenüber Berechnung im Shader außer Filterung).
- **Code-Auszug**:
```cpp
std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);                 // 57 – 64 MiB bei n=256
const int workers = n < 32 ? 1 : std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
```
- **Lösung**: (1) Messen (Bake-Zeit, VRAM). (2) Optionen: Cache der gebackenen Daten auf Disk (deterministisch, Hash der Parameter), Bake im Hintergrund-Job statt im Init, oder Auflösung/Format senken (RG8 → 32 MiB, 128³ → 8 MiB; Worley mit Wrap hält sich gut) – optische Änderung. (3) Bei mehreren Renderern (Editor-Viewports + Preview) denselben Vektor teilen (`static` Cache hinter dem Funktionsaufruf).
- **Erwarteter Effekt**: Start-/Ladezeit und 64 MiB RAM/VRAM weniger (Schätzung je nach Variante).
- **Risiko / Verifikation**: Bytes sind „pinned“ (Kommentar in Z. 25-28: Bit-Determinität über Compiler/Backends): Disk-Cache/Teilen ändert nichts, Auflösungsreduktion schon (Wolken-Look). Test: `tests/test_sky_shader.cpp`, ein Byte-Hash-Test des Bakes.

### REN-1016 – Toter Resource-/Handle-Unterbau und doppelte `RenderGraph::execute`-Überladung
- **ID**: REN-1016
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/RenderResourceManager.cpp` (komplett), `include/HorizonRendering/GPUMemoryAllocator.h:10-14`, `src/HE_Rendering/src/GPUMemoryAllocator.cpp`; `RenderObject.h:54-55`, `CommandBuffer.h:21-22`; `RenderPass.cpp:66-67`, `:181-182`; `RenderGraph.cpp:24-33`
- **Problem**: `RenderResourceManager` wird außer in `tests/test_rendermanager.cpp` nirgends benutzt (Grep über `src/`); `GPUMemoryAllocator.h` sagt selbst „which the backends do not use either“. `RenderObject::meshHandle/materialHandle` und `DrawCall::mesh/material` (4 × 8 B) werden nie gesetzt (immer `invalid()`), aber pro Objekt/DrawCall mitkopiert (`RenderPass.cpp:66-67`). Die `execute(…, CommandBuffer&)`-Überladung wird nur von Tests benutzt (Backends nutzen die `PassSink`-Form).
- **Code-Auszug**:
```cpp
dc.mesh     = first.meshHandle;     // RenderPass.cpp:66 – immer RenderHandle::invalid()
dc.material = first.materialHandle;
```
- **Lösung**: Entscheiden: entweder Plan „3.7 Handle-Auflösung“ wirklich umsetzen, oder `RenderResourceManager`, `GPUMemoryAllocator`, die vier Handle-Felder und die Merged-Buffer-`execute`-Überladung entfernen (Tests ggf. auf `PassSink` umstellen). Entfernen spart ~32 B je `DrawCall`/`RenderObject` (siehe REN-1002).
- **Erwarteter Effekt**: Weniger Code (~150 Zeilen), kleinere Structs (Cache), eine Abstraktion weniger.
- **Risiko / Verifikation**: Öffentliche Header (DLL-Export) – externe Nutzer? `tests/test_rendermanager.cpp`, `tests/test_rendergraph.cpp` anpassen.

### REN-1017 – Extractor-Kleinkram: Magic Numbers, rohe Licht-Typ-Ints, doppelte Day/Night-Logik
- **ID**: REN-1017
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `RenderExtractor.cpp:64`, `:887-890`, `:916`, `:1062`, `:1068`, `:1089`, `:1106`, `:1064`, `:1515`/`:1569`, `:1531-1541`/`:1553-1560`; `RenderPass.cpp:272-273`; `RenderConstants.h:14`; `LightPacking.h:27`
- **Problem**: (a) `constexpr int kMaxShaderLights = 8;` dupliziert `kMaxLightWindow` (`LightPacking.h:27`); ändert jemand eine Konstante, divergieren Fenster und Schatten-Layer-Zuweisung still. (b) `l.type != 0`, `== 1`, `== 2` statt `HE::LightType` (`orderLightWindow`, `fitDirectionalShadow`, `assignLocalShadowLayers`). (c) `ShadowPass::describe()` hartkodiert `2048` statt `kShadowMapResolution`. (d) `extractTransforms(world, reg)` hat einen ungenutzten `entt::registry&`-Parameter. (e) `applyDayNight`: Luminanzformel doppelt, Sonne-/Mond-Synthese als Kopie, und mehrfaches „push_back kann Zeiger invalidieren“-Zurücksetzen (`moonLight = nullptr; for … find`). (f) `assignLocalShadowLayers` alloziert pro Aufruf ein `std::vector<int> shadowed` (max. 8 Einträge).
- **Code-Auszug**:
```cpp
constexpr int kMaxShaderLights = 8;                   // 1062
io.output.width = 2048;                               // RenderPass.cpp:272 (kShadowMapResolution = 2048 existiert)
moonLight = nullptr;  for (LightData& l : out.lights) if (l.envRole == 2) { moonLight = &l; break; }   // 1539-1541
```
- **Lösung**: `kMaxLightWindow` und `kShadowMapResolution` verwenden; `LightType`-Enum-Vergleiche; `applyDayNight`: erst fehlende Sun-/Moon-Lichter anhängen (Indizes statt Zeiger), danach einmal Zeiger ermitteln und befüllen; `std::array<int,8>` für `shadowed`; ungenutzten Parameter entfernen.
- **Erwarteter Effekt**: Lesbarkeit/Drift-Sicherheit; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Reihenfolge der angehängten Lichter (Sonne vor Mond) muss gleich bleiben (`orderLightWindow` stabil). `tests/test_light_window_order.cpp`, `tests/test_culling.cpp`.

### REN-1018 – `MaterialShaderLibrary`: handgepackte Cache-Keys und Whitespace-Nachbau in `assembleVertex`
- **ID**: REN-1018
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/material/MaterialShaderLibrary.h:510-522`; `src/material/MaterialShaderLibrary.cpp:1467`, `:1980`, `:2002`, `:2025`, `:2051`, `:2218`, `:2242`, `:2259`, `:2538-2539`, `:920`; `assembleVertex` `:41-45`, `:65-66`
- **Problem**: Zehn `unordered_map<int|uint64_t, Compiled>` mit von Hand zusammengebauten Schlüsseln (`backend*4 + k`, `+ 64`, `0x100 + backend`, `hash ^ (K * (backend+1))`); der Header-Kommentar (`:519-521`) gesteht eine frühere Schlüsselkollision. Außerdem emuliert `assembleVertex` die Ausrichtungs-Leerzeichen der drei Ur-Varianten (`wpPad`), obwohl Whitespace im GLSL-Quelltext die Kompilierung nicht beeinflusst (Begründung in Z. 41-45 „a single stray character is a runtime shader-compile failure“ gilt nicht für Leerzeichen).
- **Code-Auszug**:
```cpp
const int key = static_cast<int>(backend) * 4 + 3;                  // 1980 (m_ssrCache)
const char* wpPad = wpo ? "   " : (ssbo ? "  " : " ");               // 66
```
- **Lösung**: Eine `enum class ShaderSlot { …}` und `std::array<std::optional<Compiled>, kSlots>` pro Backend (oder `struct Key{Backend b; Slot s;}` mit Hash); Hash-Keys für Fragment/Vertex als `struct{uint64_t hash; Backend; bool clustered;}`. `wpPad` ersatzlos streichen (Hash-Wechsel einmalig akzeptieren).
- **Erwarteter Effekt**: Weniger fehleranfälliger Code; keine Laufzeitänderung.
- **Risiko / Verifikation**: Entfernen des Padding ändert `std::hash` des Vertex-Quelltexts → In-Process-Caches neu (kein Disk-Cache gefunden; prüfen, ob `ShaderCompiler.h` auf Platte cached). Tests: `tests/test_material_graph.cpp`.

### REN-1019 – `OcclusionCuller`: Rasterizer-Inneres, Doppel-Lookups, Vollsortierung (Feature standardmäßig aus)
- **ID**: REN-1019
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/OcclusionCuller.cpp:139`, `:164-165`/`:342-343`, `:178-186`/`:287-292`, `:189-192`, `:200-257`, `:294-309`
- **Problem**: `Settings::enabled = false` per Default, nur GL/Metal rufen `refine` (`OpenGLRenderer.cpp:10929`), daher niedrige Priorität. Dennoch: (a) pro Pixel dreimal `edge(...)` mit `swap`-Verzweigung statt inkrementeller Edge-Funktion/Tile-Reject; (b) jeder Occluder transformiert **alle** Vertices (`m_clipScratch.resize(vcount)`), auch wenn nur ein Index-Teilbereich (Section) gezeichnet wird; (c) `getStaticMesh`/`opaqueRanges`/`boxCorners` werden für ausgewählte Kandidaten doppelt berechnet; (d) `std::sort` aller Kandidaten, danach `resize(maxOccluders=48)` (→ `nth_element`/`partial_sort`); (e) `m_depth.assign(W*H, 0)` pro Frame (256×~144, klein).
- **Code-Auszug**:
```cpp
if (s * edge(x0, y0, x1, y1, cx, cy) < 0.0f) continue;      // 249-251: 3× pro Pixel mit swap-Branch in edge()
std::sort(candidates.begin(), candidates.end(), ...);       // 189, danach nur 48 behalten
```
- **Lösung**: Edge-Koeffizienten (A,B,C) pro Dreieck vorberechnen und über die Zeile inkrementieren (die „kanonische Reihenfolge“ für Nahtfreiheit bleibt, indem die Koeffizienten aus der kanonischen Kante abgeleitet werden); Vertices nur aus dem Indexbereich transformieren (Min/Max-Index je Range) bzw. Transformation lazy per Cache; `partial_sort` mit 48.
- **Erwarteter Effekt**: Gering bei 256×144 und ≤ 48 Occludern (Schätzung < 0,2 ms); nur relevant, falls das Feature standardmäßig aktiviert wird.
- **Risiko / Verifikation**: Nahtfreiheit (kein Loch entlang Diagonalen) ist ein explizites Designziel. Test: `tests/test_culling.cpp` (Occlusion-Fälle) erweitern um Bitgleichheit des Tiefenpuffers vor/nach Umbau.

### REN-1020 – `sortBackToFront` berechnet den Key im Vergleich über Zeiger auf große DrawCalls
- **ID**: REN-1020
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderSorter.cpp:67-76`; Aufrufer D3D11 `:6966`, D3D12 `:10929`, Vulkan `:8205`
- **Problem**: Der Kommentar in `sort()` (Z. 7-11) begründet, warum der Key vorab berechnet wird; `sortBackToFront` macht es genau umgekehrt: pro Vergleich zwei Matrix-Spaltenzugriffe über `const DrawCall*` (~260 B-Struct, Cache-Miss), O(n log n) Mal.
- **Code-Auszug**:
```cpp
return backToFrontKey(a->transform, camPos) > backToFrontKey(b->transform, camPos);   // 73-74 im Comparator
```
- **Lösung**: `std::vector<std::pair<float, const DrawCall*>>` (Key vorab) sortieren, danach zurückschreiben. Header-Kommentar „std::sort ist nicht stabil“ bleibt gültig – das Verhalten bei Gleichstand dürfte sich minimal ändern (nicht stabil, daher nicht beobachtbar spezifiziert).
- **Erwarteter Effekt**: Klein bei wenigen hundert transparenten Draws (Schätzung µs); relevant bei vielen transparenten Einzel-Draws (Partikel-Objekte, Tint-Fades).
- **Risiko / Verifikation**: Reihenfolge bei exakt gleicher Distanz kann sich ändern (Header erklärt, dass absichtlich nicht „repariert“). Test: `tests/test_rendergraph.cpp`/Transparenz-Tests prüfen.

### REN-1021 – `queryUnderwaterCamera` baut pro Voll-Extract einen frischen `WaterIndex`
- **ID**: REN-1021
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:1195-1203`, `:1418`; `src/HE_Scene/src/WaterQuery.cpp:33-86`
- **Problem**: Jeder Voll-Extract (auch pro Viewport) durchläuft alle `TerrainComponent`, holt für jedes Terrain mit Wasser `worldMatrixOf` (Hierarchie-Walk), `glm::inverse`, baut einen `std::vector<Entry>` und sortiert ihn; der im `refresh` vorgesehene Cache der Wet-Rect-Daten (`WaterQuery.cpp:75-86`) greift nie, weil `build()` jedes Mal bei leerem `m_entries` beginnt. Die Kosten fallen nur bei Terrains mit Wasserbodies an, dafür garantiert jeden Frame.
- **Code-Auszug**:
```cpp
const water::WaterIndex index = water::WaterIndex::build(world);     // 1202
if (!index.surfaceAt(cameraPos.x, cameraPos.z, 0.0, surf)) return out;
```
- **Lösung**: Schnell-Exit vor dem Build (kein `TerrainComponent` mit `water.bodies` → sofort leer; per `registry.view` mit `any_of`), oder einen `WaterIndex` als Member des `RenderExtractor` halten und per `refresh` aktualisieren (Cache bleibt erhalten). 
- **Erwarteter Effekt**: Entfällt 1 Vektor-Allokation + Inverse je Wasser-Terrain/Frame (Schätzung µs; nur bei vorhandenen Wasserterrains).
- **Risiko / Verifikation**: `m_entries` hält `const TerrainComponent*` (Zeiger auf Registry-Speicher) – bei persistentem Index nach `refresh` vor jeder Abfrage gültig halten. Test: `tests/test_underwater_camera.cpp`.

### REN-1022 – `ScenePick::raycast`: Matrixinversion vor dem Boxtest, Normalen-Transpose bei jedem Treffer
- **ID**: REN-1022
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/ScenePick.cpp:35-79`, `:47`, `:77`
- **Problem**: Editor-Interaktion (pro Mausereignis, nicht pro Frame). Für **jedes** Objekt wird `glm::inverse(obj.transform)` berechnet und `lookup(...)` (`std::function`) aufgerufen, bevor die AABB geprüft wird; `worldBounds` der Objekte (bereits vorhanden) wird nicht für einen billigen Strahl-Vorab-Test genutzt. Bei jedem *verbessernden* Dreieck wird `glm::transpose(invModel)` + `normalize` ausgeführt statt einmal am Ende.
- **Code-Auszug**:
```cpp
const glm::mat4 invModel = glm::inverse(obj.transform);                               // 47
best.normal = glm::normalize(glm::vec3(glm::transpose(invModel) * glm::vec4(nLocal, 0.0f)));  // 77
```
- **Lösung**: Vor `lookup`/`inverse` Strahl gegen `obj.worldBounds` (falls valid) testen und mit `bestT` abbrechen; Normale erst nach der Schleife aus gemerktem `bestObj`/`nLocal` berechnen.
- **Erwarteter Effekt**: Klick-/Hover-Latenz bei 10k+ Objekten (Schätzung: ms → <1 ms); niedrige Priorität (Editor-only).
- **Risiko / Verifikation**: Gleiches Pick-Ergebnis; `tests/test_scene_pick.cpp`, `tests/test_viewport_pick.cpp`, `tests/test_gizmo_pick.cpp`.

### REN-1023 – `gi_probe.comp`/`gi_probe_hw.comp`: Lese-nach-Schreib-Hazard auf dem EMA-Image (unsicher)
- **ID**: REN-1023
- **Kategorie**: F (Korrektheit/Nebenläufigkeit)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/shaders/gi_probe.comp:153-160`, `:185`, `:267-273`; `gi_probe_hw.comp:127-133`, `:159`, `:241-247`; Vergleich `gi_probe_hw.hlsl:121-123` (liest `uIrrPrev`-SRV-Kopie)
- **Problem**: Die GLSL-Kernel lesen die Irradiance der **Nachbarprobes** per `imageLoad(uIrr, …)` (`giSampleFieldIrradiance`), während andere Workgroups desselben Dispatches ihre Texel via `imageStore` in dasselbe Image schreiben. Der Kommentar („imageLoad returns the previous frame's values, only this thread's own texel is written“) gilt nur für das eigene Texel; Nachbarprobe-Tiles der aktuellen Batch können bereits überschrieben sein. Es gibt kein `coherent`/Barrier. Die HLSL-Variante vermeidet das mit einer SRV-Kopie (`uIrrPrev`). Folge: nichtdeterministische Mehrfach-Bounce-Beiträge (EMA dämpft stark).
- **Code-Auszug**:
```glsl
sum += imageLoad(uIrr, tile + octTexel).rgb * w;     // gi_probe.comp:185 – Nachbarprobe, evtl. schon neu geschrieben
imageStore(uIrr, outCoord, vec4(mix(radiance, oldIrr.rgb, hIrr), 1.0));   // :269
```
- **Lösung**: Wie HLSL: Ping-Pong-Images (Prev/Cur) oder Kopie des Atlas vor dem Dispatch, `uIrrPrev` als `sampler2D`/readonly-Image. Alternativ Probe-Batches so wählen, dass Nachbarn nicht im selben Dispatch liegen (Round-Robin-Cursor erlaubt das nicht garantiert).
- **Erwarteter Effekt**: Deterministische GI; ggf. leichtes Flackern der Bounce-Komponente verschwindet. **Unsicher**: ob der Effekt sichtbar ist (nicht gemessen); Speicher +1 Atlas.
- **Risiko / Verifikation**: Zusätzlicher Atlas/Barrier pro Frame (Perf-Kosten messen); Gleichstand mit D3D/GL/Metal herstellen (`kGIProbeMSL`, `kGiProbeCS` prüfen, ob dort dasselbe gilt – nicht gelesen).

### REN-1024 – Shader-Mikrooptimierungen (nur nach Messung)
- **ID**: REN-1024
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `shaders/ssao_blur.frag:14-16`; `shaders/gi_atrous.frag:66-78` (`:75`); `shaders/postfx_bloom_blur.frag:8-11`; `shaders/taa_resolve.frag:29-37`; `shaders/postfx_underwatercaustics.frag:75-88`
- **Problem**: (a) `ssao_blur`: 16 Taps für eine 4×4-Box (`x,y ∈ [-2,2)`), mit linearem Sampler und Pixel-Offsets exakt durch 4 bilineare Taps bei halben Texelversätzen (±1) ersetzbar. (b) `gi_atrous`: `pow(max(dot(...),0), 32.0)` pro Tap (25×) – `x^32` per fünf Quadrierungen; zusätzlich holt jede Tap Position und Normale getrennt. (c) `postfx_bloom_blur`: 9-Tap-Gauß → 5 Taps mit Linear-Sampling-Trick. (d) `taa_resolve`: 9× `length(v)` zur Maximum-Suche → `dot(v,v)` vergleichen. (e) `postfx_underwatercaustics`: Winkel-/`cos`/`sin`-Berechnung je Train und Pixel aus `const`-Array (6×) – kann der Compiler bei Unroll falten, sonst Vorberechnung in Push-Constants.
- **Code-Auszug**:
```glsl
for (int x = -2; x < 2; ++x) for (int y = -2; y < 2; ++y) sum += texture(uAOInput, vUV + vec2(x, y) * texel).r;   // ssao_blur.frag:14-16
w *= pow(max(dot(n, normalize(tap(uGNorm, uv).xyz)), 0.0), 32.0);                                              // gi_atrous.frag:75
```
- **Lösung**: wie oben aufgeführt; jeweils nur mit GPU-Timer-Nachweis.
- **Erwarteter Effekt**: Kleine Gewinne pro Pass (Schätzung: ssao_blur ~4× weniger Fetches; übrige < 5 %); in Summe vermutlich unter der Messschwelle – bewusst als „nur bei Bedarf“ geführt.
- **Risiko / Verifikation**: (a) ist exakt nur bei `LINEAR`-Sampler und gleicher Auflösung von Ein-/Ausgabe (Texel-Mittelpunkte) – sonst Abweichung; (b)-(d) geringfügige Rundungsunterschiede. Bildvergleich Vulkan; D3D/GL/Metal-Twins (`kSmaaFS`, `kGiAtrousFS`, `giShadowAtrous`) parallel ändern.

---

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/Backends/{D3D11,D3D12,Vulkan}/…Renderer.cpp`: `resolveShaders` im Draw-Loop (siehe REN-1001), `recordDraw`-Konsumenten lesen `dc.instanceTransforms`/`boneMatrices` (REN-1002/1003) – Umstellung betrifft alle fünf Backends; `ShadowPass`-Sinks in D3D11 (`:6597-6640`), D3D12 (`:10607-10614`), GL (`:10992-11001`) verwerfen `cmds` (REN-1004).
- `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp`: `resolveWorldMaterialScalars` an drei Stellen pro Frame (`:3890`, `:7838`, `:11495`); CPU-Seite der 128-B-Instanzdaten (REN-1009) dort lesen; `scene.frag`-Himmel analytisch (REN-1008).
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp` (`kUnlitFS`), `Metal/MetalRenderer.mm` (`fragmentMain`), `D3D*/…` (`kSceneHLSL`/`GI_PROBE_OCT`-Blöcke) und `D3D_Shared/HlslSources.h`: wahrscheinlich dieselbe Reihenfolge „Schatten vor Dämpfungstest“ wie in heLitP/scene.frag (REN-1007) – nicht gelesen, per Grep `localShadowFactor` prüfen.
- `src/HE_Core/include/ContentManager/Assets.h` (`MaterialAsset`): Hash der Custom-Shader-Quellen vorhalten (REN-1001).
- `src/HE_Scene/src/WaterQuery.cpp` (`WaterIndex::build`): Cache nutzlos bei Neubau pro Aufruf (REN-1021); `EngineApi.cpp:1578` hat dieselbe Nutzung.
- `src/HE_Rendering/src/Backends/*`: `OcclusionCuller::refine` nur in GL (`:10929`) und Metal (`:17907`) verdrahtet; D3D11/D3D12/Vulkan rufen es nicht auf (Feature-Parität bzw. Absicht dokumentieren).
- `include/HorizonRendering/LightPacking.h:27` (`kMaxLightWindow`) und `RenderConstants.h:14` (`kShadowMapResolution`) sollten die einzigen Quellen für 8 bzw. 2048 sein (REN-1017).

## Unsicher / Messung nötig

- Anteil der Meshes mit `sections.size() == 1` (bestimmt den Gewinn aus REN-1005).
- Tatsächliche Zahl Graph-Material-Draws pro Frame und Kosten von Kopie+Hash (REN-1001) – Profiler-Scope um `resolveShaders`.
- D3D12-Sinkcode jenseits Zeile 10614: liest der Shadow-Zweig wirklich nie `cmds`? (REN-1004).
- Verbleibende Bake-Zeit von `BuildSkyNoise3D(256)` nach der Jitter-Tabelle und Anzahl der Renderer-Instanzen im Editor (REN-1015).
- Ob die GL/Metal/D3D-Fassungen von heLitP/`localShadowFactor` ebenfalls vor dem `atten`-Test auswerten (REN-1007) und ob der Compiler `atten==0` per Dynamic Branching ohnehin ausspart (Treiberabhängig; GPU-Profil).
- Sichtbarkeit des Lese/Schreib-Hazards in `gi_probe.comp` (REN-1023); Verhalten der GL/Metal-Kernel nicht geprüft.
- Wird `std::hash<std::string>` plattformabhängig (libstdc++ Murmur vs. MSVC FNV) als Cache-Schlüssel in persistenten Caches benutzt? (REN-1001/1018 – bisher nur In-Process-Caches gesehen.)
- Sync-Kompilierung beim ersten Draw (`MaterialShaderLibrary::fragment` ruft glslang+SPIRV-Cross inline, D3D11 `:7019` „else cross-compile now“): Backends haben Prewarm-Pfade (`D3D11:5318`, `Vulkan:7669`), Restfälle (Material wird erst zur Laufzeit geladen) können Frame-Hitches erzeugen – messen.
- `MaterialShaderLibrary` ist nicht thread-safe, und `clear()` invalidiert die zurückgegebenen `const Compiled&`-Referenzen (Hot-Reload während Pipelines gebaut werden?) – Nutzung aus mehreren Threads nicht geprüft.

## Gelesen

Alle 55 Dateien des Slice 10 vollständig gelesen (inkl. `MaterialShaderLibrary.cpp` 2768 Z. in drei Blöcken und `RenderExtractor.cpp` 1590 Z. in zwei Blöcken), dazu die in der Slice-Liste fehlenden sechs Shader `gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl` (nur Kopf + Hauptteil, Zeilen 1-60 und 120-252), `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (nur per Grep auf Schlüsselstellen, nicht vollständig); außerdem gezielt `CommandBuffer.h/.cpp`, `RenderObject.h`, `RenderSorter.h`, `RenderGraph.h`, `OcclusionCuller.h` (Defaults), `WaterQuery.cpp`, `EntityActive.*` und Backend-Aufrufstellen (nur Ausschnitte).
