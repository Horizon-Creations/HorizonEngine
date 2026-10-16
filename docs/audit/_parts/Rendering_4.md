# Rendering Slice 4 – src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm (18158 Zeilen)

## Zusammenfassung

`MetalRenderer.mm` ist funktional ausgereift und an vielen Stellen bereits performance-bewusst (Upload-Ring, Instancing-Pfade, Einmal-Extract pro Frame, memoisierte Mesh-/Material-Auflösung im Draw-Loop, parallele Sky-Env-Bake). Die Datei besteht zu rund 6350 Zeilen aus eingebetteten MSL-Strings (Z. 238–6367), danach ~11800 Zeilen Objective-C++-Host-Code. Hauptprobleme: (1) eine Reihe von **Pro-Frame-Heap-/Metal-Objekt-Allokationen** außerhalb des vorhandenen Upload-Rings (Skinning-Bone-Puffer, GI-TLAS samt Hilfspuffer, Partikel-/Ribbon-/Preview-Puffer), (2) **blockierende CPU-GPU-Syncs** (`waitUntilCompleted` im BLAS-Build und in jeder Preview/Thumbnail), (3) **O(n)-/O(n log n)-Arbeit pro Shadow-Layer und pro UI-Quad**, die sich einmal pro Frame erledigen ließe, und (4) **massive Code-Duplikation** (G-Buffer- vs. Forward-Draw-Loop ~250 Zeilen, 6× Extract-Block, 3× SSR-Blur, 7× identische MSL-Hilfsfunktionen, 168 händische `CFBridgingRelease`-Blöcke) und eine 1000-Zeilen-Funktion `EncodeFrame`.
Funde: 8 Hoch, 17 Mittel, 4 Niedrig (29 gesamt). Alle Performance-Schätzungen sind ohne Messdaten und als Schätzung gekennzeichnet; Verifikation jeweils über `HE_PROFILE_SCOPE_N`/Metal-System-Trace und die vorhandenen Headless-Captures (`HE_SKY_TIME`, he_shot).

---

## Funde (nach Schweregrad)

### REN-401 – Sky-Env-Cube: 98k CPU-Atmosphärenintegrale bei jeder Sonnenbewegung
- **ID**: REN-401
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13811–13840 (`UpdateSkyEnvCube`), Aufrufer 14107 (`EncodeScene`) und 15015 (`EncodeDeferredResolveTile`)
- **Problem**: Der Cube wird neu gebacken, sobald sich `sunDir` um mehr als `1e-4` verändert. Bei aktivem Tag-Nacht-Zyklus (Auto-Advance) ist das **jeder Frame**. Pro Bake werden 6×128×128 = 98 304 Texel mit `HE::SkyColorCPU` ausgewertet (je 12×5-Schritt-Integral, ~216 `exp`), dazu ein 1,5-MB-`std::vector<float>` frisch allokiert und per `replaceRegion` (synchron, Shared-Storage, CPU→GPU) hochgeladen. Der eigene Kommentar nennt ~60 ms seriell; `parallel_for` verteilt das nur auf die Jobs, blockiert aber den Render-Thread bis zum Ende. Der Cube dient nur als diffuse/grob-spekulare IBL (Sample mit `Nup`/`Rrough`), 128² ist dafür deutlich überdimensioniert.
- **Code-Auszug**:
```cpp
if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 1e-4f) return;
...
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(static_cast<size_t>(6) * N, [&](size_t idx){ ... HE::BuildSkyEnvFaceRow(...) }, "SkyEnvBake", 1);
```
- **Lösung**: (a) Toleranz auf ~2e-3 (≈0,1°) anheben – für Ambient unsichtbar; (b) Auflösung auf 32² senken (16× weniger Arbeit) oder (c) den Bake auf die GPU legen: `EncodeSkyReflCube` (13771) zeigt bereits, wie der Sky-Pass in Cube-Faces rendert; mit `useSkyLut=true` ist das billig und braucht keinen CPU-Upload. (d) Mindestens: `px` als Member-Scratch wiederverwenden und eine Face pro Frame zeitlich verteilen.
- **Erwarteter Effekt**: bei laufendem Tag-Nacht-Zyklus ca. 1–10 ms CPU/Frame weniger (Schätzung, abhängig von Kernzahl); bei 32²/Toleranz-Anhebung >90 % weniger Arbeit.
- **Risiko / Verifikation**: Ambient-Farbübergänge dürfen nicht stufig werden (Toleranz nicht zu groß; Bake-Intervall mit Interpolation zwischen zwei Cubes lösbar). Vergleich per `HE_SKY_TIME`-Headless-Captures vor/nach; GL-Backend hat dieselbe Stelle (`OpenGLRenderer.cpp:4226`) – Ergebnis soll backend-gleich bleiben (`SkyEnvBake.h`).

### REN-402 – Skinning: pro Objekt und Frame ein neuer 8-KB-`MTLBuffer` samt Identity-Fill
- **ID**: REN-402
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13930–13949 (`EncodeSkinnedObjects`); gleiches Muster 10840–10846 (`RenderWorldPreview`)
- **Problem**: Für jedes `SkinnedRenderObject` wird `boneScratch` (128 × `mat4` = 8 KB) komplett mit Identity überschrieben, die echten Bones darüber kopiert und danach `newBufferWithBytes` (Kernel-Allokation + Memcpy + ARC-Objekt) aufgerufen. Bei 100 Charakteren sind das 100 Metal-Buffer pro Frame (~800 KB Memcpy, ~800 KB Fill), obwohl ein Upload-Ring (`UploadTransientBytes`) für exakt diesen Zweck existiert und an 15+ anderen Stellen genutzt wird.
- **Code-Auszug**:
```cpp
std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
if (boneCount > 0) std::copy_n(obj.boneMatrices.begin(), boneCount, boneScratch.begin());
id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
                                            length:kMaxBones * sizeof(glm::mat4) ...];
```
- **Lösung**: `UploadTransient(max(boneCount,1) * sizeof(mat4))`, Bones direkt in den Ring-Slice schreiben (nur `boneCount` Matrizen, kein Identity-Fill nötig, solange Vertex-Bone-IDs < boneCount; sonst eine einzige Identity am Ende), `setVertexBuffer:offset:`. Pro Objekt ein Slice, pro Section dieselbe Bindung.
- **Erwarteter Effekt**: pro Charakter ~1 Metal-Allokation und ~16 KB Speicherverkehr weniger; bei 50–200 Skinned-Objekten spürbar (Schätzung 0,2–1 ms CPU/Frame).
- **Risiko / Verifikation**: Shader `skinnedVertex` indexiert `boneMats[ids.x]` ohne Bounds-Check: Mesh-Assets mit Bone-IDs ≥ `boneCount` würden aus dem Slice lesen (vorher Identity). Entweder `max(boneCount, maxBoneIdOfMesh+1)` allokieren/auffüllen oder Assets beim Laden validieren. Test: Skinned-Szene vor/nach per Headless-Screenshot vergleichen.

### REN-403 – UI-Material-Quads: pro Quad und Frame GLSL-Kopie, Hash und Volltextsuche
- **ID**: REN-403
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13272–13283 (`GetOrBuildUIMaterialPipeline`), Aufruf 13508–13509 (`EncodeUIPass`); Quelle `MaterialShaderLibrary.cpp:1000–1008` (`resolveFragment`)
- **Problem**: Der Cache-Lookup (`m_uiMaterialPipelines.find(key)`) steht *nach* `ResolveMaterialShader`. Dieses kopiert den kompletten `customShaderFragGlsl` (typisch Zehntausende Bytes) in einen `std::string`, hasht ihn komplett (`std::hash<std::string>`) und anschließend läuft `fragGlsl.find("heBackdrop")` über denselben String – für **jeden UI-Quad mit Material in jedem Frame**. Ein HUD mit 50 Material-Quads erzeugt damit 50 Heap-Kopien + 50 volle Hash-/Suchläufe pro Frame. Im Scene-Loop ist das durch `lastMatId`-Memoisierung gedämpft; im UI-Pfad nicht.
- **Code-Auszug**:
```cpp
uint64_t key = 0; std::string fragGlsl, vertBody;
if (!ResolveMaterialShader(materialId, key, fragGlsl, vertBody)) return nullptr;
if (usesBackdrop) *usesBackdrop = fragGlsl.find("heBackdrop") != std::string::npos;
if (auto it = m_uiMaterialPipelines.find(key); it != ...) return it->second;
```
- **Lösung**: Cache nach `materialId` (UUID) statt nach Source-Hash: `unordered_map<UUID, {void* pso; bool backdrop;}>`; Invalidierung über das vorhandene `InvalidateMaterial`. Die Source nur auf Cache-Miss auflösen. Zusätzlich pro UI-Pass die letzte Material-ID memoisieren (Quads kommen in Baumreihenfolge).
- **Erwarteter Effekt**: UI-CPU-Kosten für Material-Quads gehen von O(Quads × GLSL-Länge) auf O(Quads) (Schätzung: 10–100 µs pro Quad gespart).
- **Risiko / Verifikation**: Material-Hot-Reload muss den Cache invalidieren (`InvalidateMaterial` erweitern; heute löscht es nur `m_materialTexCache`, nicht die Pipeline-Caches – bestehendes Verhalten beibehalten, aber testen: Material im Editor ändern → UI-Quad zeigt neue Version). Test: Widget-Test mit Material-Quad vor/nach.

