# Rendering Slice 2 – Backends/D3D12/D3D12Renderer.cpp (12 904 Zeilen)

## Zusammenfassung

`D3D12Renderer.cpp` ist funktional reif (Frames in Flight, Ring-Buffer pro Slot, Retire-Listen, DRED, DXR-Zweig), aber über die Jahre zu einer einzigen Übersetzungseinheit mit ca. 1 200 Zeilen eingebettetem HLSL und einem ca. 7 300 Zeilen langen Inline-`struct D3D12RendererImpl` gewachsen. Die größten Hebel liegen im Hot Path: Pro Graph-Material-DrawCall werden der komplette GLSL-Quelltext kopiert und gehasht (Zeile 10971-10975), Mesh-/BVH-Puffer liegen dauerhaft im UPLOAD-Heap, alle 3 Kaskaden und bis zu 16 lokale Shadow-Layer werden jeden Frame neu gecullt, sortiert und gezeichnet, und der Debug-Line-Puffer ist nicht pro Frame-in-Flight vorhanden (Datenrennen). Dazu kommen viele einzelne Frame-Kosten (Bloom läuft bei "aus", doppelte Per-Frame-/Cluster-Füllung, volle Atlas-Kopien der GI-Probes, Root-Signature-/Heap-Wechsel pro Material-Draw) und viel Boilerplate (20x Root-Signature-Serialisierung, 8x identische `compile`-Lambdas, 5 parallele Retire-Listen). Funde: 4 Hoch, 17 Mittel, 3 Niedrig (davon 2 reine Vereinfachung). Alle Zeilenangaben sind gegen die Datei verifiziert; Messdaten liegen nicht vor, GPU-seitige Effekte sind als Schätzung gekennzeichnet.

---

## Funde (Hoch zuerst)

### REN-201 – Pro Graph-Material-Draw wird der komplette GLSL-Quelltext kopiert und gehasht
- **ID**: REN-201
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10971-10987` (drawDC12), `:11055`, `:11093`; Ursache in `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952` und `:1000-1009`
- **Problem**: `drawDC12` ruft für jeden DrawCall (jede Instanz-Gruppe, jeden Frame) `resolveShaders(...)` auf. `resolveFragment` kopiert `mat->customShaderFragGlsl` (ein ganzer Shader, typisch mehrere KB bis zehn KB) in einen `std::string` und berechnet `std::hash<std::string>` über den ganzen Text; danach wird `customShaderVertGlsl` genauso kopiert und gehasht. Anschließend holt derselbe Draw `getMaterial()` noch dreimal (precompiledFor, `ma0`, `ma`), je eine Map-Suche. Das Ergebnis (hash, "hat Graph-Shader") ändert sich nur, wenn das Material editiert wird. Die Strings werden nur bei einem PSO-Cache-Miss gebraucht (`GetOrBuildMaterialPSO`).
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (p.m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId,
                                    matHash, matFrag, matVertBody))
{ ... matPso = p.GetOrBuildMaterialPSO(matHash, matFrag, matVertBody, matPre, p.usingHDR, matTransp);
// MaterialShaderLibrary.cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);
```
- **Lösung**: (1) `MaterialShaderLibrary` bekommt eine zweite Methode `resolveShaderKey(cm, id) -> {hash, hasGraph, const MaterialAsset*}`, die den Hash pro `MaterialAsset` cached (Feld im Asset oder `unordered_map<UUID, Entry>`, invalidiert über das vorhandene `InvalidateMaterial`). (2) `drawDC12` holt nur den Key; `frag`/`vertBody` werden erst im Miss-Zweig von `GetOrBuildMaterialPSO` per `const std::string&`-Referenz auf das Asset gelesen (kein Copy). (3) `const MaterialAsset* ma` einmal pro DrawCall holen und an `precompiledFor`/Textur-/Param-Auflösung weiterreichen. Das ist ein Vorgriff auf einen "Draw-Vorbereitungs-Cache pro Material pro Frame".
- **Erwarteter Effekt**: Schätzung: pro Material-Draw ein Wegfall von 2 Heap-Allokationen und von O(Shader-Länge) Hash-Arbeit; bei einigen hundert Material-Draws sind das grob 0,1-1 ms CPU pro Frame (messen). Gilt gleich für D3D11, GL, Metal (Parallelstellen unten).
- **Risiko / Verifikation**: Gering. Beobachtbares Verhalten identisch, solange der Cache bei Shader-Edit/Hot-Reload invalidiert wird (`InvalidateMaterial` existiert, Zeile 12859). Tests: bestehende `tests/test_material_*`-Dateien plus ein neuer Test "Hash ändert sich nach Shader-Edit". Parallelstellen: `D3D11/D3D11Renderer.cpp:7009`, `OpenGL/OpenGLRenderer.cpp:3599`, `Metal/MetalRenderer.mm:12815`.

### REN-202 – Statische Mesh-Puffer (und GI-BVH) liegen dauerhaft im UPLOAD-Heap
- **ID**: REN-202
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:8406-8416` (uploadMesh), `:8727-8751` (Skeletal-VB/IB), `:4644-4666` (createUploadBuffer), `:7042-7047` (giNodeSB/giTriSB)
- **Problem**: `uploadMesh` legt Vertex- und Index-Buffer jedes Meshes mit `createUploadBuffer` an (`D3D12_HEAP_TYPE_UPLOAD`, `GENERIC_READ`) und behält sie so für die gesamte Lebensdauer. Auf diskreten GPUs liegt der Speicher im Host-RAM und wird pro Vertex-Fetch über PCIe gelesen; Statische Geometrie gehört in DEFAULT-Heap (VRAM). Das gleiche gilt für die BVH-Knoten/Dreiecke, die der GI-Compute-Kernel bei jedem Ray-Schritt liest. Zusätzlich ist jedes Mesh ein eigener `CreateCommittedResource` (zwei pro Mesh, jeweils mit eigener Heap-Granularität von 64 KB); bei vielen kleinen Meshes ist das Speicherverschwendung und langsam beim Laden.
- **Code-Auszug**:
```cpp
mesh.vbuf = createUploadBuffer(vbytes, nullptr, interleaved.data());
mesh.ibuf = createUploadBuffer(ibytes, nullptr, indices.data());
```
- **Lösung**: (1) `uploadMesh` erzeugt DEFAULT-Buffer in `COPY_DEST`, kopiert über einen wiederverwendeten Staging-Ring (siehe REN-212) auf der Frame-Command-List und wechselt per Barrier auf `VERTEX_AND_CONSTANT_BUFFER | INDEX_BUFFER`. (2) Mesh-Daten in einen großen DEFAULT-Pool sub-allokieren (ein VB-Pool, ein IB-Pool, `D3D12MA`-ähnlicher Linear-/Free-List-Allocator); `GpuMesh` hält nur noch Offsets. (3) GI-Node-/Tri-Puffer ebenfalls DEFAULT (einmal kopieren, nur bei `giBlasDirty`). Auf UMA-GPUs (Intel iGPU, Steam Deck) ändert sich nichts, auf dGPU entfällt der PCIe-Lesezugriff.
- **Erwarteter Effekt**: Schätzung: auf dGPU spürbar bei geometrielastigen Szenen und beim GI-Traversal (Vertex-Fetch aus VRAM statt Host-RAM); Gewinn ist nur mit GPU-Messung (PIX/Timestamps, Zeile 1452 liefert Frame-GPU-Zeit) belegbar, deshalb "messen".
- **Risiko / Verifikation**: Mittel. Resource-State-Verfolgung muss stimmen; `retire`-Pfad (`processPendingInvalidations`, 4341-4362) muss die neuen Handles auch freigeben. Verifikation: bestehende WARP-Tests (`tests/` mit D3D12 WARP, z. B. Material-Root-Signature-Test) und ein Rendering-Vergleichs-Screenshot (`HE_WORLD_PREVIEW_DUMP`, Zeile 12602).

### REN-203 – Shadow-Pass rendert jeden Frame alle Kaskaden und alle lokalen Layer neu, jeweils mit eigenem Cull + Sort
- **ID**: REN-203
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10641-10717` (renderDepthLayer + Schleifen)
- **Problem**: `renderDepthLayer` ruft für jede der bis zu 3 Kaskaden und bis zu 16 lokalen Atlas-Layer (Spot = 1, Punktlicht = 6 Würfelseiten) `m_culler.cull(...)`, `m_sorter.sort(...)` und `batchDepthCasters(...)` über die gesamte Objektliste auf, löscht den Layer und zeichnet alle Caster. Also bis zu 19 volle O(N)-Culls plus O(N log N)-Sorts pro Frame, auch wenn Licht und Caster statisch sind (typisch: Punktlichter an unbewegter Architektur). Es gibt keine Statik-/Dirty-Erkennung.
- **Code-Auszug**:
```cpp
for (int v = 0; v < nLocalLayers; ++v)
    renderDepthLayer(p.localShadowDsv(v), D3D12RendererImpl::kLocalShadowSize,
                     csm.localViewProj[v], localClip[v], csm.localOwnerEntity[v]);
...
p.m_culler.cull(p.m_renderWorld, viewProj, p.shadowVisible);
p.m_sorter.sort(p.m_renderWorld, p.shadowVisible, p.shadowSorted);
```
- **Lösung**: (1) Lokale Layer cachen: pro Layer eine Signatur aus Licht-Matrix + Hash der Caster-Transformationen im Licht-Frustum (der Extractor kennt Dirty-Zustände der Entities bereits für `GIProbeSceneSignature`, Zeile 7601); Layer nur neu rendern, wenn die Signatur wechselt, sonst überspringen (Textur bleibt erhalten, kein Clear). Die lokalen Layer sind ohnehin "fixed 1024², built once" (Kommentar 1553). (2) Für die Kaskaden: Sort nur nach Mesh (für Instancing) statt voller Tiefensortierung, weil Depth-only keine Reihenfolge braucht. (3) Der Cull pro Layer kann bei Punktlichtern aus einer Grob-Kugel-Prüfung vorgefiltert werden.
- **Erwarteter Effekt**: Schätzung: bei Szenen mit einigen statischen Punkt-/Spotlichtern wird der Shadow-Pass-Aufwand (CPU und GPU) um den Anteil der unveränderten Layer kleiner, im Idealfall um 50-90 % der Shadow-Kosten. Messen: `statDraws` plus `lastGpuFrameMs`.
- **Risiko / Verifikation**: Mittel-Hoch (Cache-Invalidierung ist die klassische Fehlerquelle: bewegter Caster im Frustum, Lichtparameter, Shadow-Auflösung, Device-Reset). Absicherung: Test, der ein Caster-Transform ändert und fordert, dass die Signatur wechselt; visueller A/B mit abgeschaltetem Cache (Umgebungsvariable wie `HE_DEPTH_INSTANCING`).

