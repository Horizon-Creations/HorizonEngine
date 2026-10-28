# Rendering Slice 8 – Backends/Vulkan/VulkanRenderer.cpp (14.698 Zeilen)

Datei: `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp` (alle Zeilenangaben beziehen sich darauf, sofern nicht anders genannt).

## Zusammenfassung

Der Vulkan-Backend ist funktional reich (Deferred, TAA, SSR, SW/HW-GI, Decals, Preview), aber die Datei ist eine einzige 14,7k-Zeilen-Klasse mit sehr viel kopiertem Vulkan-Boilerplate (33× `vkCreateGraphicsPipelines`, 16× `VkPipelineViewportStateCreateInfo`, 21× One-Shot-Command-Buffer mit `vkQueueWaitIdle`, 58× `findMemoryType`, 53× `vkAllocateMemory`, 4× identisches `makeModule`-Lambda). Die schwersten Performance-Probleme liegen im Draw-Pfad der Graph-Materialien (pro Draw/Instanz ein frisch allokiertes Descriptor-Set mit ~25 Writes plus Shader-Quelltext-Kopie und -Hash), in der blockierenden Asset-Upload-Strategie (je Mesh/Textur eigener Submit + `vkQueueWaitIdle`, je Buffer eigene `vkAllocateMemory`, Vertex-/Index-Buffer in host-visible Speicher), in der fest eingeschalteten Validation-Layer und in der mehrfachen Cull+Sort-Arbeit pro Frame. Gefunden: 5 Hoch (REN-801 bis 805), 12 Mittel (REN-806 bis 817), 5 Niedrig (REN-818 bis 822), zusammen 22 Funde. Es gibt keine Pipeline-Cache-Nutzung (`VkPipelineCache`: 0 Treffer) und keinen Sub-Allocator. Vorschläge erhalten das sichtbare Verhalten; wo nicht, ist es vermerkt. Keine Messdaten verfügbar: Effekte sind Schätzungen.

---

## Funde (Hoch zuerst)