### REN-404 – GI-Accel-Build: pro Frame lineare BLAS-Suche, Material-Auflösung und Transform-Inverse für jedes Objekt
- **ID**: REN-404
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8427–8465 (HW), 8236–8254 (SW), `giInstanceShading` 8186–8195
- **Problem**: Bei aktivem GI/GI-Reflexionen läuft pro Frame über **alle** Caster: (a) lineare Suche `for (u < uniqueBlas.size()) if (uniqueBlas[u] == blas)` → O(Objekte × einzigartige Meshes); bei 20k Objekten und 500 Meshes ≈ 10 M Vergleiche/Frame; (b) `giInstanceShading` → `HE::giInstanceSurface(obj, cm)` löst pro Objekt Material/Textur über den ContentManager auf (Hash-Lookups), obwohl sich Material und Farbe nur selten ändern; (c) im SW-Pfad zusätzlich `glm::inverse(obj.transform)` (4×4-Inverse) pro Objekt. Alles läuft auch dann, wenn sich in der Szene nichts bewegt hat.
- **Code-Auszug**:
```cpp
for (size_t u = 0; u < uniqueBlas.size(); ++u)
    if (uniqueBlas[u] == blas) { idx = (int)u; break; }
...
giInstanceShading(obj, m_contentManager, albedo, emissive, metallic, roughness);
```
- **Lösung**: `std::unordered_map<void*, uint32_t>` (BLAS-Zeiger → Index, pro Frame `clear()` mit behaltener Kapazität) statt linearer Suche. Instanz-Shading in einen Cache pro (`entityId`, Material-Revision) auslagern bzw. das Ergebnis in `RenderObject` vom Extractor mitliefern lassen (dort gibt es ohnehin Dirty-Information). SW: `glm::inverse` nur bei geänderter Transform (Hash/Vergleich gegen Vorjahres-Matrix) oder affine Schnell-Inverse.
- **Erwarteter Effekt**: O(n·u) → O(n); bei 10k+ Objekten mehrere ms CPU/Frame (Schätzung).
- **Risiko / Verifikation**: Reihenfolge der `uniqueBlas` muss stabil bleiben (TLAS-`instancedAccelerationStructures`-Array). Test: `tests/test_gi_bvh.cpp` deckt nur die BVH ab – GI-Szene vor/nach per Capture vergleichen, Instanz-IDs (`get_committed_instance_id`) müssen identisch bleiben.

### REN-405 – GI-TLAS wird jedes Frame komplett neu allokiert und gebaut (kein Dirty-Check)
- **ID**: REN-405
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8335–8576 (`EncodeGIAccelBuild`), 8266–8282 (SW), 8295–8333 (`BuildGILandscapeTable`)
- **Problem**: In jedem GI-aktiven Frame werden TLAS (`newAccelerationStructureWithSize`), Instanz-Puffer, Farb-Puffer, Landscape-Index-Puffer, Mesh-Ptr-Puffer, Mesh-Index-Puffer und ein Private-Scratch-Puffer (`newBufferWithLength`) neu erzeugt, die alten via `RetireGIObject` 3 Frames später freigegeben; die Instanz-Descriptoren werden pro Instanz mit 12 skalaren Schreibzugriffen befüllt. Für statische Szenen ist das reine Verschwendung: der TLAS-Inhalt ist bit-identisch. Zusätzlich verwaltet `m_retiredGIObjects` 6+ Einträge/Frame.
- **Code-Auszug**:
```cpp
id<MTLAccelerationStructure> tlas = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
id<MTLBuffer> scratch = [device newBufferWithLength:... options:MTLResourceStorageModePrivate];
...
RetireGIObject(m_giTlas); m_giTlas = nullptr; RetireGIObject(m_giInstanceBuffer); ...
```
- **Lösung**: (1) Szenen-Signatur (Anzahl Caster, Summe der Transform-/Material-Revisionen, `GIProbeSceneSignature` existiert bereits in `EnsureGIProbeGrid`) bilden; bei unverändertem Wert TLAS/Puffer des Vorjahres weiterverwenden. (2) Bei Änderung: persistenten Scratch (wachsend) und ein 3-fach-Ring der Instanz-/Farb-Puffer verwenden; `refitAccelerationStructure:` statt Neubau, wenn nur Transforms wechseln (Instanzanzahl/BLAS-Set gleich). (3) Instanz-Transform per Memcpy statt 12 Einzelzuweisungen.
- **Erwarteter Effekt**: bei statischen GI-Szenen ~6 Metal-Allokationen + TLAS-Build pro Frame eingespart (GPU- und CPU-Zeit; Schätzung 0,3–2 ms bei mittleren Szenen, mehr bei großen).
- **Risiko / Verifikation**: Hoch – das In-Flight-GPU-Race ist der Grund für die Frisch-Allokation (Kommentar am Member). Ring über 3 Frames oder Completion-Handler-Fence nötig. Verifikation mit Metal API Validation + `HE_GI_FORCE_SW=1`/HW-Capture. Test: GI-Capture einer bewegten und einer statischen Szene.

### REN-406 – `BuildBLAS`: blockierendes `waitUntilCompleted` pro neuem Mesh auf dem Render-Thread
- **ID**: REN-406
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8122–8169 (`BuildBLAS`), Aufruf 8439 in `EncodeGIAccelBuild`
- **Problem**: Jedes erstmals gesehene Mesh bekommt einen eigenen Command-Buffer, wird committed und der Render-Thread wartet synchron auf die GPU. Beim Laden einer Szene mit N Meshes entstehen N serielle CPU-GPU-Roundtrips innerhalb eines Frames (Hitch von N × (Submit-Latenz + Build-Dauer)). Zusätzlich neuer Scratch-Puffer pro BLAS.
- **Code-Auszug**:
```cpp
[cmdBuf commit];
// One-shot, lazy ... waiting here keeps the BLAS fully built before any TLAS references it
[cmdBuf waitUntilCompleted];
```
- **Lösung**: BLAS-Builds in den Frame-Command-Buffer (`cmdBufPtr` ist in `EncodeGIAccelBuild` vorhanden) auf einen gemeinsamen `accelerationStructureCommandEncoder` legen und einen gemeinsamen, wachsenden Scratch-Puffer verwenden (Builds seriell mit Offset). Metal ordnet Encoder desselben Command-Buffers; der TLAS-Build (gleicher Buffer, späterer Encoder) sieht die fertigen BLAS. Kein Wait nötig.
- **Erwarteter Effekt**: Szenenladen/Mesh-Streaming ohne N Pipeline-Stalls; Hitch skaliert nicht mehr mit Mesh-Anzahl (Schätzung: von mehreren 10–100 ms auf <5 ms CPU).
- **Risiko / Verifikation**: Die BLAS-Objekte werden während des Frames erzeugt und in `mesh.blas` gespeichert, bevor die GPU sie gebaut hat – andere Passes im selben Frame, die `mesh.blas` lesen (nur GI), kommen später im selben Command-Buffer, also korrekt. Mit Metal Validation prüfen; Test: Szene mit vielen neuen Meshes laden, Frame-Zeit im Profiler (`Metal::EncodeGIAccelBuild`).

### REN-407 – Shadow-Pass: Cull **und** Sort pro Layer (bis zu 3 Kaskaden + 16 Lokal-Layer)
- **ID**: REN-407
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:7960–7990 (`encodeDepthLayer` in `EncodeShadowMap`); Sorter `RenderSorter.cpp:12–46`
- **Problem**: Für jeden Layer läuft `m_culler.cull(viewProj)` **und** `m_sorter.sort(...)`. Der Sort ordnet nach (Mesh-ID, **Kamera**-Abstand) – der Schlüssel ist für alle Layer identisch (Kamera ändert sich nicht), nur die Sichtbarkeitsmenge variiert. Bei 3 Kaskaden + z. B. 8 Lokal-Layern sind das 11 × O(n log n) auf allen sichtbaren Objekten (SortKey-Aufbau, `std::sort`, Index-Kopie). Der Kommentar in `RefineObjectBounds` (7898) beziffert eine einzelne O(n)-Schleife mit ~3,5 ms bei 50k Entities; Cull+Sort ist teurer.
- **Code-Auszug**:
```cpp
auto encodeDepthLayer = [&](...) {
    m_culler.cull(m_renderWorld, viewProj, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
```
- **Lösung**: Einmal pro Frame alle Objekte nach (Mesh, Distanz) sortieren (nur für `castsShadow`); pro Layer nur noch das Sichtbarkeits-Bitset per Cull berechnen und die vorsortierte Liste linear filtern (O(n) statt O(n log n)). Das Ergebnis ist identisch geordnet. Alternativ Cull-Jobs der Kaskaden parallelisieren (JobSystem). Das Vulkan-Backend hat dieselbe Struktur (`VulkanRenderer.cpp:7007–7009`) – Lösung kann im Shared-Code (`RenderSorter`) liegen.
- **Erwarteter Effekt**: Sort-Anteil der Shadow-Pass-CPU-Zeit nahezu eliminiert; bei vielen Local-Lights/50k Objekten mehrere ms/Frame (Schätzung).
- **Risiko / Verifikation**: Verhalten identisch, solange die Filterung die Sortierreihenfolge erhält. `batchDepthCasters` setzt Mesh-Gruppierung der Eingabe voraus – bleibt erfüllt. Test: Shadow-Capture bitgleich; Unit-Test für `RenderSorter` mit Teilmengen.