### REN-204 – Debug-Line-Vertex-/Constant-Buffer existiert nur einmal, nicht pro Frame in Flight (Datenrennen CPU/GPU)
- **ID**: REN-204
- **Kategorie**: E / C
- **Schweregrad**: Hoch
- **Typ**: Beides (Korrektheit + Vereinfachung)
- **Ort**: `D3D12Renderer.cpp:3024-3026` (Anlage), `:3088-3122` (drawDebugLines), `:3099-3100` (Kappung), Member `:3768-3771`
- **Problem**: Alle anderen Per-Frame-Puffer (`perFrameCB[f]`, `skyCBuf[f]`, `ssaoCB[f]`, ...) sind pro Frame-Slot angelegt. `debugVBuf`/`debugCBuf` existieren dagegen nur einmal. `drawDebugLines` schreibt sie per Mapped-Pointer, die GPU liest sie zur Ausführungszeit des Frames N-1/N-2 weiter (3 Frames in Flight, `waitForFrame` wartet nur auf Frame N-3). Ergebnis: flackernde/falsche Linien und im Extremfall inkonsistente Matrix. Zusätzlich verwirft die Prüfung bei mehr als 4096 Linien ALLE Linien, statt zu kappen.
- **Code-Auszug**:
```cpp
debugVBuf = createUploadBuffer(static_cast<UINT64>(8192 * 6 * sizeof(float)), ...
...
if (!debugVBuf || needed > 8192ULL * 6 * sizeof(float)) return;
if (debugVBufPtr) { float* v = reinterpret_cast<float*>(debugVBufPtr); ... }
if (debugCBufPtr) std::memcpy(debugCBufPtr, glm::value_ptr(viewProj), 64);
```
- **Lösung**: `debugVBuf[k_frameCount]`/`debugCBuf[k_frameCount]` plus Pointer-Arrays; `drawDebugLines` bekommt den Frame-Index (wie `drawSky(cl, fi, ...)` schon). Besser: die Debug-Linien in den vorhandenen Per-Frame-Linear-Allocator legen (REN-217). Kappung: `lineCount = min(lines.size(), kMaxDebugLines)`. Der World-Preview (12500) ruft `drawDebugLines` nach einem Voll-Flush auf, bleibt also korrekt.
- **Erwarteter Effekt**: Korrektheit (kein Flackern bei laufender GPU); Kosten: 3x ca. 196 KB Upload. Performance neutral.
- **Risiko / Verifikation**: Gering. Test: Debug-Layer (`HE_GPU_DEBUG`) zeigt solche Rennen nicht; stattdessen reproduzierbar mit GBV oder durch zwei Frames mit unterschiedlichen Linien und `vsync=false`. Parallele: D3D11 nutzt `MAP_WRITE_DISCARD` und ist sicher (`D3D11Renderer.cpp:5626`).

### REN-205 – Jeder Graph-Material-Draw wechselt Heap, Root Signature, PSO und kopiert 17 Deskriptoren
- **ID**: REN-205
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11101-11130` (Block aufbauen), `:11183-11200` (Zustand zurücksetzen), `RenderSorter.h` (`struct SortKey { meshHi, meshLo, distSq, index }`)
- **Problem**: Pro Material-DrawCall: `CopyDescriptorsSimple(17, ...)`, bis zu 6 `CreateShaderResourceView` (inkl. `GetDesc()` in `srvForTexture`, 3550), danach `SetDescriptorHeaps` (Material-Heap), `SetGraphicsRootSignature(m_matRootSig)`, `bindClusterRoots`, `SetPipelineState`, `SetGraphicsRootDescriptorTable`, und nach dem Draw alles wieder zurück (`SetGraphicsRootSignature(rootSig)`, `SetDescriptorHeaps(sceneSrvHeap)`, 2x Root-Table, 3x Cluster-SRV, PSO). Der Sort-Schlüssel gruppiert nur nach Mesh, nicht nach Material/PSO, daher wechseln built-in und Graph-Material-Draws beliebig oft. `SetDescriptorHeaps`-Wechsel gelten auf einigen GPUs als teuer (Pipeline-Flush).
- **Code-Auszug**:
```cpp
cl->SetDescriptorHeaps(1, mheaps);
cl->SetGraphicsRootSignature(p.m_matRootSig.Get());
p.bindClusterRoots(cl);
cl->SetPipelineState(matPso);
...
cl->SetGraphicsRootSignature(p.rootSig.Get());   // nach jedem Draw zurück
p.bindClusterRoots(cl);
```
- **Lösung**: (1) Opaque-Pass in zwei Phasen: erst alle built-in DrawCalls, dann alle Graph-Material-DrawCalls hintereinander (Tiefentest macht die Reihenfolge für Opaque unerheblich); State-Restore nur einmal am Ende der Material-Phase. Transparent bleibt in Sortierreihenfolge. (2) Zusätzlich innerhalb der Material-Phase nach PSO gruppieren (der Material-Hash liegt vor). (3) Mittelfristig: built-in Pfad auf die Material-Root-Signature heben, dann gibt es nur eine Signatur und einen Heap. (4) Deskriptor-Block pro DrawCall nur schreiben, wenn sich Textur-Tupel gegenüber dem vorherigen DrawCall geändert hat (Block-Wiederverwendung innerhalb des Frames).
- **Erwarteter Effekt**: Schätzung: bei gemischten Szenen mit n Material-Draws entfallen 2n Heap-/Root-Signature-Wechsel; Gewinn nur messbar mit PIX; CPU-Seite ca. 10-20 API-Aufrufe pro Draw.
- **Risiko / Verifikation**: Mittel (Reihenfolge der Opaque-Draws ändert Early-Z-Verhalten, Ergebnis sonst gleich; bei `deferredForwardDCs` unverändert). Absicherung: Bildvergleich Deferred/Forward, `tests/test_culling.cpp`-ähnlicher Textvergleich entfällt.

### REN-206 – Instanzierte Graph-Material-Batches: pro Instanz Param-Ring-Slot und 256-Byte-Kopie mit identischem Inhalt
- **ID**: REN-206
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11141-11181` (drawMatInstance), Ringe `k_matMaxDraws = 1024` (3623)
- **Problem**: Für jede Instanz wird `float padded[64]` auf Null gesetzt, `params` hineinkopiert und in einen eigenen Slot des HeParams-Rings geschrieben (256 B), obwohl der Inhalt für alle Instanzen eines DrawCalls identisch ist. Zusätzlich frisst jede Instanz einen Slot des 1024-Slot-Rings; ein Batch mit 1000 Instanzen verbraucht den ganzen Frame-Ring (`noteRingFull`, 11010) und der Rest fällt auf Built-in-PBR zurück ("silent optics change", Kommentar 11008).
- **Code-Auszug**:
```cpp
float padded[64] = { 0.0f };
if (params) std::memcpy(padded, params->data(), std::min(params->size(), size_t(64)) * sizeof(float));
std::memcpy(p.m_matParamPtr[p.frameIndex] + i * k_matSlot, padded, sizeof(padded));
...
cl->SetGraphicsRootConstantBufferView(2, parVA);
```
- **Lösung**: HeParams einmal pro DrawCall (vor der Instanzschleife) in einen Slot schreiben und `parVA` für alle Instanzen wiederverwenden. Die Per-Instanz-U-Daten (`MatU`, 176 B) bleiben. Das halbiert Ring-Verbrauch und Schreibvolumen. Weiter: echte Instanzierung für Graph-Materialien (Vertex-Shader liest `mvp/model` aus dem `t3`-Ring wie `VSMainInstanced`, 612-621) ist eine größere Änderung.
- **Erwarteter Effekt**: Schätzung: -50 % Ring-Schreibvolumen und doppelte Batch-Obergrenze (ca. 1024 -> bis ca. 2048 Instanzen, abhängig von U-Slots).
- **Risiko / Verifikation**: Gering; beobachtbar identisch. Test: DrawCall mit `instanceTransforms.size() == 3` und `paramOverride`, beide Varianten rendern denselben Frame (WARP-Screenshot).