### REN-801 – Graph-Material-Draw: Descriptor-Set-Allokation, ~25 Writes und Shader-String-Kopie/-Hash pro Draw (und pro Instanz)
- **ID**: REN-801
- **Kategorie**: D (zusätzlich C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: VulkanRenderer.cpp:8251-8259 (Resolve pro Draw), 8317-8352 (Texturen), 8364-8566 (`drawMatInstance`), 8561-8584 (Re-Bind)
- **Problem**: Für jeden DrawCall mit Graph-Material passiert pro Draw: (a) `resolveShaders(...)` kopiert den kompletten Fragment-GLSL-Text in einen neuen `std::string` und berechnet `std::hash<std::string>` über ihn (`MaterialShaderLibrary.cpp:1000-1008`), bei WPO zusätzlich Hash des Vertex-Bodys (`:947-952`); (b) `precompiledFor`, 3-4× `getMaterial`, `matGlslTextureArrayMask`, vier `std::string gPaths[]`, je Textur-Slot `resolveGraphTexture` (baut `graphTexKey`-String + `"#arr"` und hasht, 3616-3622); (c) in `drawMatInstance` pro **Instanz**: `vkAllocateDescriptorSets` + bis zu 25 `VkWriteDescriptorSet` + `vkUpdateDescriptorSets` + eigener Pipeline-/Set-Bind, obwohl sich pro Draw nur zwei UBO-Offsets (Objekt/Params) ändern; fast alle Image-Bindings (GI, CSM, SSR, Sky, AO, Cluster-SSBOs) sind frame-konstant. Das Ring-Limit `k_matMaxDraws = 1024` (Header :457) wird dadurch zu einer harten Szenengrenze (Überschuss rendert still als Built-in-PBR, 8299/8365).
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId, matHash, matFrag, matVertBody)) { ...
auto drawMatInstance = [&](const glm::mat4& model) {
    ...
    vkAllocateDescriptorSets(m_device, &dsai, &set) ...
    vkUpdateDescriptorSets(m_device, nWrites, w, 0, nullptr);
```
- **Lösung**:
  1. Pro Material (Key = `UUID`) einmal `{hash, const string* frag, vertBody-Hash, precompiled*, arrMask, Texturslots, Params}` in einer Cache-Struktur halten (Invalidierung über das vorhandene `InvalidateMaterial`); `resolveShaders` nur auf Miss. Der Hash gehört in `MaterialAsset` (einmal berechnet), nicht per Draw.
  2. Frame-konstante Bindings (0 Lighting, 10-13, 15-18, 24-26, 31-33) in ein **frame-weites Set** legen oder per Descriptor-Set-Layout-Split (Set 0 = Frame, Set 1 = Material, Set 2 = Draw mit Dynamic-UBO-Offsets für `U` und `HeParams`). Pro Draw dann nur `vkCmdBindDescriptorSets` mit Dynamic Offsets; ein Set je (Material, Albedo/Override-Textur, Weightmap) cachen und beim Frame-Wechsel nur bei geänderten Views neu schreiben.
  3. Instanzen: Per-Instanz-Daten in die Instanz-Vertex-Buffer-Schiene legen (wie A3 für Built-in) statt Set pro Instanz.
- **Erwarteter Effekt**: (Schätzung) CPU-Zeit im Material-Draw-Pfad um ein Mehrfaches niedriger (Descriptor-Writes ~25→0-2 pro Draw, Heap-Allokationen ~10→0); Entfall der 1024-Draw-Grenze. Messen mit CPU-Profiler an einer Szene mit ≥500 Graph-Material-Objekten.
- **Risiko / Verifikation**: Hoch (Descriptor-Layout ist durch `HE::vkmat::kBindings` fixiert und gegen SPIR-V getestet, siehe `tests/` Material-Layout-Tests). Layout-Änderung heißt: Shader-Reflexionstest anpassen. Bildvergleich mit `tests/test_*` GL/Vulkan-Parität und Validation-Layer-Lauf (Layouts GENERAL vs. READ_ONLY beachten, 8444-8451).

### REN-802 – Jeder Asset-Upload blockiert die Queue (`vkQueueWaitIdle`) auf dem Render-Thread
- **ID**: REN-802
- **Kategorie**: E (D)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 7217-7332 (`uploadRGBA8Image`, Wait 7314), 7338-7509 (`uploadTextureImage`, Wait 7491), 7760-7764 (`resolveMesh`, Kommentar bestätigt Stall), 9903 (`SetMoonTexture`), 10052 (`CreateImGuiTexture`), 14494 (`uiFontAtlasSet`), 12042/12148/12880 (SSAO/Prime), 10552 (BLAS)
- **Problem**: 21× `vkQueueWaitIdle`, 22× `vkDeviceWaitIdle`. `resolveMesh`/`resolveAndUploadAlbedo`/`resolveGraphTexture`/`resolveDecalTexture`/`resolveUIImageSet` werden **mitten im Draw-Loop** aufgerufen; jeder erste Zugriff erzeugt Staging-Buffer + Memory, Command Buffer, Submit und Wait-Idle: die GPU wird komplett geleert, der Frame-Overlap geht verloren. Beim Laden einer Szene mit N Meshes/Texturen sind das N harte Pipeline-Stalls innerhalb eines Frames. Zusätzlich ein zusätzlicher `memcpy` der Textur in den Staging-Buffer (7410) und je Mip ein eigener `vkCmdCopyBufferToImage` (7461-7472).
- **Code-Auszug**:
```cpp
vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
vkQueueWaitIdle(m_graphicsQueue);
vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);
```
- **Lösung**:
  1. Kleine `UploadContext`-Klasse: ein persistenter Staging-Ring (z. B. 32-64 MB host-visible, per Frame-Slot), ein Transfer-Command-Buffer pro Frame-Slot. `uploadX` schreibt in den Ring, zeichnet Copy + Barrier in den Upload-CB auf und gibt View/Image sofort zurück; der Upload-CB wird am Frame-Anfang vor dem Haupt-CB submittet (gleiche Queue ⇒ Reihenfolge reicht, Semaphore nicht nötig). Freigabe von Staging über Frame-Fence (`m_frameSerial`/`GetCompletedFrameSerial` existiert bereits).
  2. Bis ein Asset „fertig“ ist: wie heute Fallback (weiße Textur / Würfel); dafür müssen Caches einen `pending`-Zustand kennen. Alternativ erst den Einmal-Helper (ein Wait pro **Batch** am Frame-Ende statt pro Ressource) einführen.
  3. Die Dubletten zu einer Hilfsfunktion `submitOneShot(fn)` zusammenführen (siehe REN-815).
- **Erwarteter Effekt**: (Schätzung) Beim Streaming vieler Assets entfallen N Voll-Stalls; typische Hitches im ms-Bereich pro Asset (Treiber/GPU-abhängig) verschwinden. Messen: Zeit von `resolveMesh` beim ersten Zugriff.
- **Risiko / Verifikation**: Mittel-hoch (Layout/Barrier-Reihenfolge, Lebenszeit der Staging-Daten). Tests: bestehende Render-Smoke-Läufe mit lavapipe (HE_HIDDEN_WINDOW), Validation-Layer sauber halten.

### REN-803 – Mesh-/Skeletal-/Ribbon-Buffer liegen in host-visible Speicher; je Buffer eigener `vkAllocateMemory`, kein Sub-Allocator
- **ID**: REN-803
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 7107-7136 (`createMeshBuffers`), 13986-14018 (`makeHostBuf` Skeletal), 7153-7182 (Ribbon), Allokationsstatistik: 53× `vkAllocateMemory`, 29× `HOST_VISIBLE_BIT`
- **Problem**: Statische Vertex-/Index-Buffer werden mit `HOST_VISIBLE|HOST_COHERENT` angelegt, einmal befüllt und nie wieder von der CPU geschrieben. Auf diskreten GPUs liest die GPU sie dann über PCIe (bzw. aus BAR-Speicher). Zusätzlich 2 Allokationen pro Mesh (vbuf, ibuf; Skeletal 4, plus Textur-Image+Staging). Der Treiber-Grenzwert `maxMemoryAllocationCount` (typisch 4096) begrenzt damit die Szene hart (≈2000 Meshes ohne Textur); `k_maxMeshTextures = 1024` (Header :550) ist die Descriptor-Variante derselben Grenze.
- **Code-Auszug**:
```cpp
mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
...
std::memcpy(p, data, static_cast<size_t>(size));
```
- **Lösung**: Einen Mesh-Pool einführen: große DEVICE_LOCAL-Blöcke (z. B. 64 MB Vertex+Index in einem Buffer, `vkCmdBindVertexBuffers` mit Offset, `firstIndex`/`vertexOffset` im Draw), Befüllung über den Staging-Ring aus REN-802. Gleiche Idee für Bild-Speicher (ein Block, mehrere `vkBindImageMemory` mit Offset). Vorarbeit: `GpuMesh` bekommt `vbufOffset/ibufOffset`. Bei Wegfall von vmem/imem im Struct fallen auch die manuellen Free-Zweige in `processPendingInvalidations` (7605-7612) und `Shutdown` (351-361) weg.
- **Erwarteter Effekt**: (Schätzung) Vertex-Fetch-Bandbreite aus VRAM statt PCIe (auf dGPU deutlich), weniger Allokationen, weniger Handle-Verwaltung. Auf iGPU/UMA keine Änderung.
- **Risiko / Verifikation**: Mittel. `firstIndex`/Index-Range-Logik (`DrawIndexRange`, 67-73, hängt an Buffer-Länge) muss mit Offsets korrekt bleiben (Tests rund um Sections/`DrawIndexRange` im Tests-Verzeichnis prüfen). Fragmentierung/Invalidierung (Mesh-Sculpt) braucht Freelist.

### REN-804 – Validation-Layer fest eingeschaltet (`gpuDebug = true`) auch in Release
- **ID**: REN-804
- **Kategorie**: F / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 1283-1288, 1309-1313, 1341
- **Problem**: Die Instanz aktiviert `VK_LAYER_KHRONOS_validation`, sobald installiert, unabhängig von Build-Typ oder Umgebungsvariable. Kommentar nennt es „TEMPORARILY“. Validation-Layer verteuert jeden `vk*`-Aufruf um ein Vielfaches; zusammen mit REN-801 (hunderte Descriptor-Updates pro Frame) wird die CPU-Zeit verfälscht und der Backend unnötig langsam. Außerdem Log-Ausgabe der Meldung „(HE_GPU_DEBUG)“, obwohl die Variable nie gelesen wird.
- **Code-Auszug**:
```cpp
const bool gpuDebug = true;
(void)0;
```
- **Lösung**: `const bool gpuDebug = [] { const char* e = std::getenv("HE_GPU_DEBUG"); return (e && *e && *e != '0')
 #ifndef NDEBUG
 || true
 #endif
 ; }();` Die Brace-Struktur 1289-1314 (ein überflüssiger innerer Block) gleichzeitig begradigen.
- **Erwarteter Effekt**: (Schätzung) Auf Rechnern mit installiertem Vulkan SDK deutlich geringere CPU-Last pro Frame; Startzeit kürzer. Auf Rechnern ohne SDK keine Änderung.
- **Risiko / Verifikation**: Niedrig. Die CI (lavapipe) hängt laut Kommentar 5773-5783 teils an Validation als Fehler-Erkennung: dort `HE_GPU_DEBUG=1` setzen.

### REN-805 – Cull + Sort bis zu 5× pro Frame über denselben Objektsatz
- **ID**: REN-805
- **Kategorie**: D
- **Schweregrad**: Hoch (Annahme; messen)
- **Typ**: Performance
- **Ort**: 3891-3892 (`EncodeDecalDepth`), 7840-7841 (`DrawScene`), 11510-11511 (`runGi`), 12656-12657 (`runSSAO`); zusätzlich `resolveWorldMaterialScalars` in 3890, 7838, 11495; `refineObjectBounds` je Aufruf mit Frühausstieg (6888)
- **Problem**: Der Extraktor ist per `FrameScope` bereits auf einen Walk pro Frame optimiert (413-418), aber `m_culler.cull(m_renderWorld, m_visible)` und `m_sorter.sort(...)` laufen für dieselbe Kamera und dasselbe Aspect in bis zu vier Passes erneut. Ergebnis wird jeweils in dieselben Member (`m_visible`, `m_sortedIndices`) geschrieben. Bei 50k Objekten ist jede Wiederholung O(n) Frustum-Test + O(n log n)-Sortierung.
- **Code-Auszug**:
```cpp
m_culler.cull(m_renderWorld, m_visible);
m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
```
- **Lösung**: Wie bei `refineObjectBounds` einen Schlüssel `(fullExtractCount, objects.data(), count, view*proj-Hash, taaJitter)` führen und `cullAndSortOnce()` schreiben; die vier Aufrufer rufen es auf und überspringen bei gleichem Schlüssel. Achtung: `EncodeDecalDepth` extrahiert mit eigenem Aspect (3880) und löst dadurch einen neuen Walk aus; im Viewport-Pfad ist der Aspect gleich, dort greift der Cache. `resolveWorldMaterialScalars` analog (idempotent pro Walk) hinter denselben Schlüssel.
- **Erwarteter Effekt**: (Schätzung) 2-4 Cull+Sort-Durchläufe weniger pro Frame im Viewport-HDR-Pfad bei aktivem SSAO/GI/Decals. Messen: Zeitscope um die vier Aufrufstellen.
- **Risiko / Verifikation**: Mittel. `tests/test_culling.cpp` pinnt bereits die FrameScope-Regeln; für diesen Cache einen Test ergänzen, der bei geändertem Jitter/Aspect neu berechnet. Beachten: `m_sortedIndices` wird in `encodeTaaVelocity` über `m_taaSceneSorted` indirekt vorausgesetzt (5638).

---

## Mittel

### REN-806 – TAA-Velocity: `unordered_map` pro Frame komplett neu aufgebaut
- **ID**: REN-806
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 5648-5674; Header `VulkanRenderer.h:874` (`std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms, m_taaCurTransforms;`)
- **Problem**: Pro TAA-Frame wird `m_taaCurTransforms.clear()` aufgerufen (gibt alle Knoten frei) und dann für jedes sichtbare Objekt `m_taaCurTransforms[entityId] = transform` eingefügt (je ein Heap-Knoten, 72 B), dazu ein `find` in der Vorgänger-Map. Danach `swap`. Bei n sichtbaren Objekten sind das n Allokationen + n Frees pro Frame. Die Velocity-Passage zeichnet außerdem jedes Objekt einzeln (kein Batching), siehe auch REN-812.
- **Code-Auszug**:
```cpp
m_taaCurTransforms.clear();
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache Struktur: `std::vector<std::pair<uint32_t, glm::mat4>>` sortiert nach `entityId` (oder offene Adressierung) für prev/cur; `m_sortedIndices` ist stabil sortiert nach Mesh, daher zuerst nach `entityId` sortieren oder Hash-Tabelle mit Reserve (`reserve(max(prev.size(), n))`, `clear()` ohne Rehash als flat map). Minimal: `m_taaCurTransforms.reserve(...)` und `swap` reicht nicht (Knoten-Allokation bleibt) – flache Tabelle ist die eigentliche Lösung.
- **Erwarteter Effekt**: (Schätzung) bei 10k+ sichtbaren Objekten mehrere zehn µs bis ms pro Frame (Allocator-Druck); messen.
- **Risiko / Verifikation**: Niedrig. Verhalten identisch („neues Objekt = keine Bewegung“). Testen mit TAA-Dump (`HE_*`-Capture) auf Gleichheit der Velocity-Textur.

### REN-807 – GI: pro Frame `glm::inverse` + Upload für alle Objekte, BLAS-Builds blockierend
- **ID**: REN-807
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 10226-10352 (`updateGiAccel`), 10263-10314, 10442-10572 (`buildGiHwBlas`, `vkQueueWaitIdle` 10552), 10154-10179 (`BuildGIBlas` CPU-BVH on demand)
- **Problem**: Jeder Frame mit aktiver GI iteriert über alle `castsShadow`-Objekte, berechnet `glm::inverse(obj.transform)` (4×4 vollständig), baut `GIInstanceGpu`- und `VkAccelerationStructureInstanceKHR`-Vektoren neu (Heap pro Frame trotz `reserve`) und kopiert den gesamten Array in den host-visible Ring (10334). Es gibt keine Dirty-Erkennung (statische Welt = identische Daten). Erste Sichtung eines Meshes baut BVH auf der CPU im Render-Thread (`BuildGIBlas`) bzw. das HW-BLAS mit eigener Submit+Wait-Idle-Runde. Außerdem: bei Mesh-Invalidierung wird der **gesamte** BLAS-Cache verworfen (7618-7624).
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
if (!uploadGiBuffer(m_giInstanceBuf[m_currentFrame], instances.data(), instances.size() * sizeof(GIInstanceGpu)))
```
- **Lösung**: (1) Inverse nur bei geänderter Transform (Hash/Version aus Extraktor, falls vorhanden; sonst Vergleich mit Vorframe-Matrix pro Objekt) – oder vom Extraktor mitliefern lassen. (2) Persistente `std::vector` als Member (`clear()` statt neu allozieren). (3) Upload überspringen, wenn Instanz-Array unverändert (Hash/Memcmp gegen letzten Frame je Ring-Slot). (4) BLAS-Aufbau über den Upload-Kontext aus REN-802 asynchron bzw. auf Job-Thread (BVH ist CPU-rein).
- **Erwarteter Effekt**: (Schätzung) GI-CPU-Kosten pro Frame bei statischen Szenen ~O(1) statt O(n); Hitch beim ersten Mesh entfällt. Messen.
- **Risiko / Verifikation**: Mittel. Instanzreihenfolge muss mit `InstanceId` (`vi.instanceCustomIndex`, 10303) übereinstimmen – deshalb Reihenfolge nicht ändern. Kommentar 10319-10322 weist selbst darauf hin, dass `m_giNodeBuf/m_giTriBuf` bei Wachstum zerstört werden, während ein Vorframe sie noch lesen kann (Hazard, unsicher; nicht verifiziert) – bei Umbau mit abdecken.

### REN-808 – Keine `VkPipelineCache`; Material-Pipelines und Sky-Shader werden synchron beim ersten Draw / Start gebaut
- **ID**: REN-808
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3076-3302 (`GetOrBuildMaterialPipeline`), 7659-7694 (`drainMaterialWarmup`), 9345-9368 (Sky-Fragment-Shader-Kompilierung per shaderc bei jedem Start), 9139-9145 (3D-Rauschvolumen 256³ = 64 MB CPU-Bake + Upload bei jedem Start im Release)
- **Problem**: Alle 33 `vkCreateGraphicsPipelines`/Compute-Aufrufe übergeben `VK_NULL_HANDLE` als Cache. Ein neues Graph-Material löst Quelltext→SPIR-V (glslang) + Pipeline-Kompilierung im Draw-Loop aus (Warm-up existiert, deckt aber nur angemeldete IDs ab, 7659). Der Sky-Fragment-Shader (großer GL-Text) wird bei jedem Start erneut zu SPIR-V kompiliert; das Rauschvolumen wird bei jedem Start neu berechnet (`HE::BuildSkyNoise3D(256)`, deterministisch).
- **Code-Auszug**:
```cpp
vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe)
```
- **Lösung**: Ein `VkPipelineCache` Member (Initialdaten aus `<userdata>/vk_pipeline_cache.bin`, beim Shutdown zurückschreiben) an alle Create-Aufrufe geben; Sky-SPIR-V und Rauschvolumen nach Hash der Eingabe auf Platte cachen (oder Rauschen als gepackte Asset-Datei liefern). Pipeline-Build optional in einem Job (`JobSystem`) und der Draw wird bis dahin übersprungen/Built-in gezeichnet.
- **Erwarteter Effekt**: (Schätzung) Start- und Material-Hitch-Zeiten sinken von zig-hundert ms auf wenige ms bei warmem Cache; Messung beim Start (`HE_LOG_INFO` Zeitstempel um 9345-9368 und 9145).
- **Risiko / Verifikation**: Niedrig (Cache ist treiberseitig validiert). Cache-Datei bei Treiber-/UUID-Wechsel verwerfen (Header-Check).

### REN-809 – Decals: pro Decal ein Descriptor-Set, `glm::inverse`, UUID-Cache-Lookup
- **ID**: REN-809
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3986-4058 (`EncodeDecals`), 3593-3607 (`resolveDecalTexture`)
- **Problem**: Pro Decal: `vkAllocateDescriptorSets` + 3 Writes + `vkUpdateDescriptorSets`, `glm::inverse(dcl.transform)`, Map-Lookup in `m_decalTexCache`. Das UBO-Slot-Schema (`k_decalSlotStride`) ist bereits da und würde mit Dynamic-Offset-UBO ein einziges Set pro Textur erlauben. Deckel `k_maxDecals` fix.
- **Code-Auszug**:
```cpp
if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) break;
...
vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
```
- **Lösung**: Binding 23 auf `UNIFORM_BUFFER_DYNAMIC` ändern, je Textur-View ein gecachtes Set (Key = View-Handle), pro Decal nur UBO-Write + Bind mit Offset. `invModel` aus dem Extraktor liefern lassen.
- **Erwarteter Effekt**: (Schätzung) bei vielen Decals (>50) Reduktion der Descriptor-Arbeit auf ~0; klein bei wenigen.
- **Risiko / Verifikation**: Niedrig-mittel. Decal-Shader-Layout (b19/b22/b23, generiertes SPIR-V) muss Dynamic-UBO tolerieren (Layout-Typ ändert sich, SPIR-V nicht). Decal-Parity-Tests prüfen.

### REN-810 – `updateSkyEnvCube`: 1,5-MB-Vektor pro Neuberechnung + serielle Half-Konvertierung
- **ID**: REN-810
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3019-3070
- **Problem**: Bei jeder Sonnenverschiebung > 2e-3 (Tag/Nacht-Zyklus ⇒ regelmäßig) wird `std::vector<float> px(128*128*6*4)` (~1,5 MB) allokiert, per `parallel_for` gefüllt und anschließend **seriell** `packHalf1x16` über alle 393k Floats in den Staging-Puffer geschrieben (3036-3037). Allokation + serielle Schleife laufen auf dem Render-Thread.
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
...
for (size_t i = 0; i < px.size(); ++i)
    dst[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: Scratch als Member (einmal allokieren) und die Half-Konvertierung in die `parallel_for`-Zeilenfunktion verlagern (jede Zeile schreibt ihren Zeilenbereich in `dst`). Alternativ `HE::BuildSkyEnvFaceRow` direkt auf Half-Ausgabe erweitern (Cross-Modul, REN-Cross-1).
- **Erwarteter Effekt**: (Schätzung) < 1 ms gespart je Neuberechnung; sichtbar nur bei laufendem Tag/Nacht. Messen.
- **Risiko / Verifikation**: Niedrig; Bytegleichheit des Cube mit Test (`SkyEnvBake.h`-Tests) vergleichbar.

### REN-811 – `findMemoryType` fragt die Speichertypen bei jedem Aufruf neu ab
- **ID**: REN-811
- **Kategorie**: D
- **Schweregrad**: Mittel (häufig, aber billig)
- **Typ**: Performance + Vereinfachung
- **Ort**: 1644-1652 (58 Aufrufstellen)
- **Problem**: `vkGetPhysicalDeviceMemoryProperties` wird bei jeder Allokation aufgerufen, auch im Laufzeitpfad (Mesh-/Textur-Upload, GI-Buffer-Wachstum). Funktion wirft eine Exception bei Fehlschlag, was an einigen Stellen mit `try/catch (...)` umschifft wird (10406-10407).
- **Code-Auszug**:
```cpp
VkPhysicalDeviceMemoryProperties mp{};
vkGetPhysicalDeviceMemoryProperties(m_physDevice, &mp);
```
- **Lösung**: Einmal in `pickPhysicalDevice` in ein Member `m_memProps` kopieren; zusätzlich eine zentrale `allocBuffer(size, usage, memFlags, Buffer&)` / `allocImage(...)` Hilfe (REN-815), damit die 58 Stellen verschwinden.
- **Erwarteter Effekt**: gering (µs pro Aufruf), aber Grundlage für REN-815.
- **Risiko / Verifikation**: Sehr niedrig.

### REN-812 – Redundante State-Binds im Built-in-/Material-Draw-Loop
- **ID**: REN-812
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 8575-8584, 8623-8637, 8656-8681, 7855 (36× `vkCmdBindDescriptorSets` in der Datei)
- **Problem**: Nach **jedem** Graph-Material-Draw: Pipeline-Rebind der Szenenpipeline + Neubinden von Set 0 (8576-8584). Im Built-in-Pfad pro Draw: Set 0 (mit Dynamic Offset), Set 2 (Albedo), Vertex-/Index-Buffer-Bind, auch wenn Mesh/Material/Albedo gleich dem Vorgänger sind (die Sortierung `RenderSorter` gruppiert nach Mesh). Kein State-Cache (zuletzt gebundene Pipeline/Set/Mesh).
- **Code-Auszug**:
```cpp
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
vkCmdBindDescriptorSets(cmd, ..., m_scenePipelineLayout, 0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
```
- **Lösung**: Kleiner `struct BindState { VkPipeline pipe; VkDescriptorSet set0, set2; uint32_t dynOff; VkBuffer vb, ib; }` im Frame, mit Hilfsfunktionen `bindPipeline/bindSet/bindMesh`, die nur bei Änderung `vkCmd*` aufrufen (analog `UIPass::bindSet`, 14274-14280). Der Re-Bind nach Material-Draw entfällt, weil die nächste Bind-Funktion den Stand kennt. Hinweis: Set 0 des Material-Layouts ist zu Set 0 des Szenen-Layouts inkompatibel; der State-Cache muss das als Invalidierung (`set0 = null`) werten.
- **Erwarteter Effekt**: (Schätzung) 20-50 % weniger Bind-Aufrufe in Szenen mit vielen Instanzen derselben Mesh/Material; Wirkung vor allem CPU/Treiber.
- **Risiko / Verifikation**: Niedrig-mittel. Layout-Kompatibilität (Set 0) genau beachten; Validation-Layer-Lauf.

### REN-813 – Viewport-Resize/Invalidierung: mehrfach `vkDeviceWaitIdle` + `vkQueueWaitIdle` pro Änderung
- **ID**: REN-813
- **Kategorie**: E
- **Schweregrad**: Mittel (Editor)
- **Typ**: Performance
- **Ort**: 516-518 + 5237 (`destroyPostFXResources` wartet erneut) + 5548 (TAA-Prime) + 12880/13414 (`primeShaderReadLayout` je Bild: 6× pro `createSSRTargets`) + 7583 (`processPendingInvalidations`) + 6123/6169 (`RenderSceneImage`) + 6551 (`RenderWorldPreview` pro Aufruf), 5933 (`CaptureViewport`)
- **Problem**: Eine Viewport-Größenänderung (Ziehen eines Docks erzeugt pro Frame eine neue Größe) löst `vkDeviceWaitIdle` in `Render()`, dann nochmals in `destroyPostFXResources`, dann Neuanlage von HDR/Bloom/LDR/TAA/SSAO/Decal-Depth; die Prime-Helfer submitten jeweils einzeln mit `vkQueueWaitIdle`. `processPendingInvalidations` idlet bei jeder Material-/Mesh-/Textur-Invalidierung (während Terrain-Sculpt jeden Frame, wie der Kommentar 7580-7582 selbst notiert).
- **Code-Auszug**:
```cpp
void VulkanRenderer::destroyPostFXResources()
{
    vkDeviceWaitIdle(m_device);
```
- **Lösung**: (1) Editor-seitig Resize entprellen (nur anwenden, wenn die Größe ≥ N ms stabil ist, oder auf 8-px-Raster runden – Cross-Modul). (2) Einen „retire list“ (wie `m_retiredViewports`, 443-454) für Bilder/Framebuffer/Views erweitern: statt Idle werden Ressourcen N Frames später freigegeben (D3D12-Weg laut Kommentar 7582). (3) `primeShaderReadLayout` zu einem Batch (alle Bilder in einem CB) bündeln. (4) Doppelten Idle entfernen: `destroyPostFXResources` wird nur aus bereits wartenden Pfaden aufgerufen (`createViewportResources` hinter Idle) – der Kommentar 13398-13401 verbietet das Entfernen **upstream**, deshalb besser den Idle dort belassen und in `destroyPostFXResources` weglassen, sofern alle Aufrufer vorher warten (`Shutdown` tut es, 222).
- **Erwarteter Effekt**: (Schätzung) deutlich flüssigeres Resize im Editor; eliminiert wiederholte GPU-Leerläufe.
- **Risiko / Verifikation**: Mittel – wegen der dokumentierten Sicherheitsannahmen (Kommentar 13398-13401). Nur mit Validation-Layer + Resize-Stresstest ändern.

### REN-814 – Pipeline-State-Boilerplate und HDR/LDR-Doppelaufbau dupliziert
- **ID**: REN-814
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 2413-2545 (`createScenePipeline`) vs. 5016-5094 (HDR-Kopie derselben Pipelines), 3209-3301, 3733-3804, 4280-4337, 4863-4896, 4948-5003, 5321-5441, 6316-6358, 9378-9441, 9633-9719, 10898-10985, 12398-12514, 13228-13287, 13846-13935, 14170-14241
- **Problem**: 16× ein vollständiger Satz `ia/vp/rs/ms/ds/cba/cb/dyn` mit praktisch identischen Werten (Dreiecksliste, Cull None, CCW, Dynamic Viewport/Scissor). `createPostFXPipelines` baut die Scene-Pipelines (HDR, Instanced HDR, Transparent HDR) als zweite handgeschriebene Kopie (5016-5094) mit eigenen Variablennamen (`vi2`, `ia2` …) statt `createScenePipeline` mit einem `VkRenderPass`-Parameter aufzurufen. Änderungen am Zustand (z. B. Culling) müssen so an ~16 Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
VkPipelineViewportStateCreateInfo vp2{ ... }; vp2.viewportCount=1; vp2.scissorCount=1;
VkPipelineRasterizationStateCreateInfo rs2{ ... }; rs2.polygonMode=VK_POLYGON_MODE_FILL; rs2.cullMode=VK_CULL_MODE_NONE; ...
```
- **Lösung**: Kleiner `GraphicsPipelineBuilder` (Defaults: Dreiecksliste, FILL, CULL_NONE, CCW, 1 Sample, Dynamic Viewport/Scissor; Setter für Vertex-Input, Depth, Blend-Preset (None/Alpha/Premult), Layout, RenderPass, Shader-Stages; `build(cache)`), und `buildScenePipelineSet(VkRenderPass rp, Pipelines& out)` für LDR und HDR. Optional eine `ShaderModuleGuard`-RAII-Klasse (`makeModule` ist 4× kopiert: 3118, 3715, 4461, 13207).
- **Erwarteter Effekt**: -1500 bis -2500 Zeilen; Konsistenz; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Render-Pass-Kompatibilität bleibt, Pipeline-Parameter 1:1 übernehmen. Smoke-Test + Bild-Hash.

---

## Mittel (Vereinfachung / Struktur)

### REN-815 – Buffer-/Image-/One-Shot-Boilerplate in ~50 Kopien
- **ID**: REN-815
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: z. B. 2141-2168, 2220-2234, 2852-2865, 3699-3711, 4429-4442, 6226-6256, 7110-7129, 7153-7182, 10181-10224, 10391-10417, 12158-12170, 13101-13114, 13986-14007; One-Shot-Submit: 2739-2765, 4746-4764, 5525-5548, 7270-7315, 7439-7492, 9194-9246, 9850-9904, 9999-10053, 11266-11293, 11420-11450, 12018-12043, 12124-12149, 12864-12881, 13176-13203, 14441-14495
- **Problem**: Das Muster „`vkCreateBuffer` → `GetBufferMemoryRequirements` → `findMemoryType` → `vkAllocateMemory` → `vkBindBufferMemory` → `vkMapMemory`“ steht ~25× fast wörtlich in der Datei (Lambda-Namen `makeBuf`, `makeSSBO`, `makeHostBuf`, `ensure`, `ensurePreviewRing`, `uploadGiBuffer`, `createGiHwBuffer`, `ssaoMakeImage` …). Dasselbe für Bilder (`makeImg` ×6) und One-Shot-Command-Buffer (21×). Fehlerpfade sind uneinheitlich (`vkCheck` wirft vs. `return false` vs. unbehandelt, z. B. 9156-9168, 9814-9826, 10061-10066: Rückgabewerte nicht geprüft).
- **Lösung**: `VkBufferEx createBuffer(size, usage, MemKind{DeviceLocal, HostVisibleCoherent, HostMapped})`, `createImage2D(desc)` und `oneShot(std::function)` bzw. Template-Variante (im Cold-Path ok) als private Helfer oder eigener `VulkanResources.h`. Fehlerbehandlung vereinheitlichen. Basis für REN-802/803.
- **Erwarteter Effekt**: -1500 Zeilen, ein Ort für Allokator-Umstellung.
- **Risiko / Verifikation**: Niedrig; mechanische Umstellung, stückweise.

### REN-816 – Handle-Lebenszeit manuell: `Shutdown()` und ~40 `destroy*`-Funktionen mit `if (h) { vkDestroy…; h = VK_NULL_HANDLE; }`
- **ID**: REN-816
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 219-407 (`Shutdown`, 190 Zeilen), 1778-1787, 2611-2657, 2901-2926, 3840-3862, 4523-4546, 5099-5126, 5235-5261, 6396-6410, 6501-6529, 9451-9474, 9721-9737, 10354-10385, 11298-11316, 13306-13349
- **Problem**: Reihenfolgeabhängige manuelle Zerstörung (z. B. „destroyDecalPipelines vor destroyScenePipeline – borrows m_albedoSampler“, 378; „SSR vor destroySSAOTargets“, 379-381). Reihenfolgefehler sind nur per Kommentar abgesichert; Leaks bei neuen Members sind leicht (z. B. `m_whiteArrayView` wird nur in `destroyMaterialResources` freigegeben, das nur bei `m_matReady` sinnvoll durchläuft).
- **Lösung**: Kleine RAII-Wrapper (`Unique<VkImageView>`, `UniqueBuffer {buf, mem}`, `UniqueImage {img, mem, view}`) mit `VkDevice`-Referenz oder Deleter; ein `Frame/DeviceScope`, das beim Verlassen in umgekehrter Deklarationsreihenfolge freigibt. Dadurch `Shutdown` auf wenige Zeilen.
- **Erwarteter Effekt**: Weniger Fehleranfälligkeit; -600 Zeilen; Verhalten gleich.
- **Risiko / Verifikation**: Mittel (Destruktionsreihenfolge der Member in `VulkanRenderer.h` ist dann semantisch relevant). Mit Validation-Layer-Shutdown-Lauf prüfen.

### REN-817 – `DrawViewportFrame` (~480 Zeilen) und `drawDCVk` (~460 Zeilen) als Monolithen mit Flag-Parametern
- **ID**: REN-817
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 710-1193 (`DrawViewportFrame`), 8230-8690 (`drawDCVk`-Lambda in `DrawScene`, ~1000 Zeilen mit Rest), 1109-1131 vs. 1165-1186 (UI-Canvas-Block 2× identisch), 653-661 (3. Variante), 844-965 (Unterwasser-Fog + Caustics inline)
- **Problem**: Verschachtelte Lambdas (`blitPass`, `depthBarrier`, `drawDCVk` ⊃ `noteRingFull`, `drawMatInstance`, `wr`, `drawOne`), Zustandsflags `hdr`, `gbufferPass`, `deferred`, `allowInstancing`, `activeMatScenePipe` im Closure; schwer testbar. `DrawScene` ist >1000 Zeilen. Der UI-Canvas-Block ist zweimal vollständig kopiert; Viewport-/Scissor-Setzen mit `VkViewport vp{...}` ist ≥30× wörtlich wiederholt.
- **Lösung**: Aufteilen in `encodeUnderwater(cmd)`, `encodeBloom(cmd)`, `encodeUiCanvasOntoViewport(cmd)`, `encodeTonemapAndAA(cmd)`; `drawDCVk` → Methoden `drawGraphMaterialDC`, `drawBuiltinDC` mit einer `DrawPassState`-Struktur statt Closure-Flags. `setViewportScissor(cmd, w, h)` Helfer.
- **Erwarteter Effekt**: Lesbarkeit/Reviewbarkeit; kein Laufzeiteffekt (Methoden-Calls gegenüber Lambdas gleich).
- **Risiko / Verifikation**: Niedrig, wenn mechanisch und mit Render-Hash-Tests abgesichert; zuerst REN-801 festlegen, dann refaktorieren, sonst doppelte Arbeit.

---

## Niedrig

### REN-818 – `SetDebugLines` kopiert den Vektor; danach zweite Kopie beim Packen
- **ID**: REN-818
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 9789-9792, 9757-9768
- **Problem**: `m_debugLines = lines;` kopiert pro Frame alle Debug-Linien (Allokation bei Wachstum), `drawLineList` schreibt dann nochmals in den gemappten Buffer. Das Interface (`IRenderer::SetDebugLines(const std::vector<DebugLine>&)`) erzwingt die Kopie; D3D11:8517, D3D12:12659, GL:10091 haben dieselbe Form.
- **Lösung**: Direkt in den gemappten Frame-Ring des **nächsten** Frame-Slots packen oder `assign` (kein Realloc dank bleibender Kapazität); mittelfristig Interface auf `std::span`/Move ändern (Cross-Modul).
- **Erwarteter Effekt**: klein; relevant bei vielen tausend Debug-Linien (Physik-Visualisierung).
- **Risiko / Verifikation**: Niedrig.

### REN-819 – Pro-Frame-Heap im Draw-Pfad (`opaqueDCs`, `transparentDCs`, Ribbon-Container, `writes`)
- **ID**: REN-819
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 8140 (`std::vector<const DrawCall*> opaqueDCs, transparentDCs`), 8153-8155 (`ribbonDCs`, `ribbonMeshes`, `std::unordered_map<const DrawCall*, const GpuMesh*> ribbonMeshByDC`), 8229 (`deferredForwardDCs`), 11589 (`std::vector<VkWriteDescriptorSet> writes` ohne `reserve`), 6569-6570 (Preview), 3026 (siehe REN-810)
- **Problem**: Jeder Frame allokiert diese Vektoren neu (Kapazität geht verloren). Die Ribbon-Map ist eine Pointer-gekeyte `unordered_map` mit Knoten-Allokation pro Trail.
- **Lösung**: Als Member halten und `clear()` (Kapazität bleibt); für Ribbons eine `std::vector<std::pair<const DrawCall*, const GpuMesh*>>` oder ein Indexfeld in `DrawCall` (siehe Cross-Modul-Hinweise). `writes.reserve(64)` bzw. festes Array.
- **Erwarteter Effekt**: wenige Allokationen pro Frame weniger; sehr klein.
- **Risiko / Verifikation**: Niedrig. Achtung: `transparentDCs` hält Zeiger in `ribbonDCs`; dessen `reserve` (8160) muss bestehen bleiben.

### REN-820 – Editor-/Capture-Pfade blockieren und holen Env-Variablen pro Aufruf
- **ID**: REN-820
- **Kategorie**: E
- **Schweregrad**: Niedrig (Editor)
- **Typ**: Performance
- **Ort**: 5905-5993 (`CaptureViewport`: Staging pro Aufruf + `vkDeviceWaitIdle` + `vkQueueWaitIdle`), 6531-6802 (`RenderWorldPreview`: pro Aufruf `vkDeviceWaitIdle`, `vkAllocateCommandBuffers`, `vkQueueWaitIdle`, `std::getenv` ×2, Grid-Vektoren neu: 6674-6682), 6050-6215 (`RenderSceneImage`)
- **Problem**: Pro Preview-Tile (Asset-Browser kann viele Slots aktualisieren) volle GPU-Idle-Synchronisation; `buildPreviewGrid` erzeugt pro Aufruf zwei Vektoren und kopiert sie in `DebugLine`.
- **Lösung**: Preview in den normalen Frame-CB einhängen (Fence pro Slot statt Device-Idle), Grid pro (Extent, Origin) cachen; `getenv` einmal in Initialize lesen. Für `CaptureViewport` Staging-Buffer cachen und Fence statt Queue-Idle nutzen.
- **Erwarteter Effekt**: Editor-Ruhe beim Preview-Update; nur im Editor.
- **Risiko / Verifikation**: Mittel (Synchronisation mit Preview-Rings, 6585-6603). Preview-Dump-Test (`HE_WORLD_PREVIEW_DUMP`) beibehalten.

### REN-821 – Doppelter Swapchain-Aufbau für Sekundärfenster, `renderWindowData` ohne OUT_OF_DATE-Behandlung
- **ID**: REN-821
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 1809-1935 (`createWindowData`) vs. 1536-1595 + 1654-1717 (Primär); 1956-2012
- **Problem**: Format-/Extent-Wahl, Swapchain-/View-/Renderpass-/Framebuffer-Erzeugung sind für Primär- und Sekundärfenster kopiert (Unterschied: Sekundär wählt nur `B8G8R8A8_UNORM`, keine R8G8B8A8-Alternative, kein Depth; `fmts[0]` bei leerer Liste UB). Der Sekundärpfad fängt `VK_ERROR_OUT_OF_DATE_KHR` nicht ab (`vkCheck` wirft) und `vkQueuePresentKHR` wird ignoriert; laut 2038-2050 und `GetCapabilities` ist das Feature ohnehin deaktiviert (`supportsSecondaryWindows == false`).
- **Lösung**: `createSwapchainFor(surface, SwapchainData&)` als gemeinsame Funktion; Sekundärpfad entweder fertigstellen (Recreate) oder als „nicht unterstützt“ löschen, bis A5 umgesetzt ist (Toter Code ~200 Zeilen).
- **Erwarteter Effekt**: -150 bis -200 Zeilen.
- **Risiko / Verifikation**: Niedrig; Pfad ist laut Kommentar nicht erreichbar.

### REN-822 – Veraltete Kommentare, tote Reste, ungenutzte Parameter und Includes
- **ID**: REN-822
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 
  - 8969-8971: „NOTE: a single per-frame bone UBO means only the LAST skinned draw's bone pose is visible …“ – widerspricht dem direkt darunter implementierten Dynamic-Offset-Ring (8982-9011).
  - 10150-10152: „GetCapabilities keeps supportsGlobalIllumination = false“ – tatsächlich `true` (1207).
  - 11, 37: `<glm/gtc/packing.hpp>` doppelt inkludiert.
  - 1287: `(void)0;` Rest; 216: eingerückte Zeile mit Tab `m_shaderManager = VulkanShaderManager();` (überschreibt am Ende von `Initialize` den Member, sofern vorher benutzt – unsicher, ob beabsichtigt).
  - 11891-11896 (`ssaoMakeImage`): Parameter `memTypeBits` wird immer mit `0` aufgerufen und nie benutzt; die Suche geht über einen `std::function`-Callback (Overhead + Hürde), obwohl `findMemoryType` eine Memberfunktion ist.
  - 9476 (`drawSky`): `width/height` ungenutzt; `Render()` 1286-1288.
  - 1304-1308: Meldung „(HE_GPU_DEBUG)“ ohne dass die Variable ausgewertet wird (siehe REN-804).
- **Lösung**: Kommentare korrigieren/löschen, doppelte Includes entfernen, `ssaoMakeImage` in Memberfunktion ohne `std::function` und `memTypeBits` umwandeln.
- **Erwarteter Effekt**: Wartbarkeit.
- **Risiko / Verifikation**: Sehr niedrig.

---

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-1009` (`resolveShaders`/`resolveFragment`): kopiert pro Aufruf die komplette `customShaderFragGlsl` in einen `std::string` und hasht sie (`std::hash<std::string>`). Dasselbe Muster bei jedem Draw in den Parallelstellen `Backends/D3D11/D3D11Renderer.cpp:7009`, `Backends/D3D12/D3D12Renderer.cpp:10974`, `OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815` (und deren Warm-up-Schleifen D3D11:5318, D3D12:4406). Besser: `MaterialAsset` hält `uint64_t shaderHash` (beim Laden/Edit berechnet) und die Library liefert `const std::string&` / `string_view` ohne Kopie. Eine Änderung beschleunigt alle fünf Backends (REN-801).
- `Backends/D3D11/D3D11Renderer.cpp:7079` enthält denselben Block `matGlslTextureArrayMask`/`gIds/gPaths` pro Draw: gleiche Cache-Lösung (Material-Draw-Info-Cache) einmal gemeinsam in `src/HE_Rendering/src/material/` anlegen.
- Cull+Sort mehrfach pro Frame (REN-805): ähnlich D3D11 (`D3D11Renderer.cpp:6260`), D3D12 (`:10253`), OpenGL (`:10923`), Metal (`:7963`, `:17906`) – dort ist die Anzahl nicht geprüft. Ein gemeinsamer `RenderCuller`-Cache pro Frame (`src/HE_Rendering/src/`, `RenderCuller`/`RenderSorter`) wäre backendübergreifend wiederverwendbar.
- `RenderExtractor` (`m_extractor.extract(...)` in 3880, 6871, 7807, 11487, 12651): nur dann günstig, wenn die Walk-Key-Logik (Aspect, Settings) exakt zutrifft; Metrik „Anzahl vollständiger Walks pro Frame“ (`fullExtractCount()`, 6885) im Debug-HUD ausgeben, damit Regressionen auffallen.
- `HE::BuildSkyEnvFaceRow` (`HorizonRendering/SkyEnvBake.h`, REN-810): Variante mit Half-Float-Ausgabe anbieten (Vulkan und Metal nutzen RGBA16F).
- `HE::BuildSkyNoise3D(256)` (`HorizonRendering/SkyNoise3D.h`, REN-808): Bake bei jedem Start; als Pak-Asset oder Disk-Cache bereitstellen, gilt für alle Backends, die das Volumen nutzen.
- `IRenderer::SetDebugLines` (REN-818): Interface auf Move/`span` umstellen (alle Backends).
- Editor (`src/HE_Editor`): Viewport-Größenänderung entprellen (REN-813), sonst löst jeder Drag-Frame `createViewportResources` aus.
- `ContentManager::resolveTextureRef` lädt synchron (Kommentar 3616-3617): Mit REN-802 gekoppelt – Textur- und Mesh-Load im Render-Thread sollten in einen Streaming-Job wandern.

## Unsicher / Messung nötig

- Bloom: 10 getrennte Blur-Passes (1000-1012, je eigener Renderpass + Barrier, Half-Res): ob ein Dual-Kawase/Downsample-Kette mit weniger Passes sichtbar gleich aussieht (Verhaltensänderung!) – nur mit Bildvergleich entscheiden; GPU-Zeit via `gpuTimerBegin/End` (14646) pro Pass messen (derzeit nur „whole-frame“).
- Wie teuer `resolveWorldMaterialScalars`, `refineObjectBounds` (je Objekt `resolveMesh`-Hash-Lookup, 6889-6891) und `cull+sort` bei 50k Objekten sind – Zeitscopes um 3890, 6889, 7840 setzen, bevor REN-805 priorisiert wird.
- Ob `GpuMesh`-Zugriffe `m_meshCache.find(UUID)` pro Draw (7721) relevant sind (Hash von 128-Bit-UUID); ein direkter Zeiger im `RenderObject` (Mesh-Handle) würde sie sparen – Cross-Modul (Extractor).
- Bestätigung, dass die `m_giNodeBuf`/`m_giTriBuf`-Neuallokation bei Wachstum (10317-10329, 10185-10190) nie ein in-flight Frame liest (Kommentar sagt „VK-B revisits“): möglicher Synchronisationsfehler, nicht verifiziert.
- `m_shaderManager = VulkanShaderManager();` (216) – ob die Zuweisung nach den Pipeline-Erzeugungen etwas überschreibt, das vorher befüllt wurde (Header `VulkanRenderer.h`, Slice 9, prüfen).
- Wirkung von REN-803 (Device-Local) hängt von der GPU-Klasse ab (dGPU vs. UMA/iGPU/lavapipe): Messung auf Zielhardware.
- `k_matMaxDraws = 1024` / `k_maxMeshTextures = 1024` / `k_maxDecals` / `k_sceneMatSlots = 16384` / `kMaxSkinnedVk = 256` sind harte Obergrenzen; reale Szenengrößen der Zielprojekte nicht bekannt.

## Gelesen

Ja: `VulkanRenderer.cpp` vollständig (Zeilen 1–14698, in Blöcken von 500-1500 Zeilen, nichts übersprungen). Ergänzend gezielt gegengelesen: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-1009`, `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:1981-1990`, `include/Backends/Vulkan/VulkanRenderer.h` (nur Zeilen für Konstanten/Member per `grep`), sowie `grep`-Stichproben in D3D11/D3D12/GL/Metal für die Cross-Modul-Hinweise.