### REN-408 – `EncodeGBuffer` ist eine ~250-Zeilen-Kopie der Forward-Opaque-Schleife in `EncodeScene`
- **ID**: REN-408
- **Kategorie**: A
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: MetalRenderer.mm:14375–14683 (Forward) vs. 16179–16457 (G-Buffer); dazu die Vor-Blöcke 14229–14240 vs. 16124–16157
- **Problem**: Die Memoisierungs-Variablen (`lastMeshId`, `cMaterialPipeline`, `cGraphTex`…), die Material-/Shader-/Textur-Auflösung, die dense-Vector-Pointer-Reparatur nach `ResolveMesh`, `drawInstance`-Lambda, Transparent-Routing, Param-Padding (`float padded[64]`), WPO-Binds, Weightmap-Bind und der Instancing-`fits`-Block existieren zweifach mit nur kleinen Abweichungen (G-Buffer-Pipeline, `forwardOpaque`-Routing). Änderungen (z. B. das Weightmap-Binding oder der `dense vector`-Fix) müssen doppelt gepflegt werden – der Kommentar „the G-buffer twin of the forward pass's re-take" belegt die Drift-Gefahr bereits.
- **Code-Auszug**:
```cpp
HE::UUID  lastMeshId{}; const GpuMesh* cMesh = nullptr; bool meshValid = false;
HE::UUID  lastMatId{};  bool matValid = false;
void*     cOverrideTex = nullptr; bool cHasOverride = false;
...   // identisch in EncodeScene (14384ff) und EncodeGBuffer (16184ff)
```
- **Lösung**: Einen `struct DrawLoopState` (Memo-Zustand) plus `ResolveDrawMaterial(state, dc)` und `ResolveDrawMesh(state, dc)` extrahieren; die Pass-spezifischen Teile (Ziel-Pipeline: `defaultPipeline` vs. `gbufferPipeline`, Routing nach `forwardOpaque`/`transparent`) als kleine Policy-Funktion/Lambda übergeben. Der `fits`-Instancing-Block nimmt zwei Pipelines als Parameter. Reihenfolge: erst die Auflösungs-Helfer (reiner Refactor), dann die Draw-Emission.
- **Erwarteter Effekt**: ca. 200–250 Zeilen weniger, ein Ort für künftige Material-Features; keine Laufzeitänderung.
- **Risiko / Verifikation**: Verhalten muss bit-identisch bleiben (Headless-Captures Forward+Deferred+Tile, `HE_RENDER_PATH=1`, `HE_MTL_INSTANCING=0/1`). Keine Tests im Repo für die Metal-Draw-Loops → Capture-Vergleich ist Pflicht.

### REN-409 – `EncodeFrame` (≈1000 Zeilen) mit verschränktem Detailed-Capture-Pfad
- **ID**: REN-409
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: MetalRenderer.mm:16461–17455
- **Problem**: Eine Funktion enthält Frame-Setup, Invalidierungen, GPU-Timer-Setup (zwei Modi + Fallback), Shadow/GI/SSAO/G-Buffer/HDR/Post/Tonemap/AA/UI/Swapchain und drei Varianten der Completion-Handler. `flushPass(...)`-Aufrufe (7×) und `curPass`-Zustand verzahnen den Debug-Modus „detailed" mit dem normalen Pfad; die verschachtelten Lambdas `attachDetail`/`flushPass` und mehrere `static`-Zustände (`s_lastSceneW`, `s_lastMode`, `s_loggedDetailed`) machen Kontrollfluss und Lebensdauer schwer prüfbar.
- **Code-Auszug**:
```cpp
auto flushPass = [&](const char* nextPass) { if (!detailed) return; attachDetail(cmdBuf, curPass); [cmdBuf commit]; [cmdBuf waitUntilCompleted]; ...};
flushPass("GIAccel"); ... flushPass("GIShadow"); ... flushPass("Present");
```
- **Lösung**: Aufteilen in `BeginFrame()` (Invalidierungen, Retire-Aging), `SetupGpuTiming()` (liefert ein `FrameTiming`-Objekt mit `flush(next)` und `finish(cmdBuf)`), `EncodeShadowStage()`, `EncodeGIStage()`, `EncodeSceneStage()` (SSAO/G-Buffer/HDR), `EncodePostStage()` (DoF/UW/MB/Bloom/MFX/Tonemap/TAA/AA), `EncodePresent()`. Das `FrameTiming`-Objekt kapselt Detailed-vs-Counter-vs-Whole-Frame in einer Klasse statt drei `else if`-Ästen am Funktionsende.
- **Erwarteter Effekt**: bessere Lesbarkeit/Testbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Mittel – Reihenfolge der Encoder muss exakt bleiben (Kommentare zu Ordnungsabhängigkeiten: SSAO vor Tile-Pass, Velocity nach G-Buffer). Capture-Vergleich Forward/Deferred/Tile + Profiler-Pass-Namen unverändert.

### REN-410 – `CreateScenePipeline`: 625-Zeilen-Monolith, ~35× identischer Fehlerblock, alle Libraries eager beim Start
- **ID**: REN-410
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: MetalRenderer.mm:7199–7824 (`CreateScenePipeline`), `Initialize` 6609–6692
- **Problem**: (1) Jede der ~16 Libraries/~30 Pipelines wiederholt Schema „`newLibraryWithSource` → `if (!lib) throw runtime_error(std::string(...) + (err ? [[err localizedDescription] UTF8String] : "unknown"))` → Descriptor-Aufbau → PSO → `CFBridgingRetain`". (2) Alle Libraries – inkl. des enormen `kSkyMSL` (Wolken, Nebel, Aurora, ≈2100 Zeilen) sowie Tonemap, FXAA/SMAA/TAA, DoF, Motion Blur, Bloom, SSAO, UI – werden **synchron beim Start** auf dem Main-Thread kompiliert, auch wenn das Feature (z. B. DoF, Motion Blur, TAA, SMAA) nie genutzt wird. Die GI-/SSR-/Underwater-Pfade machen es bereits richtig (lazy `Ensure*`).
- **Code-Auszug**:
```cpp
throw std::runtime_error(std::string("MetalRenderer: bloom bright pipeline creation failed: ")
    + (blError ? [[blError localizedDescription] UTF8String] : "unknown"));
```
- **Lösung**: Hilfen `MakeLibrary(src, label)` und `MakePso(desc, label)` (werfen bzw. loggen zentral) sowie ein kleines `PipelineDesc`-Builder-Lambda für Fullscreen-Passes (`vertex`, `fragment`, `colorFormat`, optional `depthFormat`, Blend-Preset). Rarely-used-Gruppen (DoF, MotionBlur, TAA/SMAA, Underwater, UI-Materialien, Cloud-Shadow, Sky-LUT) lazy beim ersten Gebrauch bauen (`Ensure*Pipelines()` Muster aus Z. 15187). Startzeit und Initialisierungs-Peak-Speicher sinken.
- **Erwarteter Effekt**: ~300–400 Zeilen weniger; Startzeit des Renderers sinkt um die Compile-Zeit ungenutzter Libraries (Schätzung 50–300 ms, **messen**).
- **Risiko / Verifikation**: Lazy-Build verlagert Compile-Hitches in den ersten Frame mit Feature → bei Post-FX-Toggle im Editor kurzer Hitch (Warmup-Option). Tests: Initialisierung + jedes Feature einmal einschalten (Headless).

### REN-411 – UI-Quads: ~12 `setBytes`-Aufrufe pro Quad, String-Schlüssel pro Textur-Quad
- **ID**: REN-411
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13583–13654 (`EncodeUIPass`), 9533–9543 (`ResolveUITexture`)
- **Problem**: Jeder Solid-/Glyph-Quad setzt 3 Vertex- und 9 Fragment-`setBytes` (jeweils eigene Command-Stream-Einträge) und führt einen eigenen Draw aus; Text mit 500 Glyphen = 6000 Encoder-Aufrufe + 500 Draws pro Frame. Textur-Quads bilden in `ResolveUITexture` pro Aufruf `std::to_string(hi) + ":" + std::to_string(lo)` (zwei Heap-Allokationen) für den Cache-Key.
- **Code-Auszug**:
```cpp
[enc setVertexBytes:&rect length:sizeof(rect) atIndex:0];  ... [enc setFragmentBytes:&innerCol length:sizeof(innerCol) atIndex:8];
...
const std::string key = texId != HE::UUID{} ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
```
- **Lösung**: Die 9 Fragment-Konstanten in **einen** `struct UIQuadFrag { float4 ... }` packen (ein `setFragmentBytes`, Shader entsprechend anpassen) bzw. alle Quads einer Clip-/Atlas-/Material-Gruppe als Array in einen Ring-Slice schreiben und mit `instanceCount` zeichnen (`instance_id` indiziert die Struct-Array). Cache-Key als `UUID` (Hash über `hi/lo`) – String nur für Pfad-only-Texturen.
- **Erwarteter Effekt**: Encoder-Overhead pro Quad ~10× kleiner; für UI-lastige Games messbarer CPU-Gewinn (Schätzung 0,2–1 ms bei 500 Quads).
- **Risiko / Verifikation**: Shader-Signatur (`kUIMSL`) und Host-Struct-Layout synchron halten (`static_assert`). UI-Widget-Tests (`tests/test_ui_widgets.cpp`, nur gezielt lesen) laufen headless gegen den Software-Renderer – Metal-Pfad per Capture prüfen.