### REN-207 – Shadow-Fallback pro Caster verbraucht 256-Byte-CB-Slots des gemeinsamen Draw-Rings und verdrängt die Geometrie
- **ID**: REN-207
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10681-10692`, `:3425` (`count < 2`), `:90-95` (Kommentar zum Budget), Verbraucher `:11241-11244`, `:11632-11637`
- **Problem**: `drawIdx` ist ein einziger Zähler für Shadow-, Geometrie-, Skinned- und TAA-Velocity-Draws (`k_maxDraws = 16384` Slots a 256 B). Ein Caster-Run der Länge 1 (`count < 2`) oder ein Run bei vollem Instance-Ring nimmt einen vollen 256-Byte-Slot, obwohl `VSDepth` nur `uMVP` (64 B) liest. Bei 19 Shadow-Layern mit vielen einzelnen Meshes ist der Ring schnell leer; dann brechen die Geometrie-Draws still ab (`if (drawIdx >= k_maxDraws) return;`, 10950), der Kommentar in 90-95 bestätigt, dass das Budget wegen dieses Effekts schon zweimal verdoppelt wurde.
- **Code-Auszug**:
```cpp
if (!instPSO || !perInstancePtr[fi] || count < 2 || !RenderSorter::depthInstancingEnabled() ...) return false;
...
PerObjectCB o{}; o.mvp = clip * xf[k]; o.model = xf[k];
std::memcpy(ringPtr + drawIdx * k_cbSlot, &o, sizeof(o));
```
- **Lösung**: `count < 2` auf `count < 1` lockern: auch ein Run der Länge 1 geht über den 128-Byte-Instanz-Ring (65 536 Einträge) statt über einen 256-Byte-CB-Slot. Alternativ für Depth-Only `SetGraphicsRoot32BitConstants(0, 16, ...)` (Root-Signature-Änderung nötig). Zusätzlich: getrennte Zähler pro Pass (Shadow bekommt nie Geometrie-Slots).
- **Erwarteter Effekt**: Entfernt die Kapazitätsgrenze, bei der Geometrie verschwindet; Schreibvolumen pro Caster 256 B -> 128 B.
- **Risiko / Verifikation**: Gering. `RenderSorter::depthInstancingEnabled()` (HE_DEPTH_INSTANCING=0) bleibt als A/B-Schalter. Test: Szene mit 20 000 Einzel-Casters, Geometrie-Draw-Zähler gegen `statDraws` prüfen.

### REN-208 – Bloom-Kette (Bright + 10 Blur-Passes + 22 Barrieren) läuft auch bei deaktiviertem Bloom
- **ID**: REN-208
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:2577-2609` (Bright + Blur-Schleife), `:2616` (Tonemap-Konstanten)
- **Problem**: `runPostFX` führt Bright-Pass und 10 Gaussian-Blur-Passes auf Halbauflösung immer aus; `bloomEnabled == false` setzt nur die Stärke im Tonemap auf 0. Der GPU-Aufwand (11 Fullscreen-Draws, 22 Barrieren, 10x 9-Tap-Blur) fällt also auch dann an, wenn der Nutzer Bloom ausschaltet, um Leistung zu gewinnen.
- **Code-Auszug**:
```cpp
cl->SetPipelineState(bloomBlurPSO.Get());
for (int pass = 0; pass < 10; ++pass) { ... cl->DrawInstanced(3,1,0,0); horiz = !horiz; }
...
const float cb[4]={exposure, bloomEnabled?bloomStrength:0.0f, 0,0};
```
- **Lösung**: `if (bloomEnabled && bloomStrength > 0.0f) { Bright + Blur } else { t1 = srvGpuHandle(0) (Null-SRV, liest 0) }`. Die Blurpass-Zahl (10) zusätzlich als Qualitätsparameter statt Konstante; Vulkan nutzt dieselbe harte 10 (`VulkanRenderer.cpp:1000`).
- **Erwarteter Effekt**: Schätzung: bei "Bloom aus" entfallen ca. 11 Fullscreen-Passes in Halbauflösung (messen, typischerweise 0,2-0,6 ms auf Mittelklasse-GPU).
- **Risiko / Verifikation**: Gering. Bild bleibt identisch, solange der Null-SRV-Pfad 0 liefert (`nullSrv0`, 2042-2048, ist RGBA8-Null). Zwei WARP-Screenshots (Bloom an mit Stärke 0 / Bloom aus) müssen pixelgleich sein.

### REN-209 – GI-Probe-Update kopiert jeden Frame die kompletten Irradiance- und Visibility-Atlanten
- **ID**: REN-209
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:8033-8052` (dispatchGiProbeUpdate)
- **Problem**: Das Kernel-Update schreibt pro Frame nur `budget` (Standard 256) Probes, doch vor jedem Dispatch werden `giIrrTex` (RGBA16F) und `giVisTex` (RG16F) vollständig per `CopyResource` in die Prev-Texturen kopiert, mit 6 Barrieren drumherum. Bei einem Raster mit mehreren Tausend Probes sind das viele MB pro Frame, obwohl die Prev-Daten nur für die Probes dieses Batches gebraucht werden ("texels outside this batch keep their values in the canonical atlases since the kernel never writes them", Kommentar 8037).
- **Code-Auszug**:
```cpp
cl->CopyResource(giIrrPrevTex.Get(), giIrrTex.Get());
cl->CopyResource(giVisPrevTex.Get(), giVisTex.Get());
```
- **Lösung**: Nur die Region des aktuellen Batches kopieren. Die Atlas-Kacheln liegen zeilenweise (`probesPerRow = min(count, 32)`, Kachel 8x8): der Batch `[cursor, cursor+budget)` entspricht höchstens zwei Rechtecken (Wrap). Zwei `CopyTextureRegion`-Aufrufe pro Atlas mit `D3D12_BOX`. Alternative: Kernel liest Prev über einen Ping-Pong und alle Texel, die der Batch nicht schreibt, kopiert ein trivialer Compute; die Region-Kopie ist einfacher.
- **Erwarteter Effekt**: Schätzung: Kopievolumen sinkt auf budget/probeCount (z. B. 256 von 4096 = 6 %).
- **Risiko / Verifikation**: Mittel (Rand/Wrap-Fall, Batch-Ende am Atlas-Ende). Absicherung: `tests/test_gi_*`-Bildvergleich über mehrere Frames, Unit-Test für die Rechteckberechnung (reine Funktion in `GIProbeGrid.h`).

### REN-210 – `updateGiAccel`: pro Frame Matrix-Inverse und Hash-Lookups je Caster, BVH-Bau im Render-Thread, Komplett-Neuanlage der BVH-Puffer
- **ID**: REN-210
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:6985-7000` (Schleife), `:6924-6951` (BuildGIBlas), `:7038-7048` (Neuanlage), `:6962-6967`
- **Problem**: Für jedes Shadow-Caster-Objekt (ungeculled) laufen jedes GI-Frame: `giBlasCache.find(UUID)` (Zeile 6964, plus `GIBlasRange`-Kopie), bei DXR ein zweites `find`, und `glm::inverse(obj.transform)` (volle 4x4-Inverse, Zeile 6996). Bei statischen Szenen ändert sich fast nichts, trotzdem wird das ganze Instanz-Array neu gebaut und hochgeladen. Neue Meshes bauen ihre CPU-BVH synchron im Render-Thread (`buildGiBvh`), und jede neue BLAS lässt `giNodeSB`/`giTriSB` komplett neu anlegen und neu hochladen (O(Gesamtgröße) pro neuem Mesh). Beim Szenenstart mit 200 Meshes ist das O(n²) Datenbewegung.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
giNodeSB = giNodesCpu.empty() ? nullptr : createUploadBuffer(giNodesCpu.size() * sizeof(HE::GiBvhNode), nullptr, giNodesCpu.data());
```
- **Lösung**: (1) `invTransform` aus Affine-Inverse (Rotation/Skalierung transponieren/dividieren) oder pro Objekt cachen, solange das Transform unverändert ist (Extractor-Dirty-Flag). (2) Instanz-Upload nur bei Änderung der Objektliste/Transforms (Signatur existiert für die Probe-Grid-Logik). (3) BLAS-Aufbau auf Job-System auslagern und mit Kapazitätswachstum (Verdopplung) statt Komplett-Neuanlage appenden. (4) Zwei Hash-Lookups auf einen reduzieren (Struct mit SW- und HW-BLAS).
- **Erwarteter Effekt**: Schätzung: pro GI-Frame O(n) Inverse und Lookups entfallen; Ladeverhalten bei vielen Meshes deutlich flüssiger. Parallelstellen: `D3D11Renderer.cpp:3184`, `OpenGLRenderer.cpp:5974`, `MetalRenderer.mm:8245` haben dieselbe `glm::inverse`-Zeile.
- **Risiko / Verifikation**: Mittel. Test: `tests/test_gi_bvh.cpp` existiert für die BVH; ergänzen um Vergleich Affine-Inverse vs `glm::inverse` bei nicht-uniformer Skalierung.

### REN-211 – `fillPerFrame` und `fillMatLight` (inkl. `BuildClusterLights`) laufen pro Frame doppelt
- **ID**: REN-211
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10316-10331` (uploadClusters), `:10393` und `:10852` (fillPerFrame), `:10523` und `:10854` (fillMatLight)
- **Problem**: Beide Lambdas werden einmal mit "GI aus" vorbefüllt und später mit der endgültigen GI/AO/SSR-Entscheidung neu gefüllt (der Kommentar bestätigt: "last write wins"). Jeder Aufruf führt in `fillPerFrame` im Cluster-Modus `HE::BuildClusterLights(...)` aus (Clustering aller Lichter gegen bis zu 3456 Zellen), kopiert danach drei Upload-Puffer (Lights/Grid/Indices bis 256 KB) und `BuildDirectionalLightWindow`; `fillMatLight` füllt 832 Byte Lighting inkl. mehrerer Helfer (`FillMaterialLightWindow`, `FillMaterialWeather`, `GetWaterRipples()`, ...). Die erste Füllung wird von keinem Pass gelesen, der vor der zweiten läuft (Shadow, GI, SSAO nutzen eigene Puffer), solange der Backbuffer-Zweig des Render-Graphen immer läuft.
- **Code-Auszug**:
```cpp
fillPerFrame(false, p.ssaoEnabled && p.ssaoReady);   // 10393
...
fillPerFrame(giShadingActive, aoWanted, ssrResult != nullptr);   // 10852
```
- **Lösung**: Erste Füllung nur machen, wenn der Backbuffer-Zweig nicht erreicht wird (sonst entfernen). Falls doch nötig: Cluster-Build einmal (nur das GI-Masken-Kanalfeld hängt von `giActive` ab, siehe Aufruf `BuildClusterLights(..., giActive && p.giLocalMaskTex)`) und die zweite Füllung patcht nur `params.z`.
- **Erwarteter Effekt**: Schätzung: -1 `BuildClusterLights` + 3 Memcpy + 1 `Lighting`-Fill pro Frame (grob 20-100 µs bei vielen Lichtern, messen).
- **Risiko / Verifikation**: Gering-Mittel: nur sicher, wenn kein Pass die CB vor der zweiten Füllung braucht. Verifikation: Debug-Layer-Lauf und Test mit Szene ohne Backbuffer-Pass (Early-Return Zeile 10257).

### REN-212 – Textur-Upload: je Textur ein eigener committed UPLOAD-Buffer, mehrere Vektor-Allokationen, synchron im ersten Draw
- **ID**: REN-212
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:4159-4250` (uploadTexture2D), `:4132-4146` (allocAlbedoSlot), `:3995-4010` (UI-Bilder), `:3588-3602` (Graph-Texturen)
- **Problem**: Jeder erste Draw einer Textur legt auf dem Render-Thread einen `CreateCommittedResource` im UPLOAD-Heap an (Kosten: Heap-Allokation und Kernel-Aufruf), erzeugt drei `std::vector` (`footprints`, `numRows`, `rowSizes`) und kopiert die gesamte Mip-Kette zeilenweise. Das passiert mitten im Frame (Hitch), ohne Budget pro Frame, und der Staging-Buffer wird erst nach `k_frameCount + 2` Frames freigegeben. Laden einer Szene mit 300 Texturen ergibt 300 Kernel-Allokationen in wenigen Frames.
- **Code-Auszug**:
```cpp
std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subs);
std::vector<UINT> numRows(subs);
std::vector<UINT64> rowSizes(subs);
ComPtr<ID3D12Resource> uploadBuf = createUploadBuffer(uploadSize, &mapped);
```
- **Lösung**: (1) Linearer Upload-Ring pro Frame-Slot (z. B. 32-64 MB, wächst), Texturen werden hineinkopiert, Retire entfällt (gleicher Frame-Fence). (2) Footprint-Arrays als `thread_local`/Member-Scratch. (3) Pro Frame Upload-Budget (z. B. 8 MB), Rest im nächsten Frame ("Pop-in" statt Hitch; Fallback bleibt Flat/Null-SRV wie heute). (4) Optional Copy-Queue plus Fence.
- **Erwarteter Effekt**: Schätzung: weniger Frame-Spitzen beim Streaming; gemeinsam mit REN-202 (Mesh-Staging) ein einziger Mechanismus.
- **Risiko / Verifikation**: Mittel (State-/Lifetime-Fehler). Test: `tests/test_texture_array*` (Payload-Layout, `HE::textureArrayPayloadValid`) bleibt maßgeblich; WARP-Test mit 100 Texturen.

### REN-213 – Graph-Material-PSOs und FXC laufen synchron im Draw-Loop, ohne Disk-Cache
- **ID**: REN-213
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:9021-9219` (GetOrBuildMaterialPSO), Aufruf `:11006`, Warmup `:4396-4428`
- **Problem**: Bei einem Cache-Miss ruft der erste Draw des Frames auf dem Render-Thread `D3DCompile` (VS+PS), SPIRV-Cross (über `m_matShaderLib`) und `CreateGraphicsPipelineState` auf; Kosten pro Material typisch Zehner bis Hunderte ms. `WarmupMaterials` entschärft das nur für vorab bekannte IDs. Es gibt keinen On-Disk-Cache für DXBC und keinen `ID3D12PipelineLibrary`, jeder Start kompiliert alles neu.
- **Code-Auszug**:
```cpp
if (FAILED(D3DCompile(vsSrc.c_str(), vsSrc.size(), "matVS", nullptr, nullptr, "main", "vs_5_0", cflags, 0, &vs, &cerr)))
```
- **Lösung**: (1) DXBC-Cache auf Platte, Schlüssel = `hash + cflags + Compiler-Version`; vor `D3DCompile` prüfen. (2) PSO-Bau auf Job-Threads, bis zur Fertigstellung Draw überspringen oder Built-in-Fallback (wie Pak-Variante). (3) `ID3D12PipelineLibrary` für die PSOs. Teilweise vorhanden: Pak-Precompiled-Pfad (`precompiledFor`).
- **Erwarteter Effekt**: Schätzung: keine Frame-Spitzen beim ersten Sichten eines Materials, deutlich kürzere Folgestarts.
- **Risiko / Verifikation**: Mittel (Cache-Invalidierung bei Shader-Änderung, Treiberwechsel). Test: Cache-Treffer = identische Bytecode-Bytes.