### REN-412 – Velocity-Pass: Hash-Map-Knoten pro Objekt und Frame, Foliage-Cluster pro Instanz gezeichnet
- **ID**: REN-412
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:12945–13030 (`EncodeVelocity`), Member `m_taaPrevTransforms/m_taaCurTransforms` (Header 898–899)
- **Problem**: Bei TAA/MetalFX wird pro Objekt `m_taaPrevTransforms.find` + `m_taaCurTransforms[id] = transform` (Knotenallokation in `unordered_map<uint32_t, mat4>`) ausgeführt und die Map am Ende getauscht; `m_taaCurTransforms.clear()` gibt alle Knoten (je ~80 B) wieder frei → bei 20k Objekten ~20k malloc/free pro Frame. Dazu kommt ein Einzel-Draw (`setVertexBytes` 192 B + Draw) pro Objekt **ohne** die Instancing-Batches der anderen Passes, und Foliage-Cluster zeichnen `forEachInstance` **eine Instanz pro Draw** (mehrere tausend Draws pro Cluster möglich).
- **Code-Auszug**:
```cpp
const auto it = m_taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != end) ? it->second : obj.transform;
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache `std::vector<std::pair<uint32_t, mat4>>` (sortiert nach `entityId` oder Open-Addressing-Tabelle mit behaltener Kapazität) statt Knoten-Map. Velocity mit `batchDepthRuns` (wie SSAO/GI-Prepass) instanziieren: `{mvpJitter, mvpNow, mvpPrev}` (192 B/Instanz) in einen Ring-Slice, `velocityVertexInstanced`. Cluster-Instanzen statisch ebenfalls als Instanz-Array zeichnen.
- **Erwarteter Effekt**: Entfernt per-Frame-Heap-Churn und reduziert Draws auf die Anzahl Mesh-Gruppen (Schätzung: bei TAA + 20k Objekten 1–3 ms CPU).
- **Risiko / Verifikation**: Pass muss bit-identische Geschwindigkeiten liefern (Kommentar „entity-keyed history holds ONE pose per entity"). Capture-Vergleich mit TAA/MetalFX; Geister-Test bewegter Objekte.

### REN-413 – Cloud-Shadow-Pass: pro Frame O(n)-Szenen-AABB und vollständiges 512²-Re-Render
- **ID**: REN-413
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:11801–11888 (`EncodeCloudShadow`), Shader-Feld 4720–4822
- **Problem**: (a) Die Bodenhöhe wird über eine Schleife über **alle** `m_renderWorld.objects` (`sceneBox.expand`) ermittelt, nur um `groundY` (min) auf einem sehr grob hysteresierten Wert zu halten (±35 % Wolkenhöhe Totband). (b) Die Karte (512² = 262k Texel × 6 Marsch-Schritte × `cloudFieldDensity`, Realistic-Stil mit ≥12 3D-Textur-Fetches/Auswertung) wird **jeden Frame** neu gerendert, obwohl sie nur von Zeit, Wind, Sonne, Coverage und texel-gerasterter Kameraposition abhängt (Origin ist auf Texelgrenzen gerastert, ändert sich also selten).
- **Code-Auszug**:
```cpp
HE::AABB sceneBox;
for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
...
[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];  // jeden Frame
```
- **Lösung**: (a) Szenen-Bounds einmal pro Extract im `RenderExtractor` (oder zumindest nur alle N Frames / bei Objektanzahl-Änderung) berechnen und hier lesen – `RefineObjectBounds` hat die Daten bereits angefasst. (b) Karte nur bei Änderung neu rendern: Zeit-Quantisierung (z. B. alle 4–8 Frames bzw. wenn `|Δtime·evo|` einen Schwellwert überschreitet), sofort bei Änderung von Sonne/Wind/Coverage/Origin-Texel.
- **Erwarteter Effekt**: GPU-Zeit des Passes um 75–90 % reduzierbar; CPU-Schleife entfällt (Schätzung 0,05–0,5 ms CPU, GPU-Gewinn **messen**).
- **Risiko / Verifikation**: Schatten dürfen nicht sichtbar ruckeln (Interpolation zwischen zwei Karten oder Zeitquantisierung < Wolkenbewegung/Texel). Vergleich mit `HE_SKY_TIME`-Captures.

### REN-414 – Preview-/Thumbnail-Pfade warten pro Aufruf auf die GPU (auch ohne Readback)
- **ID**: REN-414
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:9997–9998 (`RenderMaterialPreview`), 10077–10078 (`CommitAndReadThumbnail`), 10599–10600 (`RenderSkeletalPreview`), 10906–10907 (`RenderWorldPreview`), 11134–11135 (`RenderParticlePreview`)
- **Problem**: `[cb waitUntilCompleted]` wird in den Live-Previews (Material-Editor, Skeletal-Editor, World-Preview, Particle-Preview) **in jedem Frame** unbedingt aufgerufen, obwohl die Texturen nur an ImGui weitergereicht werden und nur der Debug-Dump (`HE_*_PREVIEW_DUMP`, nur bei gesetzter Env-Variable) einen CPU-Readback braucht. Der Editor-Frame stallt damit pro offenem Preview-Panel auf Vorgänger-Latenz + GPU-Zeit. Content-Browser-Thumbnails (`RenderAssetThumbnail`) warten je Kachel einzeln.
- **Code-Auszug**:
```cpp
[cb commit];
[cb waitUntilCompleted];
if (staging) { ...Dump... }
return m_previewColorTex; // id<MTLTexture> for ImGui::Image
```
- **Lösung**: Wait nur wenn `staging` gesetzt ist (Dump-Pfad) bzw. für Thumbnails, die zwingend Bytes liefern. Live-Previews reihen sich auf derselben Queue vor dem ImGui-Pass ein – die Reihenfolge ist durch die Queue garantiert, kein Wait nötig. Thumbnails: Batch (mehrere Kacheln in einem Command-Buffer, ein Wait) oder asynchron per Completed-Handler mit Callback.
- **Erwarteter Effekt**: Entfernt einen CPU-GPU-Roundtrip pro Preview und Frame (editor-only; Schätzung 0,5–3 ms/Frame je offenem Preview).
- **Risiko / Verifikation**: Die Preview-Ziele sind Member-Texturen, die im nächsten Frame wiederverwendet werden: Da dieselbe Queue sequenziell abarbeitet, kein Hazard. Bei Größenänderung (`CFBridgingRelease` der Textur, 9945ff) muss die Texture-Retire-Logik (`RetireTexture`) statt sofortiger Freigabe genutzt werden, sobald nicht mehr gewartet wird – Test: Panel-Resize während Preview.

### REN-415 – Pro-Frame-`MTLBuffer`/`std::vector`-Allokationen außerhalb des Upload-Rings
- **ID**: REN-415
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 7183–7191 (`CollectRibbonDraws`), 17828–17833 (`DrawParticleGraphBatches`), 10942–10949 (`EncodeParticleBillboards`), 10787–10792 (World-Preview-Grid), 8266–8282 (SW-GI-Instanzen), 14649–14658 und 16426–16436 (`std::vector<glm::mat4> xf` pro instanziertem DrawCall)
- **Problem**: Die Datei hat mit `UploadTransient`/`UploadTransientBytes` (7032–7081) einen Ring für Frame-Daten, nutzt ihn aber für Ribbons (2 Puffer pro Batch), Particle-Graph-Batches (1 Puffer + `std::vector<float>` pro Batch), Preview-Partikel, World-Preview-Grid und SW-GI-Instanzen nicht. Im Opaque-/G-Buffer-Loop wird pro instanziertem `DrawCall` ein neuer `std::vector<glm::mat4> xf` mit `reserve` allokiert, dann per `UploadTransientBytes` ein zweites Mal kopiert.
- **Code-Auszug**:
```cpp
id<MTLBuffer> vbuf = [device newBufferWithBytes:rb.vertices.data() ...];
id<MTLBuffer> ibuf = [device newBufferWithBytes:rb.indices.data() ...];
m_ribbonBuffers.push_back((void*)CFBridgingRetain(vbuf));
...
std::vector<glm::mat4> xf; xf.reserve(instCount * 2);   // pro DrawCall
```
- **Lösung**: Für Ring-fähige Daten `UploadTransient` verwenden und direkt in den Slice schreiben (`float* ptr = UploadTransient(...)`, wie `EncodeDebugLines` 7001). Ribbons/Particle-Batches brauchen dafür `vbufOffset` in `TPDraw` (heute nicht vorhanden – `TPDraw` erweitern). `xf` als wiederverwendeten Member-Scratch oder direkt in den Ring-Slice schreiben, die Zwischenkopie entfällt.
- **Erwarteter Effekt**: Entfernt 1–3 Metal-Allokationen je Batch/Frame und eine Heap-Allokation je instanziertem Draw (Schätzung: kleine, aber kontinuierliche Entlastung; relevant bei Partikel-/Trail-reichen Szenen).
- **Risiko / Verifikation**: `m_ribbonBuffers`-Lebensdauer-Hack („Last frame's staging buffers go now") fällt weg, wenn der Ring den Lifetime-Job übernimmt. Capture-Vergleich Ribbons/Partikel.

### REN-416 – Transparenz-Replay: schwergewichtige `TPDraw`-Kopien, Sortierung großer Structs, `matLight` pro Draw neu gesendet
- **ID**: REN-416
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 6402–6408 (`TPDraw`), 14533–14547, 14709–14712, 14326, 14739–14754
- **Problem**: `TPDraw` hält `UnlitUniforms` (224 B), einen `std::vector<float> params` (Heap-Kopie des Material-Parameterblocks pro transparentem Draw: `t.params = *cMaterialParams`), ein `gtex[4]`-Array usw. (~330 B). `std::sort` verschiebt diese Structs O(n log n) statt Indizes. Im Replay und im `forwardOpaque`-Replay wird bei jedem Draw mit eigener Pipeline `setFragmentBytes(&matLight, sizeof(matLight))` gesendet – `HE::MaterialShaderLibrary::Lighting` ist >2 KB groß (u. a. `localShadowVP[16][16]` = 1 KB, `csmVP`), obwohl sich der Block innerhalb des Passes nicht ändert (Kommentar 14730: „matLight@1 is still bound"); WPO-Draws senden ihn zusätzlich als Vertex-Bytes.
- **Code-Auszug**:
```cpp
if (cMaterialParams) t.params = *cMaterialParams;
...
std::sort(transparent.begin(), transparent.end(), [](const TPDraw& a, const TPDraw& b){ return a.distSq > b.distSq; });
...
[encoder setFragmentBytes:&matLight length:sizeof(matLight) atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
```
- **Lösung**: `params` als Zeiger/Index in ein Frame-Arena (oder `shared_ptr<const vector>`) statt Kopie; Sortierung über `vector<pair<float,uint32_t>>`-Index. `matLight` nur beim Pipeline-Wechsel setzen, wenn der Block nicht ohnehin seit der letzten Bindung gültig ist (Buffer-Slot bleibt im Encoder bestehen, solange kein anderer Pass ihn überschreibt) oder einmal in den Ring legen und per `setFragmentBuffer:offset:` binden.
- **Erwarteter Effekt**: Weniger Heap-Allokationen und Speicherverkehr bei Transparenz-/Partikel-Szenen (Schätzung: 2 KB × Draws gespart; bei 500 transparenten Custom-Draws ~1 MB/Frame).
- **Risiko / Verifikation**: Zwischen Pipeline-Wechseln darf sich Buffer 1 nicht ändern – `BindMaterialPreambleSlots`/Sky-Pass überschreiben nicht Slot `kMetalLightingBufferIndex`, aber der Sky-Pass setzt Fragment-Buffers 0–2 → vor dem Replay einmal neu binden (heute schon der Fall, 14715).

### REN-417 – `ResolveGraphTexture` baut pro Aufruf einen String-Schlüssel (auch im Draw-Pfad)
- **ID**: REN-417
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:9511–9524; Aufrufer im Hot Path: 14588–14591 und 16370–16373 (pro Draw/Instanz, auch für `kDefaultLayer0WeightTextureId`), 13567 (pro UI-Material-Quad × 4 Slots), 14457/16253, 8312, 15169
- **Problem**: `key = std::to_string(texId.hi) + ":" + std::to_string(texId.lo)` (+ `"#arr"`) erzeugt drei String-Allokationen pro Lookup, danach Hash des Strings. Besonders ärgerlich: jeder Custom-Material-Draw ohne eigene Weightmap fragt pro Instanz zweimal (`dc.weightmapTextureId` leer → Default-Layer0-Textur), also zwei Lookups mit String-Bau pro Instanz-Draw.
- **Code-Auszug**:
```cpp
std::string key = texId != HE::UUID{} ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
if (array) key += "#arr";
if (auto it = m_graphTexCache.find(key); ...)
```
- **Lösung**: UUID-basierter Schlüssel: `unordered_map<GraphTexKey{UUID id; bool array}, void*>` (Hash über `hi^lo`), Pfad-only-Texturen in eine zweite Map. Default-Weightmap als gecachter Member-Zeiger (`m_defaultWeightTex`, bei `InvalidateTexture` zurücksetzen). Die Weightmap-Bindung pro Draw nur bei Änderung des Zeigers neu setzen.
- **Erwarteter Effekt**: Entfernt 3 Allokationen × Lookups pro Custom-Draw (Schätzung 100–300 ns pro Lookup; bei 5k Draws 1–3 ms/Frame).
- **Risiko / Verifikation**: Invalidierung (`m_pendingTexInvalidations`, 16545–16557) benutzt heute die String-Schlüssel `"hi:lo"` und `"#arr"` – muss mit der neuen Schlüsselform konsistent bleiben. Test: Weightmap-Paint (Terrain) aktualisiert die Textur sichtbar.

### REN-418 – Pipeline-Archiv wird bei jeder neuen PSO komplett neu serialisiert (synchron)
- **ID**: REN-418
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:12781–12795 (`GetOrBuildMaterialPipeline`), `WarmupMaterials` 9588–9626
- **Problem**: Nach jedem erfolgreich gebauten Material-PSO (pro Material mindestens opaque + blend, im Deferred-Modus +G-Buffer) wird `addRenderPipelineFunctionsWithDescriptor:` aufgerufen und danach `serializeToURL:` – die **gesamte** Archivdatei wird synchron auf dem Render-Thread neu geschrieben. Beim Warmup von M Materialien entstehen 2–3·M Serialisierungen eines wachsenden Archivs (O(M²) I/O). Die Datei-I/O liegt im Frame-Pfad (Ladezeit/Hitch).
- **Code-Auszug**:
```cpp
if ([arch addRenderPipelineFunctionsWithDescriptor:desc error:&aerr] && !m_matArchivePath.empty())
    [arch serializeToURL:[NSURL fileURLWithPath:@(m_matArchivePath.c_str())] error:nil];
```
- **Lösung**: Serialisierung entkoppeln: Dirty-Flag setzen und einmal pro Warmup-Ende bzw. mit Debounce (z. B. 2 s Idle) oder im `Shutdown()` auf einem Hintergrund-Job schreiben. `WarmupMaterials` ruft am Ende genau einmal `FlushMaterialArchive()`.
- **Erwarteter Effekt**: Warmup/Szenenöffnen mit vielen Materialien: von O(M²) Datei-I/O auf O(M); Hitch-Reduktion (Schätzung bei 100 Materialien Sekunden bis wenige 100 ms).
- **Risiko / Verifikation**: Absturz-Sicherheit (Kommentar 12598: `serializeToURL` ist auf macOS 26+ instabil und deshalb dort ganz abgeschaltet) – Hintergrund-Thread darf Metal-Archive nicht parallel zu `addRender…` anfassen; deshalb nur am definierten Punkt auf dem Render-Thread flushen. Test: Neustart lädt Archiv, `HE_MTL_ARCHIVE=1`.

### REN-419 – Mesh-Invalidierung gibt die Mesh-Textur nicht frei (Leak) und lädt sie bei jedem Edit neu
- **ID**: REN-419
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: MetalRenderer.mm:16516–16524 (Invalidierung), 9248–9267 (`ResolveMesh`: Textur-Upload), 6722–6728 (Shutdown gibt `mesh.texture` frei)
- **Problem**: `ResolveMesh` retained pro Mesh `vertexBuf`, `indexBuf`, `texture` (und ggf. `blas`). Bei `m_pendingMeshInvalidations` (Sculpting, Terrain-Edit) werden nur `vertexBuf`, `indexBuf`, `blas` mit `CFBridgingRelease` freigegeben; `it->second.texture` fehlt, obwohl `Shutdown()` (6727) sie korrekt freigibt. Jedes Invalidieren eines texturierten Meshes leakt damit eine `MTLTexture` (bis zu MB-Größe), und der nächste `ResolveMesh` lädt dieselbe Basis-Textur erneut hoch (`uploadMetalTexture`) statt sie zu behalten. Zusätzlich: Die Freigabe erfolgt sofort statt über `RetireTexture`/Frame-Alter, obwohl dieselbe Datei an anderen Stellen genau dafür Retire-Listen hat.
- **Code-Auszug**:
```cpp
if (it->second.vertexBuf) CFBridgingRelease(it->second.vertexBuf);
if (it->second.indexBuf)  CFBridgingRelease(it->second.indexBuf);
if (it->second.blas)      CFBridgingRelease(it->second.blas);
m_meshCache.erase(it);       // texture bleibt retained → Leak
```
- **Lösung**: `if (texture) RetireTexture(texture)` ergänzen **oder** besser: Texture beim Invalidieren im Cache behalten (nur Geometrie ersetzen) und beim Neu-Resolve wiederverwenden. Dasselbe gilt für `m_skeletalMeshCache`, falls dort invalidiert wird (derzeit nicht).
- **Erwarteter Effekt**: Beseitigt wachsenden VRAM-/Shared-Memory-Verbrauch bei Sculpt-Sessions; weniger Upload-Last pro Edit.
- **Risiko / Verifikation**: Niedrig. Test: Terrain-Sculpt-Session (100 Edits) + Speicher-Monitor/Instruments (Allocations) vor/nach.

### REN-420 – Gleicher Extract-/Setup-Block sechsmal kopiert; `matLight`-Fill in vier Varianten
- **ID**: REN-420
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: Extract-Block: 7881–7891 (`ExtractFrame`), 8374–8380, 12367–12374, 14009–14019, 16095–16103, 17073–17079; `matLight`-Fills: 13426–13444 (UI), 14785–14903 (`FillMaterialLighting`), 16130–16154 (G-Buffer)
- **Problem**: Die vier Zeilen `setDayNight(...)`/`setContentManager`/`extract(...)` existieren bereits als `ExtractFrame(aspect)`, werden aber in `EncodeGIAccelBuild`, `EncodeSSAO`, `EncodeScene`, `EncodeGBuffer` und im Low-Res-Cloud-Pfad von `EncodeFrame` inline wiederholt (Kommentar: „the established convention in this file"). Zusätzlich existieren drei Teil-Duplikate der Lighting-Fill-Logik (Dominant-Light, Camera, Wind/Weather/Water), die von `FillMaterialLighting` abweichen; `FillMaterialLighting` selbst läuft bis zu 3× pro Frame (EncodeScene 14235, EncodeDeferredResolveTile 15031, EncodeSSRPasses 15494) mit identischen Eingaben inklusive 16 lokaler Shadow-Matrizen, `glm::inverse(view)` und Cascade-Matrizen.
- **Code-Auszug**:
```cpp
m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay, env.sunColor, env.sunIntensity,
                        env.moonColor, env.moonIntensity, env.cloudCoverage);
m_extractor.setContentManager(m_contentManager);
m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
```
- **Lösung**: Alle Stellen auf `ExtractFrame(aspect)` (optional mit `extractUI`-Flag) umstellen. `FillMaterialLighting` einmal pro Frame in einen Member-Block (`m_frameMatLight`) schreiben und pro Pass nur die abweichenden Felder (`specAA[1]`, `ssr[...]`, Cluster) auf einer Kopie patchen. UI-/G-Buffer-Varianten als „Minimal-Profil" von derselben Funktion bedienen.
- **Erwarteter Effekt**: ca. 60–100 Zeilen weniger; gleiche Ergebnisse; weniger CPU für Matrix-Arbeit (Schätzung <0,1 ms/Frame).
- **Risiko / Verifikation**: `ExtractFrame` hat identischen Inhalt (7884–7890). Wegen des `FrameScope` (16469) ist der wiederholte `extract()` ein Key-Compare. Capture-Vergleich aller Pfade.

### REN-421 – SSR-Trace und Blur-Kette dreifach implementiert
- **ID**: REN-421
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 15366–15422 (Trace-Uniforms/Encoder in `EncodeSSRPasses`) vs. 15584–15637 (`EncodeForwardSSR`); `blurPass`-Lambda 15431–15449, 15643–15662, 15970–15989 (`EncodeGIReflections`)
- **Problem**: Die Befüllung von `SSRTraceUniforms` (15 Felder), die Temporal-Hist-Berechnung (4×4-Delta-Schleife) und das `blurPass`-Lambda stehen mit minimalen Abweichungen (Textur-Quellen) in drei Funktionen. Dazu zwei weitere unabhängige Kopien der Qualitäts-Tier-Logik (`m_ssrQuality <= 0 ? 16 : …`).
- **Code-Auszug**:
```cpp
auto blurPass = [&](void* src, void* dst, float dx, float dy) {
    HE::MaterialShaderLibrary::SSRBlurUniforms bu; bu.dir[0] = dx; ...
    MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor]; ...