### REN-214 – Beim Start wird dieselbe Scene-HLSL-Quelle bis zu zehnmal von FXC kompiliert
- **ID**: REN-214
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance (Startzeit)
- **Ort**: `D3D12Renderer.cpp:5192-5298` (createPipeline: VSMain, PSMain, VSMainInstanced, VSDepth, VSDepthInstanced), `:8621-8624` (PSMain nochmals), `:9611-9648` (VSMain, GBufPS, VSMainInstanced nochmals), `:3206` (VSMain im World-Preview); insgesamt `grep -c "D3DCompile("` = 26
- **Problem**: `PSMain` wird mit identischen Parametern (Quelle, Entry, Profil, Flags) zweimal kompiliert (5201 und 8623), `VSMain` dreimal, `VSMainInstanced` zweimal. Jeder Aufruf verarbeitet die Quelle (ca. 500 Zeilen plus `kSkyFuncHLSL`) erneut. Dazu acht fast identische `compile`-Lambdas (siehe REN-222).
- **Code-Auszug**:
```cpp
const std::string sceneSource = std::string(kSkyFuncHLSL) + kSceneHLSL;   // 5192, 8621, 9611, 3206
D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &cerr)
```
- **Lösung**: Ein kleiner `ShaderBlobCache` im Impl (`map<(entry,profile) -> ComPtr<ID3DBlob>>`) für die Scene-Quelle; `createPipeline` füllt ihn, `createSkinnedPipeline`, `createDeferredPipelines`, `ensureWorldPreviewPipeline` lesen daraus. Dazu `sceneSource` einmal als Member.
- **Erwarteter Effekt**: Schätzung: 4-5 FXC-Aufrufe weniger beim Start (zusammen vermutlich 50-300 ms, messen).
- **Risiko / Verifikation**: Gering. Flags (`_DEBUG`) bleiben pro Build gleich.

### REN-215 – `SetMoonTexture` zerstört die alte Textur, bevor die GPU damit fertig ist
- **ID**: REN-215
- **Kategorie**: E (Korrektheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung / Korrektheit
- **Ort**: `D3D12Renderer.cpp:12664-12740` (`p.moonTex12.Reset()` in Zeile 12667, `waitForAllFrames()` erst in Zeile 12707)
- **Problem**: Die Textur wird sofort freigegeben, obwohl Frames in Flight noch den Deskriptor in `skyEnvHeap[0]` samplen können (Sky-Pass jeden Frame). Ein Wechsel der Mondtextur kann damit eine zerstörte Ressource referenzieren (Device-Removed/TDR im ungünstigen Fall). Gleiche Reihenfolge-Frage bei `CreateImGuiTexture` (dort wird nichts freigegeben, daher unkritisch).
- **Code-Auszug**:
```cpp
auto& p = *m_impl;
p.moonTex12.Reset();
...
p.waitForAllFrames();   // erst 40 Zeilen später
```
- **Lösung**: `waitForAllFrames()` vor dem `Reset()` oder die alte Textur in `m_retiredTextures` schieben (`emplace_back(std::move(p.moonTex12), k_frameCount + 2)`), wie bei den anderen Hot-Reload-Pfaden.
- **Erwarteter Effekt**: Entfernt ein mögliches Use-After-Free der GPU-Ressource beim Mondtextur-Wechsel.
- **Risiko / Verifikation**: Gering. Test: zweimal `SetMoonTexture` hintereinander mit laufendem Rendern unter `HE_GPU_DEBUG`.

### REN-216 – UI-Pass: String-Schlüssel pro Quad und Frame, ein Draw pro Quad, 256-Byte-Slot für 176 Byte
- **ID**: REN-216
- **Kategorie**: B / C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:4861-4977` (renderUIPass12), `:3995-4010` (uiImageSlotFor), `:3577-3580` (graphTexKey), `:4941`
- **Problem**: (1) Für jedes texturierte Quad (`type == 0 && textureAssetId != 0`) baut `graphTexKey` zweimal pro Frame einen `std::string` (`to_string(hi) + ":" + to_string(lo)`, drei Allokationen) und sucht ihn in einer `unordered_map<string,...>` (Zeile 4873/3997 im Vor-Pass, Zeile 4941 im Zeichen-Pass). (2) Jedes Glyph-Quad ist ein eigener `DrawInstanced(4,1,0,0)` mit eigenem Root-CBV und 256-Byte-Slot (176 genutzt), bis zu 4096 pro Frame; Text ist der Normalfall in UI-Szenen. (3) `m_uiFontAtlases.find` pro Glyph, zwei Durchläufe über `uiObjects`.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
...
const auto it = m_uiImageCache.find(graphTexKey(obj.textureAssetId, {}));
```
- **Lösung**: (1) UI-Bild-Cache als `unordered_map<HE::UUID, UIImage12>` (wie `decalTexCache`, 3681) statt String-Schlüssel; Invalidierung entsprechend umstellen (4333-4338). (2) Slot pro Objekt im Vor-Pass in ein Scratch-Array schreiben, Zeichen-Pass liest den Slot (kein zweiter Lookup). (3) Glyph-Quads auf einen Structured-Buffer (`StructuredBuffer<UIQuad>` + `SV_InstanceID`) mit einem Draw pro (Atlas, Clip)-Run umstellen, ähnlich dem Instance-Ring `t3`. (4) UICB-Stride auf 192 B reduzieren oder Quads in den Instance-Ring packen.
- **Erwarteter Effekt**: Schätzung: bei 500 Glyphen von 500 Draws auf wenige Draws; String-Allokationen pro Frame entfallen.
- **Risiko / Verifikation**: Mittel (Clip-Wechsel `applyClip` muss Runs trennen, Reihenfolge bleibt Painter-Order). Test: `tests/test_ui_widgets*`-Rendering vergleichen; WARP-Screenshot der Widgets.

### REN-217 – Großzügig dimensionierte, eager angelegte Upload-Ringe (ca. 65 MiB) und viele Einzelringe
- **ID**: REN-217
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `D3D12Renderer.cpp:95-98` (Konstanten), `:5339-5360`, `:5557-5569`, `:7359`, `:4822-4832`, `:8681-8686`
- **Problem**: Aus den Konstanten `k_maxDraws = 16384`, `k_cbSlot = 256`, `k_maxInstances = 65536`, `k_instStride = 128` folgt: `perObjectRing` 4 MiB, `perInstanceRing` 8 MiB, `ssaoPosPerObjRing` 4 MiB, `giGBufObjRing` 4 MiB, `m_uiCB` 1 MiB, `m_bonesCB` je 2 MiB (256 x 8192 B), alles x 3 Frames = grob 65 MiB Upload-Speicher, beim Start angelegt, auch wenn SSAO/GI/UI nie benutzt werden (`createGiPipelines` läuft bewusst eager, 5393). Jedes Feature hat einen eigenen Ring plus eigene Slot-Zähler und eigene Overflow-Logik (`drawIdx`, `gbufIdx`, `posDrawIdx`, `skinnedIdx`, `m_matDrawCursor`, `instRingCursor`).
- **Code-Auszug**:
```cpp
perObjectRing[f]   = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
ssaoPosPerObjRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
giGBufObjRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
```
- **Lösung**: Ein `FrameUploadArena` pro Frame-Slot (ein großer UPLOAD-Buffer, Bump-Allocator mit 256-B-Ausrichtung, `alloc(bytes) -> {cpuPtr, gpuVA}`, `reset()` pro Frame, Wachstum durch Doppelpuffer-Strategie). Alle Ringe (Object, Instance, SSAO, GI, UI, Debug, Bones, Decal) werden zu `arena.alloc(...)`-Aufrufen; Overflow wird an einer Stelle behandelt und geloggt. Damit lösen sich auch REN-204 und REN-207 mit.
- **Erwarteter Effekt**: Speicher: bedarfsgesteuert statt Worst-Case (Schätzung: 10-20 MiB typisch statt 65 MiB); Code: ca. 300-400 Zeilen Ring-Boilerplate weniger.
- **Risiko / Verifikation**: Mittel. Alle `ringPtr`/`ringBase`-Verwendungen müssen migriert werden; WARP-Test mit Frame-Wiederholung und Debug-Layer.

### REN-218 – Viele vollständige GPU-Drains (32x `waitForAllFrames`) auf Pfaden, die auch zur Laufzeit laufen
- **ID**: REN-218
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:12424`/`:12638` (RenderWorldPreview, pro Aufruf zwei), `:8327`/`:8342` (buildGiHwBlas, pro neuem Mesh zwei), `:12794`/`:12818` (CreateImGuiTexture, pro Icon zwei), `:12707`/`:12731` (SetMoonTexture), `:1685` und `:1990` (createViewportRT -> createPostFXResources, doppelt), `:5683`/`:5708`/`:5753`/`:5777`, `:2755`/`:2780` (Init)
- **Problem**: `waitForAllFrames()` legt CPU und GPU still (Signal + Wait). (1) Jeder `RenderWorldPreview`-Aufruf (mehrere Scene-Viewports pro Frame möglich) drainiert zweimal die komplette Queue, also verliert der Editor-Viewport die CPU/GPU-Überlappung. (2) Jede neue DXR-BLAS wartet zweimal innerhalb des laufenden Frames (`updateGiAccel` läuft in `DrawScene` bei offener Frame-Liste). (3) `CreateImGuiTexture` pro Content-Browser-Icon und Logo zwei Drains. (4) Jede Viewport-Größenänderung (Ziehen eines Dock-Splitters) ruft `createViewportRT` -> `waitForAllFrames` und danach in `createPostFXResources` erneut.
- **Code-Auszug**:
```cpp
p.waitForAllFrames();           // 12424: vor dem Preview
if (!p.ensureWorldPreviewTarget(...)) return nullptr;
...
p.cmdQueue->ExecuteCommandLists(1, lists);
p.waitForAllFrames();           // 12638: nach dem Preview
```
- **Lösung**: (1) Preview: eigener Ring pro Preview-Slot x Frame (CB/Bones) plus Fence; Queue-Reihenfolge reicht, die Hauptqueue muss nicht geleert werden. Alternativ alle Previews eines Frames in eine Liste und ein Drain. (2) BLAS-Builds der Frame-Liste hinzufügen (`BuildRaytracingAccelerationStructure` ist im Frame-`giCmdList4` erlaubt, dahinter UAV-Barrier) und Scratch per Retire freigeben. (3) `CreateImGuiTexture`/`SetMoonTexture`/Init-Uploads über eine gemeinsame `uploadAndFlush`-Hilfsfunktion (siehe REN-222), die mehrere Uploads zu einem Drain bündelt. (4) In `createViewportRT` den zweiten Drain entfernen (der erste hat die GPU schon geleert und seither nichts eingereiht).
- **Erwarteter Effekt**: Schätzung: Editor-Frametime bei mehreren Scene-Viewports deutlich besser (jeder Drain kostet bis zu eine volle Frame-Latenz); Ladezeit mit vielen Icons/Meshes kürzer.
- **Risiko / Verifikation**: Mittel (Synchronisation). `RenderWorldPreview`-Test via `HE_WORLD_PREVIEW_DUMP` (Bildvergleich vorher/nachher).

### REN-219 – Himmel wird als erster Pass über die komplette Bildfläche mit Volumenwolken berechnet und danach überzeichnet
- **ID**: REN-219
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10225-10231` (Aufruf vor allen Draws), `:3036-3085` (drawSky), `:2933-2944` (`DepthEnable = FALSE`), Fallback-Shader `:279-357`
- **Problem**: Der Sky-Pixel-Shader (16 Raymarch-Schritte, je 3 Licht-Schritte mit 4+3 Noise-Fetches, dazu Sterne, Nebel, Aurora) läuft für jedes Pixel des Viewports, bevor die Geometrie gezeichnet wird; überdeckte Pixel (bei Innenräumen oder Terrain meist der größere Teil) werden danach überschrieben. Der Sky-PSO hat keinen Tiefentest.
- **Code-Auszug**:
```cpp
// Sky is independent of scene geometry — draw it before any early returns ...
p.drawSky(cl, p.frameIndex, glm::inverse(svp), p.m_renderWorld.sunDirection, m_environment, ...);
```
- **Lösung**: Sky nach den Opaque-Draws zeichnen, mit `DepthEnable = TRUE`, `DepthFunc = EQUAL`/`LESS_EQUAL` gegen 1.0 und Vertex-Z = 1.0 im `VSSky` (nur Pixel ohne Geometrie werden geshadet). Der Deferred-Pfad rechnet bereits mit "Sky bleibt wo d >= 1" (Kommentar 11329-11334) und müsste Sky vor dem Resolve anders einordnen. Alternativ Early-Out per Stencil-Maske.
- **Erwarteter Effekt**: Schätzung: GPU-Zeit des Sky-Passes sinkt proportional zur Geometrie-Abdeckung (Innenraum bis auf wenige Prozent). Muss gemessen werden, Gewinn hängt vom Cloud-Coverage-Regler ab (`coverage <= 0` überspringt schon den Raymarch).
- **Risiko / Verifikation**: Mittel-Hoch (Passreihenfolge, SSR-Farb-History, Deferred, Decals und Transparenz greifen darauf zu). Nur nach GPU-Messung angehen; Bildvergleich je Pfad. Backend-übergreifend gleiche Struktur in GL/Vulkan/D3D11/Metal.

### REN-220 – TAA-Velocity: ein Draw pro Objekt und zwei Hash-Operationen pro Objekt und Frame
- **ID**: REN-220
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11597-11659`, Member `:1882` (`unordered_map<uint32_t, glm::mat4>`)
- **Problem**: Der Velocity-Pass zeichnet jedes sichtbare Objekt einzeln (Root-CBV 192 B, kein Instancing, `DrawIndexedInstanced(mesh->indexCount, ...)` ohne `DrawIndexRange`), und nutzt `taaPrevTransforms.find(entityId)` plus `taaCurTransforms[entityId] = transform` (zwei Hash-Zugriffe, Map-Knoten-Allokation bei jedem neuen Objekt) pro Objekt und Frame. `taaCurTransforms.clear()` gibt die Knoten jedes Frame frei und allokiert neu.
- **Code-Auszug**:
```cpp
const auto it = p.taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != p.taaPrevTransforms.end()) ? it->second : obj.transform;
p.taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Statt `unordered_map` ein Vektor `{entityId, transform}` pro Frame, sortiert nach `entityId`, mit zwei-Zeiger-Merge oder (einfacher) `vector<glm::mat4>` indiziert über den Objekt-Index plus Parallel-Array `prevIndex` (aus Extractor). Für Instanzgruppen den Instance-Ring nutzen (zwei Matrizen pro Instanz, `mvpPrev` zusätzlich).
- **Erwarteter Effekt**: Schätzung: Entfernt O(n) Hash-Operationen und Allokationen im TAA-Pfad (Messung bei >2 000 Objekten sinnvoll).
- **Risiko / Verifikation**: Gering-Mittel (Reihenfolge der History muss deterministisch bleiben). Test: zwei Frames mit bewegtem Objekt, Velocity-RT per Readback vergleichen.