```
- **Lösung**: Member-Funktionen `EncodeFullscreenBlur(cmdBuf, src, dst, dx, dy, texW, texH)` und `EncodeSsrTrace(cmdBuf, TraceInputs{colorTex, attrTex, depthTex, forwardColorMode})`; `SSRTraceUniforms` über eine `MakeSsrTraceUniforms(viewProj, hist, seed, mode)`-Funktion befüllen. Das Muster mit `fullscreenPass` aus `EncodeBloom` (11922) kann dasselbe Lambda für alle verwenden.
- **Erwarteter Effekt**: ~120 Zeilen weniger; ein Ort für Tier-Anpassungen.
- **Risiko / Verifikation**: bit-identische Captures mit SSR Q0/Q1/Q2, deferred + forward, GI-Refl Q0–Q2.

### REN-422 – Post-Chain: Vollbild-Kopien statt Ping-Pong, Bloom als 11 Einzel-Encoder
- **ID**: REN-422
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 13060–13069 (`EncodeTaa`: Blit `m_taaResolved → m_taaHistory`), 17113–17143 (Forward-SSR: Blit `m_hdrColor → m_ssrColorHist`), 11911–11963 (`EncodeBloom`)
- **Problem**: TAA kopiert jedes Frame das komplette LDR-Bild (BGRA8, sceneW×sceneH) in die History; Forward-SSR kopiert jedes Frame das komplette HDR-Bild (RGBA16F, ~8 B/Pixel → 16 MB bei 1080p). Beides wäre als Ping-Pong zweier Texturen kopierfrei. Bloom nutzt eine Kette 1 Bright + 10 Blur-Encoder auf Halbauflösung (5 H + 5 V), jeder als eigener Render-Pass mit Store/Load im DRAM; eine Downsample-Kette (Mips, Dual-Kawase) deckt dieselbe Reichweite mit weniger/ kleineren Passes ab.
- **Code-Auszug**:
```cpp
id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
[blit copyFromTexture:(__bridge id<MTLTexture>)m_taaResolved ... toTexture:(__bridge id<MTLTexture>)m_taaHistory ...];
```
- **Lösung**: TAA: zwei `m_taaResolved[2]`, History = Vorjahres-Resolved; Index flippen statt Blit. Forward-SSR: HDR-Ziel alternierend (`m_hdrColor[2]`) oder SSR liest direkt die Vorjahres-Scene-Color. Bloom: Kette auf 5–6 Passes über Mip-Pyramide umbauen (Qualität prüfen).
- **Erwarteter Effekt**: pro Frame ein Vollbild-Blit (1080p BGRA8 ≈ 8 MB / RGBA16F ≈ 16 MB Lese+Schreib) weniger; Bloom GPU-Zeit je nach Auflösung deutlich geringer (**messen**, Apple-TBDR-Bandbreite ist hier der Engpass).
- **Risiko / Verifikation**: Ping-Pong ändert Textur-Lebensdauern (Resize, MetalFX-Output, `m_taaHistoryValid`). Bloom-Umbau ändert das Bild → nur mit visuellem A/B.

### REN-423 – 168 händische `if (x) { CFBridgingRelease(x); x = nullptr; }`-Blöcke
- **ID**: REN-423
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 6694–6920 (`Shutdown`, ~150 Zeilen), `Destroy*`-Funktionen (8726, 11492, 11544, 11709, 12000, 12111, 12338, 13227, …), `void*`-Member in `MetalRenderer.h`
- **Problem**: Alle Metal-Objekte liegen als `void*` mit manuellem `CFBridgingRetain/Release` im Header (ARC-Bridging wegen C++-Header). `grep` zählt 168 solche Blöcke; ein vergessenes Feld in `Shutdown` oder `Destroy*` ist ein stiller Leak (Beispiel REN-419). `Shutdown` ist 227 Zeilen reine Freigabe.
- **Code-Auszug**:
```cpp
if (m_fxaaPipeline)  { CFBridgingRelease(m_fxaaPipeline);  m_fxaaPipeline = nullptr; }
if (m_aaBlitPipeline){ CFBridgingRelease(m_aaBlitPipeline);m_aaBlitPipeline = nullptr; }
```
- **Lösung**: Kleines `MtlHandle`-RAII (`struct MtlHandle { void* p; ~MtlHandle(){ if(p) CFBridgingRelease(p);} ... }`, move-only, `get()`) oder zumindest freie Funktion `static void ReleaseHandle(void*& p)`. Mit RAII entfällt `Shutdown` weitgehend (Destruktor), Reihenfolge-Abhängigkeiten (Device zuletzt) per Member-Deklarationsreihenfolge. Header-Änderung betrifft nur die Member-Typen.
- **Erwarteter Effekt**: ~250 Zeilen weniger, Leak-Klasse eliminiert.
- **Risiko / Verifikation**: Mittel – Destruktionsreihenfolge (Device/Queue zuletzt, `m_retiredTextures` vor Targets, Completion-Handler halten `m_uploadRing` per `shared_ptr`). Bestehende Re-Initialize-Semantik (Flags `m_*Tried` zurücksetzen) muss bleiben.

### REN-424 – Software-GI-Kernel: lineare Instanzschleife pro Strahl (kein TLAS)
- **ID**: REN-424
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:3411–3444 (`giSceneAnyHit`, `giSceneClosestHit`), 3771–3793 (`giSceneClosestHitTri`)
- **Problem**: Der SW-Fallback testet **jede** Instanz pro Strahl (inkl. 2× `float4x4 × float4` pro Instanz) und fährt dann erst die BLAS. Kosten pro Strahl O(Instanzen); bei Shadow-Rays pro Pixel (`spp` Strahlen) plus Probe-Rays plus Refl-Rays mit bis zu 4 Bounces multipliziert das massiv. Der Pfad greift auf Geräten ohne HW-RT bzw. mit `HE_GI_FORCE_SW`, ist also Sekundärpfad, aber dann ist er der einzige.
- **Code-Auszug**:
```cpp
for (int i = 0; i < instCount; ++i) {
    const float3 oL = (insts[i].invTransform * float4(o, 1.0)).xyz;
    ...
    if (giBlasHit(...)) return true;
```
- **Lösung**: Top-Level-BVH über die Instanz-Welt-AABBs (CPU-Build in `EncodeGISwAccelBuild`, wie `HE::buildGiBvh` es für Meshes schon kann) und im Kernel dieselbe Traversal-Routine (`giBlasHit`-Muster) auf Instanz-Boxen; erst bei Blatt der Instanz in die BLAS. Alternativ grobes Uniform-Grid der Instanz-Indizes.
- **Erwarteter Effekt**: von O(I) auf ~O(log I) pro Strahl; bei >100 Instanzen mehrfacher GPU-Gewinn (Schätzung).
- **Risiko / Verifikation**: GI-SW-Ergebnisse müssen gleich bleiben (gleicher Test wie HW-Kernel; `tests/test_gi_bvh.cpp` auf TLAS erweitern, CPU-Referenz `giBvhIntersect` existiert). Das GL-4.3-Port (`GiBvh`) profitiert vom selben Top-Level-BVH.

### REN-425 – Identische MSL-Hilfscode-Blöcke in sieben Libraries, nur per Kommentar synchron gehalten
- **ID**: REN-425
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `octEncode/octDecode` (565, 2576, 2591, 2897–2912, 3549, 3564), `giPcg3d/giHash2/giConeSample` (2433–2456, 2922–2943, 3460–3481), `GILand/giAutoLandAlbedo/giLandscapeAlbedo` (2827–2893 vs. 3288–3342), `giSampleFieldIrradiance`/`giReflField` (2602, 2949, 3571, 3797), Fullscreen-Triangle-Vertex (10×, z. B. 1004, 1090, 1300, 1538, 1595, 1723, 1840, 1963, 2244, 4050)
- **Problem**: Jeder `R"MSL(…)"`-String wird als eigene Library kompiliert; die Kopien tragen Kommentare wie „keep every copy on this one" / „byte-identical twin" / „SYNC". Die PCG-Hash-Funktion wurde schon einmal an einer Stelle gefixt (Thema 131), die Gefahr von Drift ist belegt. `kSkyMSL`/`kSkyFuncMSL` (4024–6367) ist zudem eine handgepflegte Spiegelung des Sky-Shaders in `SkyShaderSource.h` (siehe CLAUDE.md §1) mit vier laut Kommentar synchron zu haltenden Kopien (Z. 6167–6169).
- **Code-Auszug**:
```cpp
// Cone-jitter random numbers ... keep every copy on this one.
static uint3 giPcg3d(uint3 v) {...}            // Z. 2433, 2922 (als giPcg3dR), 3460
```
- **Lösung**: Gemeinsame Snippets als `static const char*` (z. B. `kMslOct`, `kMslPcg`, `kMslGiLand`, `kMslFsTriVertex`) definieren und per Konkatenation/`injectSkyMSL`-Muster (6370) in die Library-Strings einsetzen. Die einzelnen Libraries bleiben getrennte Kompilationseinheiten (Compile-Isolation bleibt), der Quelltext ist aber einmalig. Für `kSky*`: Mittel- bis langfristig Quelle aus `SkyShaderSource.h` per Übersetzung (shaderc/SPIRV-Cross) erzeugen, wie es `MaterialShaderLibrary` für Materialien bereits macht – nur Bewertung, größerer Umbau.
- **Erwarteter Effekt**: ~300 Zeilen Shader-Duplikat entfallen; geringere Drift-Gefahr zwischen Metal/GL/Vulkan.
- **Risiko / Verifikation**: Reiner Textumbau, Shader-Ergebnisse unverändert (Kompilation + Capture). Wegen `#include <metal_raytracing>` nur in den RT-Libraries die Snippets ohne Namensraum-Konflikte einfügen (`static`-Funktionen).

---

### REN-426 – Kleinere Mehrfach-Duplikate: `HE_SKY_TIME` (9×), `precompiledShaders`-Suche (7×), Half-Float-Decoder (4×), Preview-Target-Boilerplate
- **ID**: REN-426
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `getenv("HE_SKY_TIME")`: 10771, 11880, 12235, 13792, 14038, 15014, 15484, 16129 (je pro Frame/Aufruf!); `var.backend == …Metal`-Schleife: 7163, 9604, 9649, 13298, 14433, 16228, 17817; `auto h2f`: 10005, 10083, 10607, 11142; Preview-Ziel-(Re)Creation: 9945–9958, 10038–10052, 10168–10182, 10448–10463, 10719–10742, 11064–11078; Preview-Sphere-Rebuild: 9680–9692 vs. 10284–10298; Mesh-Interleave: 9226–9241 vs. 9297–9310
- **Problem**: `std::getenv` (lineare `environ`-Suche, bei manchen libc mit Lock) läuft in 8 Pro-Frame-Pfaden; das Ergebnis ist prozesskonstant. `MaterialShaderLibrary::precompiledFor(ma, RendererBackend::Metal)` existiert bereits (`MaterialShaderLibrary.cpp:954`) und liefert für Metal das identische Ergebnis wie die handgeschriebene Schleife. `h2f` ist ein viermal kopierter Half→Float-Decoder; Clang auf Apple-Hardware stellt `_Float16`/`__fp16` bereit. Die `built.insert(end, {…})`-Initializer-List-Variante im Interleave ist langsamer als direktes Beschreiben eines vorab dimensionierten Arrays.
- **Code-Auszug**:
```cpp
if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
```
- **Lösung**: `static float SkyClockSeconds()` (einmal gelesener Override als `static const`) für alle 8 Stellen; `precompiledFor` verwenden; `HalfToFloat`/Dump-Helfer (`WriteDumpPPM`) einmal definieren; `EnsureColorDepthTarget(void*& color, void*& depth, w, h, fmt)` für die Preview-Ziele; `InterleaveMeshVertices(asset, out)` für beide Mesh-Uploads.
- **Erwarteter Effekt**: ~150 Zeilen weniger; vernachlässigbarer, aber kostenloser Laufzeitgewinn (getenv im Frame entfällt).
- **Risiko / Verifikation**: Gering. Hinweis: Wenn jemand `HE_SKY_TIME` zur Laufzeit ändert (Debug), wäre der Wert nach „einmal lesen" eingefroren – dann nur bei Capture-Start neu lesen.

### REN-427 – Tote/veraltete Stellen: ungenutzte Variable/Parameter, falsche Zeilenreferenz im Kommentar, `const_cast`, redundante Kopie
- **ID**: REN-427
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 6993 (`EncodeDebugLines`: `device` unbenutzt), 13901–13902 (`EncodeSkinnedObjects(…, bool shadows, …)`: `shadows` nie gelesen), 18037 (Kommentar „MetalRenderer.mm:6282" – tatsächliche UI-Pipeline-Erstellung bei 7378–7399), 8438 (`const_cast<GpuMesh&>`), 6928–6931 (`SetDebugLines` kopiert den Vektor jedes Frame), 7022 (`glm::mat4 vp = viewProj;` nur für `setVertexBytes`)
- **Problem**: Kleine Reste, die beim Lesen irritieren: unbenutzte Variable/Parameter, ein Verweis auf eine Zeilennummer, die sich nicht mehr deckt (Kommentare, die Code statt Absicht beschreiben, sind in dieser Datei sonst vorbildlich), und ein `const_cast`, weil `ResolveMesh` einen `const GpuMesh*` liefert, der `blas` später nachträglich setzt.
- **Code-Auszug**:
```cpp
id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;   // Z. 6993, nie benutzt
GpuMesh& mesh = const_cast<GpuMesh&>(*resolved);            // Z. 8438
```
- **Lösung**: Entfernen/umbenennen; `ResolveMeshMutable()` oder `EnsureBlas(const HE::UUID&)` statt `const_cast`; Zeilenverweis durch Funktionsnamen ersetzen („`CreateScenePipeline`"); `SetDebugLines` per `std::move`-Overload/`swap`.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Keines (Compiler-Warnungen `-Wunused-variable`, `-Wunused-parameter` aktivieren, um weitere zu finden).

### REN-428 – Shader-Mikro-Optimierungen (Fragment-Hot-Path): `tan(acos())`, 9 manuelle PCF-Taps, Per-Pixel-Sun-Probe im Lens-Flare
- **ID**: REN-428
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 498 und 541 (`shadowFactor`, `localShadowFactorAt`), 501–506 / 543–548 (3×3-PCF), 1043–1044 (`lensFlareOverlay`), 2171–2181 (`ssaoBlurFragment`)
- **Problem**: (a) `tan(acos(ndl))` pro Fragment und pro Licht (zwei transzendente Funktionen) ist algebraisch `sqrt(1−ndl²)/ndl`. (b) 3×3-PCF = 9 Einzel-`sample`+Vergleiche pro Licht und Fragment; MSL bietet `depth2d_array::sample_compare` mit Hardware-PCF (bilinear 2×2) und weniger Taps. (c) Der 5-Tap-HDR-Luma-Probe am Sonnenort ist für alle Pixel identisch, wird aber pro Pixel ausgeführt, sobald `lf.w > 0` – lässt sich auf CPU/ein Mini-Pass auslagern. (d) SSAO-Blur macht 16 `sample`s statt 4 bilinear versetzten.
- **Code-Auszug**:
```msl
float bias = clamp(scene.shadowBias.x * tan(acos(ndl)), scene.shadowBias.y, 0.02) * float(c + 1);
for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) { ... shadowMap.sample(...) ... }
```
- **Lösung**: (a) `sqrt(max(1.0 - ndl*ndl, 0.0)) / max(ndl, 1e-4)`; (b) `sample_compare` mit Comparison-Sampler (`compare_func::less_equal`) und 2×2-Kernel; (c) Sun-Luma einmalig pro Frame; (d) vier bilineare Taps mit Halb-Texel-Versatz.
- **Erwarteter Effekt**: ALU-/Tex-Einsparungen im Shading-Pass (Schätzung 3–10 % Fragmentzeit in schattenlastigen Szenen, **messen mit Metal GPU Counters**).
- **Risiko / Verifikation**: (a) Numerisch äquivalent bis auf Rundung am ndl→0-Rand (Clamp deckt). (b) verändert die Schattenweichheit sichtbar → nur mit Bildvergleich (GL/D3D/Vulkan haben eigene, daher Backend-Parität prüfen). Nicht ohne Messung umsetzen.

### REN-429 – `EncodeSkyReflCube`: 6 Render-Encoder + Sky-Pass pro Frame, solange GI-Reflexionen aktiv sind
- **ID**: REN-429
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13771–13809
- **Problem**: Die Reflection-Sky-Cube wird jeden Frame auf allen 6 Faces (128² mit Wolken-Marsch, ggf. Aurora/Nebel bei Nacht) neu gerendert; der Kommentar rechtfertigt es mit driftenden Wolken. Wolken bewegen sich aber im Sekunden-/Minutenmaßstab; jede Face bekommt eigenen Encoder und Render-Pass-Setup.
- **Code-Auszug**:
```cpp
for (int f = 0; f < 6; ++f) { ... [cmdBuf renderCommandEncoderWithDescriptor:pass]; EncodeSky(..., /*cubeBake=*/true); [enc endEncoding]; }
```
- **Lösung**: Eine Face pro Frame (Round-Robin, 6-Frame-Zyklus) oder Neubake nur bei Änderung von Sonne/Zeitquantum; optional alle Faces in einem Pass mit `[[render_target_array_index]]` (Amplification) rendern.
- **Erwarteter Effekt**: 6× weniger Sky-GPU-Last dieses Features (**messen**).
- **Risiko / Verifikation**: Leichte Zeitversetztheit der Reflexionen; für Spiegel/Wasser mit Kamerabewegung ist `camPos` Teil der Eingabe – bei Kamerabewegung ggf. jedes Frame voll rendern.

---

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/RenderSorter.cpp:12–46`: `sort()` sortiert immer nach (Mesh-ID, Kamera-Abstand). Für Depth-/Shadow-Passes reicht ein Mesh-Gruppen-Sort; der Mehrfach-Aufruf je Layer (REN-407) trifft auch `VulkanRenderer.cpp:7007–7009` (m_shadowSorted) und vermutlich D3D11/D3D12/OpenGL – Lösung gehört in den gemeinsamen Sorter (`partition(sorted, visibleMask)`).
- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1000–1008` (`resolveFragment`): kopiert und hasht den kompletten GLSL-String bei jedem Aufruf. Ein `uint64_t`-Hash (oder eine Revision) im `MaterialAsset` würde diesen Aufruf in allen Backends (GL: `OpenGLRenderer`, Vulkan, D3D) auf O(1) bringen; betrifft REN-403.
- `src/HE_Rendering/include/material/MaterialShaderLibrary.h:40ff` (`Lighting`): >2 KB Strukt, die jede Backend-Draw-Schleife per Bytes/UBO sendet; ein „einmal pro Pass binden"-Konzept (REN-416) gilt analog für GL/D3D/Vulkan-Materialpfade.
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:4226` (`UpdateSkyEnvCube`): gleicher per-Frame-CPU-Bake wie REN-401; gemeinsame Lösung (niedrigere Auflösung/Toleranz in `HorizonRendering/SkyEnvBake.h`) wirkt für beide Backends.
- `OpenGLRenderer.cpp:5972` / `MetalRenderer.mm:8190`: `HE::giInstanceSurface(obj, cm)` wird pro Objekt/Frame aufgerufen; ein Cache im Extractor (`src/HE_Rendering/src/GiInstanceSurface.cpp`) würde beide Backends beschleunigen (REN-404).
- `src/HE_Rendering/include/Backends/Metal/MetalRenderer.h` (898–899, 1321–1331): Cache-Typen (`unordered_map<std::string,void*>` für Graph-/UI-Texturen, `unordered_map<uint32_t,mat4>` für TAA) sind Ursprung von REN-412/417; Header-Änderung nötig.
- `docs/audit/_parts/Rendering_7.md` (OpenGL/Software, anderer Slice): `HE_SKY_TIME`-getenv-Muster (`OpenGLRenderer.cpp:5076, 5659, 10711`) kommt dort ebenfalls pro Frame vor.

## Unsicher / Messung nötig

- **REN-401**: tatsächliche Dauer von `UpdateSkyEnvCube` auf M-Serie bei Auto-Advance (Kommentar nennt ~60 ms seriell; parallel unbekannt). Profil: `HE_PROFILE_SCOPE_N` um den Bake, Instruments Time Profiler.
- **REN-405/406**: Gewinn von TLAS-Refit vs. Neubau und Hitch-Dauer beim Laden – Metal System Trace (Acceleration-Structure-Build-Spuren) mit 1k/10k Instanzen.
- **REN-407**: Anteil von Cull+Sort an `Metal::EncodeShadowMap` bei 50k Entities, 3 Kaskaden + 4–8 Lokal-Lichtern.
- **REN-413**: GPU-Kosten des Cloud-Shadow-Passes (512²) im Stil „Realistic" vs. Classic; ob Zeitquantisierung sichtbar ruckelt.
- **REN-422**: Bandbreitengewinn von Ping-Pong statt Blit auf Apple-GPUs (TBDR; ggf. kleiner als gedacht); Bloom-Kette → Mip-Pyramide ändert das Bild.
- **REN-424/428**: Alle Shader-Änderungen nur mit Metal GPU Counters/Capture-A/B (kein Messwert im Repo vorhanden).
- **Forward-SSAO-Pre-Pass** (EncodeSSAO 12353ff, nicht als Fund aufgeführt): re-rasterisiert in der Forward-Pfad die gesamte Geometrie (zusätzliche View-Space-Position-Pre-Pass-Geometrie); ob ein vorhandener Depth-Pre-Pass wiederverwendbar ist, hängt vom Gesamtdesign ab – Architekturentscheidung, nicht aus dieser Datei allein ableitbar.
- **Custom-Material-Draws sind nie instanziert** (14636, Kommentar „optics before draw-call savings"): potenziell viele Draws bei Foliage/Partikeln mit Graph-Material; Gewinn nur messbar mit Szenen-Daten.
- **Frame-Retire-Konstante 3** (`RetireTexture`/`RetireGIObject` 11373, 11404) statt Command-Buffer-Completion: bei >3 Frames GPU-Latenz theoretisch Use-after-free möglich; nicht beobachtet, Validation-Layer prüfen.
- **Shadow-Rendering pro Layer separater Encoder** (7977): alle Kaskaden in einem Pass über `render_target_array_index` rendern wäre möglich; Gewinn unklar.

## Gelesen

Ja, MetalRenderer.mm wurde vollständig gelesen (Z. 1–18158: alle eingebetteten MSL-Strings, alle Host-Funktionen), zusätzlich gezielte Stichproben in `MetalRenderer.h`, `MaterialShaderLibrary.h/.cpp`, `RenderSorter.cpp`, `SsaoKernel.h`, `OpenGLRenderer.cpp`/`VulkanRenderer.cpp` für die Cross-Backend-Hinweise; keine Quelldatei wurde geändert.