### REN-221 – `resolveMesh` und `refineWorldBounds` laufen für jedes Objekt mehrfach pro Frame
- **ID**: REN-221
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10237-10241` (Bounds), `:10657` (pro Shadow-Batch und Layer), `:10959` (drawDC12), `:6612` (SSAO), `:7838` (GI), `:11618` (TAA); Definition `:8440-8482`
- **Problem**: Jedes Objekt wird pro Frame mindestens in `DrawScene`-Bounds-Schleife, in 1-19 Shadow-Layern, im SSAO-Prepass, im GI-Prepass, im Geometry-Pass und im TAA-Pass über `meshCache.find(UUID)` (128-Bit-Hash) aufgelöst. `refineWorldBounds` rechnet die Welt-AABB jeden Frame neu, auch für unbewegte Objekte. Außerdem nimmt der nicht-gecookte Pfad (`resolveMesh`, 8458-8478) pro Vertex drei `insert`-Aufrufe mit Initializer-Liste (Loose-Assets im Editor).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : p.m_renderWorld.objects)
    if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager))
        obj.refineWorldBounds(mesh->localBounds);
```
- **Lösung**: Bei der Bounds-Schleife den `GpuMesh*` in einem Frame-Scratch-Array (`vector<const GpuMesh*> m_objMesh`, parallel zu `objects`) ablegen und in allen Folgepässen per Objekt-Index nutzen; Draw-Calls tragen ohnehin `meshAssetId`, daher Index in `DrawCall` ergänzen. `refineWorldBounds` nur bei geänderter Transform (Dirty aus Extractor). Interleaving im Loose-Pfad mit einem Index-Loop auf vorallokiertes `built.resize(n*8)`.
- **Erwarteter Effekt**: Schätzung: pro Objekt 5-25 Hash-Suchen pro Frame weniger (grob 0,2-1 ms bei 5 000 Objekten, messen).
- **Risiko / Verifikation**: Gering. Zeiger bleiben gültig, solange `meshCache` nicht rehasht (`unordered_map` hält Knoten stabil; Entfernen nur in `processPendingInvalidations`, vor dem Zeigerbau).

### REN-222 – Boilerplate: 20x Root-Signature-Serialisierung, 8x `compile`-Lambda, 17x `_DEBUG`-Flags, 5 Retire-Listen, 5 One-Shot-Uploads
- **ID**: REN-222
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `compile`-Lambdas `:2112`, `:2303`, `:2801`, `:3192`, `:5404`, `:6180`, `:7134`, `:9590`; `D3D12SerializeRootSignature` 20 Treffer (z. B. `:2146`, `:2208`, `:2263`, `:3244`, `:4778`, `:5114`, `:6087`, `:7167`, `:8607`, `:9304`); Retire-Listen `:1680`, `:3894`, `:4053`, `:4072`, `:4077` mit 5 gleichen Sweeps `:11842-11870`; One-Shot-Uploads `:2700-2793`, `:5632-5720`, `:5724-5789`, `:12664-12740`, `:12751-12823`
- **Problem**: Derselbe Code wiederholt sich: (a) `D3DCompile`-Lambda + Fehlerlog (8 Kopien, leichte Signaturunterschiede), (b) `#ifdef _DEBUG flags |= D3DCOMPILE_DEBUG ...` (17 Kopien), (c) Serialize + `CreateRootSignature` + Fehlerlog (20 Kopien), (d) `D3D12_SHADER_RESOURCE_VIEW_DESC` mit Format/Dimension/Mapping/MipLevels (>30 Kopien, obwohl `srvIntoHeapSlot` in 7582 existiert), (e) Texture-Anlage `makeRT`/`makeTex` Lambdas (`:1906`, `:2001`, `:5823`, `:5963`, `:6015`, `:7450`), (f) fünf parallele "(Ressource, Countdown)"-Listen mit identischem Sweep und der Zahl `k_frameCount + 2` an ~12 Stellen, (g) fünf One-Shot-"Upload + Reset + Execute + Wait"-Blöcke mit je zwei `waitForAllFrames`. Zusammen ca. 1 500-2 000 Zeilen.
- **Code-Auszug**:
```cpp
ComPtr<ID3DBlob> sig, err;
if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
    FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&out))))
```
- **Lösung**: Kleine Helfer im anonymen Namespace oder `D3D12Util.h`: `bool compileHlsl(src, entry, profile, name, ComPtr<ID3DBlob>&)` (ein `constexpr UINT kFxcFlags`), `bool makeRootSig(device, desc, ComPtr<ID3D12RootSignature>&, const char* what)`, `D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreenPsoDesc(rs, vs, ps, rtFmt)` mit Blend-/Raster-Voreinstellungen, `srvTex2D(device, res, fmt, cpuHandle)`, `makeRenderTarget(...)`, und ein `DeferredRelease`-Queue (Fence-Wert statt Frame-Countdown, exakter und ohne Sweep pro Liste) sowie `uploadTexture1Shot(...)` für die Init-/Editor-Pfade. Die Funktionen sind rein mechanisch und lassen sich in Schritten ersetzen.
- **Erwarteter Effekt**: Lesbarkeit und Fehleranfälligkeit (ein Stück Code statt 20); keine Laufzeitänderung (außer exakteres Retire).
- **Risiko / Verifikation**: Gering (mechanisch). Verifikation: `D3D12MaterialRootSignature.h`-WARP-Tests aus `tests/` laufen weiter; Build mit/ohne `HE_HAVE_SHADERC`.

### REN-223 – Eine Übersetzungseinheit mit ca. 7 300 Zeilen Inline-Struct und 1 200 Zeilen eingebettetem HLSL
- **ID**: REN-223
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D12Renderer.cpp:122-1213` (HLSL-Strings), `:1412-8760` (`struct D3D12RendererImpl` mit allen Passes), nur wenige Member sind ausgelagert (`createMaterialResources` 8774, `createSkyEnvCube` 8935, `GetOrBuildMaterialPSO` 9021, `EnsureDecalPipeline` 9228, `createDeferredPipelines` 9577)
- **Problem**: Post-FX, Underwater, TAA, Sky, Debug, World-Preview, UI, SSAO, SSR, GI (SW + DXR), Materialsystem, Decals, Deferred und Frame-Loop teilen eine Datei und einen Zustand. Folgen: jede Änderung baut 12,9k Zeilen neu (Template-/Header-schwere Includes `d3d12.h`, `glm`, `RenderGraph`), Merge-Konflikte, und die Reviewbarkeit sinkt (CLAUDE.md warnt bereits: nie ganz lesen). Die HLSL-Quellen (Sky-Fallback, Scene, UI, FXAA/SMAA, Bloom) liegen als Raw-Strings im Code statt in `Backends/D3D_Shared/HlslSources.h` bzw. `.hlsl`-Ressourcen.
- **Lösung**: Impl in Features aufteilen, die bereits als Muster vorhanden sind (siehe Out-of-Line-Funktionen): `D3D12Impl.h` (Struct-Deklaration), `D3D12PostFx.cpp`, `D3D12Ssao.cpp`, `D3D12Ssr.cpp`, `D3D12Gi.cpp` (+DXR), `D3D12Ui.cpp`, `D3D12Materials.cpp`, `D3D12Decals.cpp`, `D3D12Frame.cpp` (DrawScene/Render). HLSL: D3D12-eigene Strings nach `D3D12Hlsl.h`. Reihenfolge: zuerst HLSL auslagern (kein Risiko), dann pro Feature ein Commit.
- **Erwarteter Effekt**: Schätzung: Inkrementelle Build-Zeit der Datei sinkt auf ein Drittel bis Viertel; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering (reine Verschiebung); Build für alle Plattformen/Backends; `tests/test_culling.cpp` prüft Text-Drift von `kSceneHLSL` gegen andere Backends (`"drift test"` Kommentar 771-777), also muss diese Datei-/Symbolreferenz beim Auslagern angepasst werden.

### REN-224 – Kleinfunde: doppelte Includes, tote/unnötige Berechnung, redundante Clears, Per-Frame-Vektoren, Fallback-Sky
- **ID**: REN-224
- **Kategorie**: F / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: siehe Liste
- **Problem / Lösung** (je Zeile belegt):
  1. `#include <cmath>` doppelt (`:53`, `:63`); Zeile 37 enthält zwei angehängte Kommentare (`// TAA jitter sequence ... // GL depth (-1..1) -> D3D depth (0..1)`, der zweite gehört zu `ClipSpace.h` Zeile 36).
  2. `drawSky` baut `SkyCB cb` immer (`:3058-3068`), obwohl bei `skyFullModel` nur `sp` kopiert wird (`:3071`); Bau in den `else`-Zweig verschieben.
  3. `makeTex`-Lambda in `ensureGiShadowTargets` hat den ungenutzten Parameter `clearColor` (`:7450`, `(void)clearColor` in `:7467`); entfernen.
  4. SSAO-Kernel hartkodiert `32` (`:5800`) statt `HE::kSsaoKernelSize`; `ssaoBias` wird in `SetSSAOSettings` ständig auf 0.025 gesetzt (`:12837`).
  5. SSAO-/Blur-Ziele werden voll geclear't (`:6696`, `:6731`), obwohl das Fullscreen-Dreieck jedes Pixel überschreibt (3 Clear-Kommandos pro Frame sparbar).
  6. Pro Frame neu angelegte Vektoren in `DrawScene`: `opaqueDCs`, `transparentDCs` (`:10735`), `deferredForwardDCs` (`:10948`), `ribbonDCs/ribbonMeshes/ribbonMeshByDC` (`:10877-10879`); als Member mit `clear()` wiederverwenden.
  7. `SetDebugLines` kopiert den Vektor jedes Frame (`:12661`); `RenderWorldPreview` wandelt Gitter-Floats in `DebugLine` und `drawDebugLines` zurück in Floats (`:12492-12500`, `:3103-3108`).
  8. Fallback-Sky `kSkyPSHLSL12` (`:122-422`, ca. 300 Zeilen) ist die dritte Kopie der Sky-Logik (neben `kSkyFS` und der Metal-Spiegelung) und weicht schon ab ("old single-colour nebula"). Wenn Windows-Builds immer `HE_HAVE_SHADERC` haben, kann er entfallen; sonst als vorkompilierter HLSL-Blob aus dem Cross-Compiler mitliefern. Unsicher, ob ein Windows-Build ohne ShaderC unterstützt wird.
  9. `UICB` belegt 256-B-Slots für 176 B (`:4887-4891`, siehe REN-216).
- **Erwarteter Effekt**: Klein; Lesbarkeit und einzelne Allokationen/Befehle weniger.
- **Risiko / Verifikation**: Sehr gering. Punkt 8 braucht eine Entscheidung zur Build-Matrix.

---

## Cross-Modul-Hinweise

- **MaterialShaderLibrary** (`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952`, `:1000-1009`): `resolveFragment`/`resolveShaders` kopieren und hashen den GLSL-Text bei jedem Aufruf. Wird von allen Backends pro DrawCall genutzt (D3D11 `:7009`, GL `:3599`, Metal `:12815`). Ein Hash-Cache im Asset/der Library hilft allen vier Backends (REN-201).
- **RenderSorter** (`src/HE_Rendering/include/HorizonRendering/RenderSorter.h`, `SortKey { meshHi, meshLo, distSq, index }`): kein Material-/PSO-Anteil im Schlüssel; Grundlage für REN-205 und für Zustandswechsel in allen Backends.
- **RenderExtractor/RenderWorld**: Dirty-Flags pro Objekt/Licht (für Transform, `refineWorldBounds`, GI-Instanzen, Shadow-Layer-Caching) existieren nur indirekt über `GIProbeSceneSignature` (`GIProbeGrid.h`). Ein gemeinsames Frame-zu-Frame-Delta (geänderte Entity-IDs) würde REN-203, REN-210 und REN-221 gleichzeitig tragen.
- **D3D11/GL/Metal**: dieselben `glm::inverse(obj.transform)` in `updateGiAccel`-Äquivalenten (`D3D11Renderer.cpp:3184`, `OpenGLRenderer.cpp:5974`, `MetalRenderer.mm:8245`); `BuildClusterLights` je Backend aus `fillPerFrame`-Äquivalenten (`D3D11Renderer.cpp:6332`, `OpenGLRenderer.cpp:11416`, `MetalRenderer.mm:14920`), dort ebenfalls auf Doppelaufruf prüfen; Vulkan nutzt dieselbe feste 10-Pass-Bloom-Kette (`VulkanRenderer.cpp:1000`).
- **D3D_Shared/HlslSources.h**: `kSkyFuncHLSL` wird in `PSMain` dreimal pro Pixel ausgewertet (Ambient diffus, Env-Spec, Nebel); nicht im Fokus dieses Slices, aber ein Kandidat für die Slice, die die Shared-Dateien prüft.

## Unsicher / Messung nötig

- REN-202: Gewinn von DEFAULT- statt UPLOAD-Heap hängt von GPU-Architektur (dGPU vs UMA) ab; mit `GetFrameGpuStats().gpuFrameMs` (Zeile 12891) auf dGPU messen.
- REN-203: Anteil unveränderter Shadow-Layer in realen Szenen (statische Lichter) und tatsächlicher Anteil des Shadow-Passes an der Frame-Zeit.
- REN-205: Kosten von `SetDescriptorHeaps`/Root-Signature-Wechseln sind treiberabhängig (NVIDIA/AMD/Intel); mit PIX-Capture bestätigen.
- REN-208/REN-219: GPU-Zeit der Bloom-Kette bzw. des Sky-Passes pro Auflösung (1080p/1440p/4K).
- SSAO läuft in voller Auflösung (`createSSAOTargets(width, height)` bei `:10817-10818`, Position-Prepass `RGBA16F` plus extra Geometriepass): halbe Auflösung wäre ein typischer Gewinn, ändert aber das Bild (nicht verhaltenserhaltend), daher nur mit Qualitätsentscheidung.
- Sky-Noise-Bake: `HE::BuildSkyNoise3D(256)` (Zeile 2704-2708, nur Release) erzeugt 256³ x 4 B = 64 MiB auf der CPU, dazu ein gleich großer Upload-Buffer und eine 64-MiB-Textur (ca. 192 MiB transient beim Start). Startzeit messen; Offline-Bake oder Compute-Bake wäre Alternative.
- `D3D12CreateDevice(nullptr, ...)` (Zeile 9862) nimmt den Standard-Adapter; auf Hybrid-Laptops kann das die iGPU sein. Ob `EnumAdapterByGpuPreference` gewünscht ist, ist eine Produktentscheidung.
- Swapchain ohne `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` / `SetMaximumFrameLatency` (Zeile 9912-9921): Frame-Latenz hängt vom Treiber ab; nicht geprüft, ob das Pacing-Problem (Kommentar 83-86) damit besser lösbar ist.
- REN-211: ob irgendein Pass zwischen den zwei `fillPerFrame`-Aufrufen die CB braucht, bitte mit Debug-Layer prüfen, bevor die Erstfüllung entfällt.

## Gelesen

Ja: `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp` vollständig von Zeile 1 bis 12 904 in Blöcken (kein Teil übersprungen); zur Absicherung der Cross-Modul-Hinweise zusätzlich punktuell `MaterialShaderLibrary.cpp:941-1009`, `MaterialShaderLibrary.h:222-245`, `RenderSorter.h` (SortKey), `D3D_Shared/HlslSources.h:39-65` und die genannten Parallelstellen in D3D11/GL/Metal/Vulkan per Grep.
